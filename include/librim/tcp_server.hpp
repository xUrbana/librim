#pragma once

#include "librim/context.hpp"
#include "librim/expected.hpp"
#include <boost/asio.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/circular_buffer.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_set>
#include <vector>

namespace librim
{

/**
 * @class TcpConnection
 * @brief Represents a single actively connected client on the server.
 *
 * It natively inherits from `std::enable_shared_from_this` to guarantee
 * lifetime safety within ASIO async completion handlers. It provides
 * lock-free writing and continuous header/payload extraction loop logic.
 */
class TcpConnection : public std::enable_shared_from_this<TcpConnection>
{
  public:
    /**
     * @brief Triggered when a complete dynamically-sized payload arrives.
     * @param ptr Smart pointer to this specific connection object.
     * @param data The contiguous bytes cleanly extracted off the raw stream.
     */
    using ReceiveCallback = std::function<void(std::shared_ptr<TcpConnection>, std::span<const std::byte>)>;

    /**
     * @brief Triggered synchronously when the socket drops to clean up application states.
     * @param ptr Safely retained smart pointer referencing the dead connection.
     */
    using DisconnectCallback = std::function<void(std::shared_ptr<TcpConnection>)>;

    /**
     * @brief Computes overarching message dimensions derived entirely from the N-byte header.
     */
    using FramingLambda = std::function<std::size_t(std::span<const std::byte>)>;

    /**
     * @brief Constructs an immutable state for an active socket.
     *
     * @param socket Handed-off physical OS-level socket from the `acceptor`.
     * @param on_recv Routine to invoke on valid payload completions.
     * @param on_disc Routine to invoke instantly upon abrupt or clean termination.
     * @param header_size Baseline static prefix required to frame network boundaries.
     * @param get_total_size Parsing lambda translating the header into total byte dimensions.
     * @param max_message_size Safely drops overly-complex/malicious inputs.
     * @param read_timeout Limits idle time before connection termination.
     */
    TcpConnection(boost::asio::ip::tcp::socket socket, ReceiveCallback on_recv, DisconnectCallback on_disc,
                  std::size_t header_size, FramingLambda get_total_size, std::size_t max_message_size,
                  std::chrono::milliseconds read_timeout = std::chrono::milliseconds(0))
        : socket_(std::move(socket)), strand_(boost::asio::make_strand(socket_.get_executor())),
          on_recv_(std::move(on_recv)), on_disc_(std::move(on_disc)), header_size_(header_size),
          max_message_size_(max_message_size), get_total_size_(std::move(get_total_size)), read_timeout_(read_timeout),
          read_timer_(strand_)
    {
        // Mass allocation to easily fit vast majorities of packet sizes natively
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
     * @brief Tears down resources securely guaranteeing no lock-free pool leaks.
     */
    ~TcpConnection()
    {
        std::vector<std::byte> *ptr;
        while (buffer_pool_.pop(ptr))
        {
            delete ptr;
        }
        for (auto *p : send_queue_)
        {
            delete p;
        }
    }

    /**
     * @brief Triggers the actual async ingestion cycle for this connection.
     */
    void start()
    {
        reset_read_timeout();
        start_receive();
    }

    /**
     * @brief Highly optimized MPSC lock-free data transmission injection.
     *
     * @param data Logical network buffer to push out the wire via ASIO `async_write`.
     * @return Disconnection flags if socket was found totally closed prior to dispatch.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> send(std::span<const std::byte> data)
    {
        if (!socket_.is_open())
            return librim::unexpected(boost::asio::error::not_connected);

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

        // Subordinate the active socket processing block to the Strand thread context
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
     * @brief Forces an immediate severing of the physical OSI link layer.
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
                    self->read_timer_.cancel(ec); // Flush waiting timer async hooks
                    if (self->on_disc_)
                    {
                        self->on_disc_(self);
                    }
                }
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
                if (on_recv_)
                {
                    std::span<const std::byte> full_message(
                        reinterpret_cast<const std::byte *>(recv_buffer_.data() + current_offset), total_size);
                    on_recv_(shared_from_this(), full_message);
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
        start_receive(); // Jump repeatedly natively!
    }

    /**
     * @brief Sets up an overriding clock tracking stalling inputs.
     */
    void reset_read_timeout()
    {
        if (read_timeout_.count() > 0)
        {
            read_timer_.expires_after(read_timeout_);
            read_timer_.async_wait(
                boost::asio::bind_executor(strand_, [self = shared_from_this()](boost::system::error_code ec) {
                    if (!ec)
                    {
                        self->close();
                    }
                }));
        }
    }

    boost::asio::ip::tcp::socket socket_;                      ///< Live OS file descriptor connection
    boost::asio::strand<boost::asio::any_io_executor> strand_; ///< Core synchronization tool
    std::vector<char> recv_buffer_;                            ///< Dynamically sized contiguous receiving sequence

    ReceiveCallback on_recv_;    ///< Forwarding hook
    DisconnectCallback on_disc_; ///< State teardown hook

    std::size_t header_size_;      ///< Raw prefix dimension
    std::size_t max_message_size_; ///< Buffer constraint protection
    FramingLambda get_total_size_; ///< Dynamic framing logic

    std::chrono::milliseconds read_timeout_; ///< Max allowed dead network limits
    boost::asio::steady_timer read_timer_;   ///< Bound passive time limit watcher
    std::size_t active_bytes_{0};            ///< Continuous sliding buffer capacity

    boost::lockfree::queue<std::vector<std::byte> *> buffer_pool_{8192}; ///< Statically populated pool boundary
    boost::circular_buffer<std::vector<std::byte> *> send_queue_{8192};  ///< Massively optimized contiguous routing layout 
    std::vector<boost::asio::const_buffer> write_buffers_;               ///< Scatter-gather pipeline structs
    std::size_t active_writes_{0};                                       ///< Bounding native metrics trackers
};

/**
 * @class TcpServer
 * @brief Scalable asynchronous TCP acceptor node handling massive concurrency.
 *
 * Utilizes `std::unordered_set` and an active `boost::asio::ip::tcp::acceptor`
 * to physically track all dynamically loaded `TcpConnection` endpoints seamlessly.
 */
class TcpServer
{
  public:
    /**
     * @brief Handler for successfully negotiated connections executing entirely before payload pipelines start.
     */
    using ConnectCallback = std::function<void(std::shared_ptr<TcpConnection>)>;

