#pragma once

#include "librim/context.hpp"
#include "librim/expected.hpp"
#include <boost/asio.hpp>
#include <boost/lockfree/queue.hpp>
#include <deque>
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
        start_receive_header();
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

        // 1. Pop an idle buffer block from our zero-lock MPSC queue
        if (!buffer_pool_.pop(buf_ptr))
        {
            // 2. Pool empty/congested: fallback into explicit dynamic allocation
            buf_ptr = new std::vector<std::byte>();
        }
        buf_ptr->assign(data.begin(), data.end());

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
     * @brief Contiguous recursive loop blasting elements into the network card.
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

                auto *buf_ptr = self->send_queue_.front();
                self->send_queue_.pop_front();

                // Deposit memory allocations securely back to wait limits
                if (!self->buffer_pool_.push(buf_ptr))
                {
                    delete buf_ptr; // Physical drop
                }

                if (!self->send_queue_.empty())
                {
                    self->do_write(); // Recursion guarantees sequence order
                }
            }));
    }

    /**
     * @brief Primary entryhook parsing `header_size_` inputs constantly off the IP socket.
     */
    void start_receive_header()
    {
        if (recv_buffer_.size() < header_size_)
        {
            recv_buffer_.resize(header_size_);
        }

        boost::asio::async_read(
            socket_, boost::asio::buffer(recv_buffer_.data(), header_size_),
            boost::asio::bind_executor(
                strand_, [self = shared_from_this()](boost::system::error_code ec, std::size_t bytes_recvd) {
                    if (!ec && bytes_recvd == self->header_size_)
                    {
                        std::span<const std::byte> header_span(
                            reinterpret_cast<const std::byte *>(self->recv_buffer_.data()), self->header_size_);
                        std::size_t total_size = self->get_total_size_(header_span);

                        // Protect against oversized inputs potentially stalling memory
                        if (total_size < self->header_size_ || total_size > self->max_message_size_)
                        {
                            self->close();
                            return;
                        }

                        if (self->recv_buffer_.size() < total_size)
                        {
                            self->recv_buffer_.resize(total_size);
                        }
                        self->start_receive_payload(total_size);
                    }
                    else
                    {
                        self->close();
                    }
                }));
    }

    /**
     * @brief Secondary entryhook slurping remaining payload bytes once boundary length is calculated natively.
     */
    void start_receive_payload(std::size_t total_size)
    {
        std::size_t payload_size = total_size - header_size_;
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
     * @brief Safely propagates fully materialized payloads back out to main logical code contexts.
     */
    void handle_full_message(std::size_t total_size)
    {
        if (on_recv_)
        {
            std::span<const std::byte> full_message(reinterpret_cast<const std::byte *>(recv_buffer_.data()),
                                                    total_size);
            on_recv_(shared_from_this(), full_message); // Pass up stack safely
        }

        reset_read_timeout();
        start_receive_header(); // loop endlessly
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

    boost::lockfree::queue<std::vector<std::byte> *> buffer_pool_{1024}; ///< Pool recycling generic arrays
    std::deque<std::vector<std::byte> *> send_queue_;                    ///< Tracking queued `write` orders
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
