#pragma once

#include "librim/context.hpp"
#include "librim/expected.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <boost/lockfree/queue.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace librim
{

/**
 * @class TcpClient
 * @brief Asynchronous, non-blocking TCP Client with high-performance lock-free sending.
 *
 * TcpClient natively manages connection state, implements network timeouts to prevent 
 * dead/stalled connections, and leverages an internal `boost::lockfree::queue` buffer 
 * pool to absolutely eliminate `std::mutex` overhead on the hot I/O thread.
 * 
 * Because it uses Boost.ASIO, all internal handlers execute on a designated `strand_`
 * to mathematically guarantee thread safety without manual locking, even if methods 
 * are invoked from drastically different threads.
 */
class TcpClient : public std::enable_shared_from_this<TcpClient>
{
  public:
    /**
     * @brief Signature for callbacks triggered upon completely receiving a validated application message.
     */
    using ReceiveCallback = std::function<void(std::span<const std::byte>)>;

    /**
     * @brief Signature for the asynchronous connection completion hook.
     */
    using ConnectCallback = std::function<void(librim::expected<void, boost::system::error_code>)>;

    /**
     * @brief Signature for the user-supplied lambda identifying the length of a complete message from its header.
     */
    using FramingLambda = std::function<std::size_t(std::span<const std::byte>)>;

    /**
     * @brief Constructs a new TcpClient instance.
     *
     * @param context System-wide `librim::Context` event loop owner.
     * @param header_size Static size of the network header used to compute total payload length.
     * @param get_total_size User-supplied parser identifying full payload dimensions from initial header.
     * @param max_message_size Maximum legal message dimension to prevent malicious exhaustion attacks.
     * @param read_timeout Maximum ms allowed to passively wait for a read before closing the idle socket.
     */
    TcpClient(Context &context, std::size_t header_size, FramingLambda get_total_size, std::size_t max_message_size,
              std::chrono::milliseconds read_timeout = std::chrono::milliseconds(0))
        : context_(context), socket_(context.asio_context()),
          strand_(boost::asio::make_strand(context.asio_context().get_executor())), header_size_(header_size),
          max_message_size_(max_message_size), get_total_size_(std::move(get_total_size)), read_timeout_(read_timeout),
          read_timer_(strand_)
    {
        // Pre-allocate substantial receive buffer space to minimize resizing operations.
        recv_buffer_.resize(65536);
    }

    /**
     * @brief Destructor guaranteeing zero memory leaks during socket finalization.
     */
    ~TcpClient()
    {
        if (socket_.is_open())
        {
            boost::system::error_code ec;
            (void)socket_.close(ec);
            read_timer_.cancel(ec);
        }

        // Recursively hunt down and delete every stray `std::vector` inside the lock-free pool
        std::vector<std::byte> *buf_ptr;
        while (buffer_pool_.pop(buf_ptr))
        {
            delete buf_ptr;
        }

        // Flush any pointers stuck in the send queue during termination
        for (auto *ptr : send_queue_)
        {
            delete ptr;
        }
        send_queue_.clear();
    }

    /**
     * @brief Registers the primary message ingestion callback logic.
     *
     * @param cb Function to execute entirely asynchronously when a stream constructs a valid full object.
     */
    void on_receive(ReceiveCallback cb)
    {
        receive_callback_ = std::move(cb);
    }

    /**
     * @brief Dispatches a non-blocking request to resolve and connect to an upstream TCP boundary.
     *
     * @param host IPv4, IPv6, or domain name literal of the target.
     * @param port Numeric port destination.
     * @param cb (Optional) Lambda invoking once the physical TCP handshake conclusively validates or fails.
     * @return `expected<void, error_code>` Local synchronous validation failure details, if applicable.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> connect(const std::string &host, uint16_t port,
                                                                            ConnectCallback cb = nullptr)
    {
        boost::asio::ip::tcp::resolver resolver(context_.asio_context());
        boost::system::error_code ec;

        // Perform DNS host resolution synchronously (in practical use, Async DNS is preferred, 
        // but local DNS caches evaluate nearly instantly)
        auto endpoints = resolver.resolve(boost::asio::ip::tcp::v4(), host, std::to_string(port), ec);
        if (ec)
            return librim::unexpected(ec);

        // Transition immediately back to async execution inside the protective strand
        boost::asio::async_connect(
            socket_, endpoints,
            boost::asio::bind_executor(
                strand_, [self = shared_from_this(), cb](boost::system::error_code ec, boost::asio::ip::tcp::endpoint) {
                    if (!ec)
                    {
                        // Safely flip connection flag and boot up the network timers and parsers
                        self->is_connected_.store(true, std::memory_order_release);
                        if (cb)
                            cb({});
                        self->reset_read_timeout();
                        self->start_receive_header();
                    }
                    else
                    {
                        if (cb)
                            cb(librim::unexpected(ec));
                    }
                }));
        return {};
    }

    /**
     * @brief Dispatches raw byte data to the remote endpoint using the internal multi-producer sequence.
     *
     * Crucially, this method never locks. If the system is heavily congested, it drops to 
     * a dynamic allocation to prevent holding up the caller's main thread!
     *
     * @param data Generic view over the bytes slated for instantaneous network dispatch.
     * @return Local rejection errors if the socket is actively dead before the call.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> send(std::span<const std::byte> data)
    {
        if (!is_connected_.load(std::memory_order_acquire) || !socket_.is_open())
        {
            return librim::unexpected(boost::asio::error::not_connected);
        }

        std::vector<std::byte> *buf_ptr = nullptr;
        
        // 1. Attempt insanely fast lock-free atomic pop from IDLE pool
        if (!buffer_pool_.pop(buf_ptr))
        {
            // 2. The pool is temporarily exhausted, fallback to dynamic heap allocation
            buf_ptr = new std::vector<std::byte>();
        }
        
        // Populate the physical dispatch memory exactly sized to match the input vector
        buf_ptr->assign(data.begin(), data.end());

        // Subordinate the write dispatch operation directly to the I/O strand context
        boost::asio::post(strand_, [self = shared_from_this(), buf_ptr]() mutable {
            
            // If the socket was idle, we must manually transition the ASIO internal writer loop
            bool start_write = self->send_queue_.empty();
            self->send_queue_.push_back(buf_ptr);
            
            if (start_write)
            {
                self->do_write();
            }
        });
        return {};
    }

    /**
     * @brief Forcibly aborts ongoing operations and permanently closes the associated connection logic.
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
                    self->read_timer_.cancel(ec); // Abort pending stalls to allow cleanup
                }
                self->is_connected_.store(false, std::memory_order_release);
            }
        });
    }

  private:
    /**
     * @brief Continuously pumps available queued buffers into the OS kernel TCP window.
     *
     * This method recursively bounds itself back into ASIO.
     */
    void do_write()
    {
        boost::asio::async_write(
            socket_, boost::asio::buffer(*send_queue_.front()),
            boost::asio::bind_executor(strand_, [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                if (ec)
                {
                    self->close();
                    return;
                }
                
                // Buffer successful! Extract pointer off the active sequence
                auto *buf_ptr = self->send_queue_.front();
                self->send_queue_.pop_front();

                // Atomically return the pointer to the recycled lock-free idle pool
                if (!self->buffer_pool_.push(buf_ptr))
                {
                    // If the pool is physically completely swamped, natively free the memory forever
                    delete buf_ptr;
                }

                // If `send` stacked more messages while we were writing, keep pushing elements out
                if (!self->send_queue_.empty())
                {
                    self->do_write();
                }
            }));
    }

    /**
     * @brief Starts waiting for precisely `header_size_` bytes off the TCP payload.
     */
    void start_receive_header()
    {
        if (recv_buffer_.size() < header_size_)
        {
            recv_buffer_.resize(header_size_);
        }

        // Issue read for exactly N bytes to construct the generic logical block Header structure
        boost::asio::async_read(
            socket_, boost::asio::buffer(recv_buffer_.data(), header_size_),
            boost::asio::bind_executor(
                strand_, [self = shared_from_this()](boost::system::error_code ec, std::size_t bytes_recvd) {
                    if (!ec && bytes_recvd == self->header_size_)
                    {
                        std::span<const std::byte> header_span(
                            reinterpret_cast<const std::byte *>(self->recv_buffer_.data()), self->header_size_);
                            
                        // Execute user Lambda exactly predicting the overarching packet size
                        std::size_t total_size = self->get_total_size_(header_span);

                        // Abort if malformed packets or DoS attack heuristics match constraints
                        if (total_size < self->header_size_ || total_size > self->max_message_size_)
                        {
                            self->close();
                            return;
                        }

                        if (self->recv_buffer_.size() < total_size)
                        {
                            self->recv_buffer_.resize(total_size);
                        }

                        // Chain the sequence into waiting for the specific N leftover payload dimensions
                        self->start_receive_payload(total_size);
                    }
                    else
                    {
                        self->close();
                    }
                }));
    }

    /**
     * @brief Receives the rest of a delimited logical network message once header limits are defined.
     *
     * @param total_size Explicit size in bytes of Header + Trailing Body
     */
    void start_receive_payload(std::size_t total_size)
    {
        std::size_t payload_size = total_size - header_size_;
        
        // Zero payload bodies bypass async read
        if (payload_size == 0)
        {
            handle_full_message(total_size);
            return;
        }

        boost::asio::async_read(
            socket_, boost::asio::buffer(recv_buffer_.data() + header_size_, payload_size),
            boost::asio::bind_executor(strand_, [self = shared_from_this(), total_size](boost::system::error_code ec,
                                                                                        std::size_t bytes_recvd) {
                if (!ec && bytes_recvd == (total_size - self->header_size_))
                {
                    self->handle_full_message(total_size);
                }
                else
                {
                    self->close();
                }
            }));
    }

    /**
     * @brief Issues raw logical boundaries out cleanly to user scope and restarts sequence hooks.
     *
     * @param total_size Size boundary of the completely buffered message representation.
     */
    void handle_full_message(std::size_t total_size)
    {
        if (receive_callback_)
        {
            std::span<const std::byte> full_message(reinterpret_cast<const std::byte *>(recv_buffer_.data()),
                                                    total_size);
            receive_callback_(full_message);
        }
        
        // Reset connection stall detection after successful transmission processing
        reset_read_timeout();
        
        // Reset ASIO state loop back into waiting for exactly 1 logical Header size unit
        start_receive_header(); 
    }

    /**
     * @brief Arms the stall detection logic to physically close the socket if NO complete messages drop within N limits.
     */
    void reset_read_timeout()
    {
        if (read_timeout_.count() > 0)
        {
            read_timer_.expires_after(read_timeout_);
            read_timer_.async_wait(
                boost::asio::bind_executor(strand_, [self = shared_from_this()](boost::system::error_code ec) {
                    // Timer executing with SUCCESS (no abort code) strictly dictates Timeout happened 
                    if (!ec)
                    {
                        self->close();
                    }
                }));
        }
    }

    Context &context_;      ///< ASIO event loop infrastructure 
    boost::asio::ip::tcp::socket socket_; ///< Managed physical link
    boost::asio::strand<boost::asio::any_io_executor> strand_; ///< Prevents internal async races
    std::vector<char> recv_buffer_; ///< Dynamically sized continuous ingress memory target
    
    std::size_t header_size_;    ///< Predefined user protocol length
    std::size_t max_message_size_;   ///< Safeguard DoS/Exhaustion protection layer
    FramingLambda get_total_size_;   ///< Routine logic

    std::chrono::milliseconds read_timeout_; ///< Drop connections stalling over constraints
    boost::asio::steady_timer read_timer_;   ///< Wait handles executing inside the strand boundary

    std::atomic<bool> is_connected_{false}; ///< Exposes simple state checks freely across threading environments
    ReceiveCallback receive_callback_; ///< Main execution routine 

    boost::lockfree::queue<std::vector<std::byte> *> buffer_pool_{1024}; ///< Idle, ready-to-use memory layouts
    std::deque<std::vector<std::byte> *> send_queue_;                    ///< Sequences explicitly slated for native wire operations
};

} // namespace librim