    /**
     * @brief Preconfigures the system boundary to start actively generating connections.
     *
     * @param context Host worker pipeline structure.
     * @param port Target localhost IP port entrypoint.
     * @param header_size Protocol framing header constant size limit.
     * @param get_total_size Parsing handler generating limits from prefixes.
     * @param max_message_size Limit dropping bad allocations securely.
     * @param read_timeout Client disconnection timer limits inherently managing active sockets.
     */
    TcpServer(Context &context, uint16_t port, std::size_t header_size, TcpConnection::FramingLambda get_total_size,
              std::size_t max_message_size, std::chrono::milliseconds read_timeout = std::chrono::milliseconds(0))
        : acceptor_(context.asio_context(), boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
          header_size_(header_size), max_message_size_(max_message_size), get_total_size_(std::move(get_total_size)),
          read_timeout_(read_timeout)
    {
    }

    /**
     * @brief Registers the boot hook for tracking newly materialized network streams.
     */
    void on_connect(ConnectCallback cb)
    {
        on_connect_ = std::move(cb);
    }

    /**
     * @brief Registers the overarching receiver hooking internally passed downwards inherently across child nodes.
     */
    void on_receive(TcpConnection::ReceiveCallback cb)
    {
        on_receive_ = std::move(cb);
    }

    /**
     * @brief Starts the listening interface pipeline dynamically generating internal streams implicitly.
     */
    [[nodiscard]] librim::expected<void, boost::system::error_code> start()
    {
        if (!acceptor_.is_open())
        {
            return librim::unexpected(boost::system::error_code(boost::asio::error::bad_descriptor));
        }

        // Hook infinite `async_accept` loop chain immediately!
        do_accept();
        return {};
    }

  private:
    /**
     * @brief Infinite non-blocking pipeline looping on its own asynchronous results endlessly.
     */
    void do_accept()
    {
        acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            if (!ec)
            {
                // Core connection setup sequence explicitly piping parameters natively
                auto conn = std::make_shared<TcpConnection>(
                    std::move(socket), on_receive_,
                    [this](std::shared_ptr<TcpConnection> c) {
                        // Connection teardown handler explicitly clearing the tracking set gracefully
                        std::lock_guard<std::mutex> lock(connections_mutex_);
                        connections_.erase(c);
                    },
                    header_size_, get_total_size_, max_message_size_, read_timeout_);

                {
                    std::lock_guard<std::mutex> lock(connections_mutex_);
                    connections_.insert(conn);
                }

                if (on_connect_)
                {
                    on_connect_(conn);
                }

                // Bootstrap the instantiated socket explicitly hooking internal `async_read` loops
                conn->start();
            }

            // Loop natively!
            if (acceptor_.is_open())
            {
                do_accept();
            }
        });
    }

    boost::asio::ip::tcp::acceptor acceptor_;   ///< Bound OS-level socket passively routing inputs
    ConnectCallback on_connect_;                ///< Triggers synchronously to notify application
    TcpConnection::ReceiveCallback on_receive_; ///< Direct feed hook

    std::mutex connections_mutex_;                                   ///< Exclusively guards `connections_` access
    std::unordered_set<std::shared_ptr<TcpConnection>> connections_; ///< Robust live tracking

    std::size_t header_size_;                     ///< Configured pipeline standard size limits
    std::size_t max_message_size_;                ///< Buffer constraints logically defined
    TcpConnection::FramingLambda get_total_size_; ///< Protocol logical translation hooks
    std::chrono::milliseconds read_timeout_;      ///< Explicit socket abort timing limits
};

} // namespace librim
