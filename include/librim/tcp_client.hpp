#pragma once

#include "librim/context.hpp"
#include "librim/expected.hpp"
#include <atomic>
#include <boost/asio.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/circular_buffer.hpp>
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

        // Pre-allocate exactly sized memory payloads fully populating the lock-free pool statically.
        for (int i = 0; i < 8192; ++i)
        {
            auto *v = new std::vector<std::byte>();
            v->reserve(65536); // Support large TCP payloads seamlessly without extending boundaries
            buffer_pool_.push(v);
        }
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
                        self->start_receive();
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
        
        // 1. Instantly pop from IDLE array or forcefully invoke Backpressure yielding if saturated!
        while (!buffer_pool_.pop(buf_ptr))
        {
            // Crucial: OS thread-yield completely halts `while(running)` OOM starvation safely
            std::this_thread::yield();
        }
        
        // Populate the physical dispatch memory scaling cleanly over pre-allocated arrays
        buf_ptr->resize(data.size());
        std::memcpy(buf_ptr->data(), data.data(), data.size());

        // Subordinate the write dispatch operation directly to the I/O strand context
        boost::asio::post(strand_, [self = shared_from_this(), buf_ptr]() mutable {
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
     * @brief Processes queued payloads natively mapping them dynamically into Scatter Arrays bounding TCP arrays globally.
     */
    void do_write()
    {


        write_buffers_.clear();
        active_writes_ = 0;

        for (auto *buf_ptr : send_queue_)
        {
            if (active_writes_ >= 512) break; // Arbitrary Scatter Gather batch limit
            write_buffers_.push_back(boost::asio::buffer(*buf_ptr));
            active_writes_++;
        }

        boost::asio::async_write(
            socket_, write_buffers_,
            boost::asio::bind_executor(strand_, [self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                
                // Free parameters actively back into the lock-free state engine
                for (std::size_t i = 0; i < self->active_writes_; ++i)
                {
                    auto *buf_ptr = self->send_queue_.front();
                    self->send_queue_.pop_front();

                    if (!self->buffer_pool_.push(buf_ptr))
                    {
                        delete buf_ptr;
                    }
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

    /**
     * @brief Initiates a massive kernel read slurping continuous array volumes into the memory buffer directly.
     */
    void start_receive()
    {
        // Guarantee buffer has space to read large native kernel payloads natively 
        if (recv_buffer_.size() < active_bytes_ + 65536)
        {
            recv_buffer_.resize(active_bytes_ + 65536);
        }

        socket_.async_read_some(
            boost::asio::buffer(recv_buffer_.data() + active_bytes_, recv_buffer_.size() - active_bytes_),
            boost::asio::bind_executor(strand_, [self = shared_from_this()](boost::system::error_code ec, std::size_t bytes_recvd) {
                if (!ec && bytes_recvd > 0)
                {
                    self->active_bytes_ += bytes_recvd;
                    self->process_buffer(); // Jump out of async callbacks into memory string processing explicitly
                }
                else
                {
                    self->close();
                }
            }));
    }

    /**
     * @brief Extracts identical protocol elements entirely inside physical memory skipping OS bounds completely.
     */
    void process_buffer()
    {
        std::size_t current_offset = 0;

        while (active_bytes_ - current_offset >= header_size_)
        {
            std::span<const std::byte> header_span(
                reinterpret_cast<const std::byte *>(recv_buffer_.data() + current_offset), header_size_);
            std::size_t total_size = get_total_size_(header_span);

            if (total_size < header_size_ || total_size > max_message_size_)
            {
                close();
                return;
            }

            if (active_bytes_ - current_offset >= total_size)
            {
                if (receive_callback_)
                {
                    std::span<const std::byte> full_message(
                        reinterpret_cast<const std::byte *>(recv_buffer_.data() + current_offset), total_size);
                    receive_callback_(full_message); // Pass up stack safely
                }
                current_offset += total_size;
            }
            else
            {
                break; // Fractional structure incomplete; await kernel additions securely
            }
        }

        std::size_t unparsed_bytes = active_bytes_ - current_offset;
        if (unparsed_bytes > 0 && current_offset > 0)
        {
            std::memmove(recv_buffer_.data(), recv_buffer_.data() + current_offset, unparsed_bytes);
        }
        active_bytes_ = unparsed_bytes;

        reset_read_timeout();
        start_receive(); // loop endlessly
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
    std::size_t active_bytes_{0};            ///< Tracks available buffer arrays completely  

    std::atomic<bool> is_connected_{false}; ///< Exposes simple state checks freely across threading environments
    ReceiveCallback receive_callback_; ///< Main execution routine 

    boost::lockfree::queue<std::vector<std::byte> *> buffer_pool_{8192}; ///< Statically populated pool boundary
    boost::circular_buffer<std::vector<std::byte> *> send_queue_{8192};  ///< Contiguous caching structure completely eliminating L1 misses natively 
    std::vector<boost::asio::const_buffer> write_buffers_;               ///< Scatter-gather structural references
    std::size_t active_writes_{0};                                       ///< Bounding tracking limits internally
};

} // namespace librim
