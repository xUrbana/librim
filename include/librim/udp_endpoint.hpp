#pragma once

#include "librim/context.hpp"
#include "librim/expected.hpp"
#include <boost/asio.hpp>
#include <boost/lockfree/queue.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace librim
{

/**
 * @class UdpEndpoint
 * @brief Thread-safe and lock-free Asynchronous UDP Socket wrapper.
 *
 * Facilitates sending UDP datagrams to specific hosts, bonding to local ports, 
 * and subscribing to strictly-IPv4 high-performance Multicast UDP groups. 
 * Employs a custom `boost::lockfree::queue` memory pipeline eliminating `mutex_lock` delays.
 */
class UdpEndpoint : public std::enable_shared_from_this<UdpEndpoint>
{
  public:
    /**
     * @brief Signature for asynchronous reception of individual UDP datagrams.
     * @param data Raw payload span mapped directly off the OS-level ingress buffer.
     * @param sender The resolved remote IP coordinates of the transmitting node.
     */
    using ReceiveCallback = std::function<void(std::span<const std::byte>, const boost::asio::ip::udp::endpoint &)>;

    /**
     * @brief Constructs an inactive UDP interface.
     * @param context Host event loop environment tracking the execution hooks.
     */
    UdpEndpoint(Context &context)
        : context_(context), socket_(context.asio_context()),
          strand_(boost::asio::make_strand(context.asio_context().get_executor()))
    {
    }

    /**
     * @brief Destructor strictly asserting there are zero pointer leaks inside the lock-free data layout schemas.
     */
    ~UdpEndpoint()
    {
        if (socket_.is_open())
        {
            boost::system::error_code ec;
            (void)socket_.close(ec);
        }
        std::vector<std::byte> *ptr;
        while (buffer_pool_.pop(ptr))
        {
            delete ptr;
        }
        for (auto &item : send_queue_)
        {
            delete item.buf;
        }
        send_queue_.clear();
    }

    /**
     * @brief Opens and explicitly binds the physical networking card to listen on a designated port.
     * @param port Target port identifying the service.
     * @return Connection error struct if OS explicitly rejects the binding context.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> bind(uint16_t port)
    {
        boost::system::error_code ec;
        boost::asio::ip::udp::endpoint local_endpoint(boost::asio::ip::udp::v4(), port);

        socket_.open(local_endpoint.protocol(), ec);
        if (ec)
            return librim::unexpected(ec);

        socket_.bind(local_endpoint, ec);
        if (ec)
            return librim::unexpected(ec);

        return {};
    }

    /**
     * @brief Configures a default target address for implicitly addressed `send()` calls.
     *
     * In UDP, "connect" does not establish a handshake! It merely caches the IP routing 
     * metadata deeply inside the active OS socket to eliminate DNS lookups during high-speed blasts.
     *
     * @param host Address/URL mapping (IPv4 natively assumed).
     * @param port Remote receptor destination.
     * @return Errors if the host routing DNS strictly fails to materialize a viable path.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> connect(const std::string &host, uint16_t port)
    {
        boost::system::error_code ec;
        boost::asio::ip::udp::resolver resolver(context_.asio_context());
        auto endpoints = resolver.resolve(boost::asio::ip::udp::v4(), host, std::to_string(port), ec);
        if (ec)
            return librim::unexpected(ec);
        if (endpoints.empty())
            return librim::unexpected(boost::asio::error::host_not_found);

        if (!socket_.is_open())
        {
            socket_.open(boost::asio::ip::udp::v4(), ec);
            if (ec)
                return librim::unexpected(ec);
        }

        socket_.connect(*endpoints.begin(), ec);
        if (ec)
            return librim::unexpected(ec);

        remote_endpoint_ = *endpoints.begin();
        is_connected_ = true;
        return {};
    }

    /**
     * @brief Directs the OS network stack to intercept traffic traversing an IPv4 Multicast IGMP group.
     *
     * @param group_ip Dedicated IP Address (E.g. "239.255.0.1") configured for subscription.
     * @param port Common listener mapping.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> join_multicast(const std::string &group_ip,
                                                                                   uint16_t port)
    {
        boost::system::error_code ec;
        boost::asio::ip::address group_addr = boost::asio::ip::make_address(group_ip, ec);
        if (ec)
            return librim::unexpected(ec);

        // Force IPv4 for strict logical networking requirements
        if (!group_addr.is_v4())
            return librim::unexpected(boost::asio::error::address_family_not_supported);

        boost::asio::ip::udp::endpoint listen_endpoint(boost::asio::ip::address_v4::any(), port);

        if (!socket_.is_open())
        {
            socket_.open(listen_endpoint.protocol(), ec);
            if (ec)
                return librim::unexpected(ec);
        }

        socket_.set_option(boost::asio::ip::udp::socket::reuse_address(true), ec);
        if (ec)
            return librim::unexpected(ec);

        socket_.bind(listen_endpoint, ec);
        if (ec)
            return librim::unexpected(ec);

        // Natively command the OS Kernel to join the localized IGMP routing domain
        socket_.set_option(boost::asio::ip::multicast::join_group(group_addr), ec);
        if (ec)
            return librim::unexpected(ec);

        return {};
    }

    /**
     * @brief Hooks the primary logic handler executed upon fully receiving datagram inputs.
     */
    void on_receive(ReceiveCallback cb)
    {
        receive_callback_ = std::move(cb);
    }

    /**
     * @brief Commences the infinite asynchronous recursive ingestion cycle.
     */
    void start_receive()
    {
        if (!receive_callback_ || !socket_.is_open())
            return;

        socket_.async_receive_from(
            boost::asio::buffer(recv_buffer_), sender_endpoint_,
            boost::asio::bind_executor(strand_, [self = shared_from_this()](boost::system::error_code ec,
                                                                           std::size_t bytes_recvd) {
                if (!ec && bytes_recvd > 0)
                {
                    std::span<const std::byte> data(reinterpret_cast<const std::byte *>(self->recv_buffer_.data()),
                                                    bytes_recvd);
                    self->receive_callback_(data, self->sender_endpoint_);
                    
                    // Recursive ingestion loop chaining
                    self->start_receive();
                }
                else if (ec && ec != boost::asio::error::operation_aborted)
                {
                    spdlog::warn("[UdpEndpoint] Receive error: {}", ec.message());
                    self->start_receive(); // Retry on transient UDP buffer dropout errors
                }
            }));
    }

    /**
     * @brief Broadcasts a contiguous sequence off the network card utilizing the cached `connect()` IP.
     *
     * @param data Byte layout dynamically pushed to the wire.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> send(std::span<const std::byte> data)
    {
        if (!is_connected_)
            return librim::unexpected(boost::asio::error::not_connected);
        return send_to(data, remote_endpoint_);
    }

    /**
     * @brief Broadcasts a completely unique UDP packet strictly aimed at a hardcoded foreign IP Endpoint.
     *
     * @param data Fully initialized user message payload.
     * @param target Specific overriding endpoint trajectory.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> send_to(
        std::span<const std::byte> data, const boost::asio::ip::udp::endpoint &target)
    {
        if (!socket_.is_open())
            return librim::unexpected(boost::asio::error::bad_descriptor);

        std::vector<std::byte> *buf_ptr = nullptr;
        if (!buffer_pool_.pop(buf_ptr))
        {
            // Dynamically allocate when lock-free queues fall behind extreme throughput spikes!
            buf_ptr = new std::vector<std::byte>();
        }
        buf_ptr->assign(data.begin(), data.end());

        // Context-Switch explicitly moving into the Strand boundaries securely
        boost::asio::post(strand_, [self = shared_from_this(), buf_ptr, target]() mutable {
            bool start_write = self->send_queue_.empty();
            self->send_queue_.push_back({buf_ptr, target});
            if (start_write)
            {
                self->do_write();
            }
        });
        return {};
    }

    /**
     * @brief Disassociates the UDP port mappings instantly breaking active routing.
     */
    void close()
    {
        boost::asio::dispatch(strand_, [weak_self = weak_from_this()]() {
            if (auto self = weak_self.lock())
            {
                if (self->socket_.is_open())
                {
                    boost::system::error_code ec;
                    self->socket_.close(ec);
                }
                self->is_connected_ = false;
            }
        });
    }

  private:
    /**
     * @brief Processes queued payloads natively. Fully eliminates synchronous I/O waiting delays!
     */
    void do_write()
    {
        socket_.async_send_to(
            boost::asio::buffer(*send_queue_.front().buf), send_queue_.front().target,
            boost::asio::bind_executor(strand_, [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                // Free parameters actively back into the lock-free state engine
                auto *buf_ptr = self->send_queue_.front().buf;
                self->send_queue_.pop_front();

                if (!self->buffer_pool_.push(buf_ptr))
                {
                    delete buf_ptr;
                }

                if (!ec && !self->send_queue_.empty())
                {
                    self->do_write();
                }
                else if (ec)
                {
                    self->close();
                }
            }));
    }

    Context &context_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;

    std::array<char, 65536> recv_buffer_; ///< Physical limit natively enforced avoiding UDP fragmentation risks.
    boost::asio::ip::udp::endpoint sender_endpoint_;
    boost::asio::ip::udp::endpoint remote_endpoint_;

    bool is_connected_ = false;
    ReceiveCallback receive_callback_;

    struct UdpSendItem
    {
        std::vector<std::byte> *buf;
        boost::asio::ip::udp::endpoint target;
    };

    boost::lockfree::queue<std::vector<std::byte> *> buffer_pool_{1024}; ///< Highly scalable pool boundary
    std::deque<UdpSendItem> send_queue_;
};

} // namespace librim
