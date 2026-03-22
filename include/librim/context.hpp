#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace librim
{

/**
 * @class Context
 * @brief Manages the Boost::ASIO I/O context and the underlying worker thread pool.
 *
 * The Context class is responsible for initializing the `io_context` and binding an 
 * `executor_work_guard` to it, ensuring that the event loop does not exit prematurely 
 * if there is momentarily no pending work.
 */
class Context
{
  public:
    /**
     * @brief Constructs a new Context and sets up the work guard.
     */
    Context() : work_guard_(boost::asio::make_work_guard(io_context_))
    {
    }

    /**
     * @brief Destructor that ensures all worker threads and pending I/O operations are cleanly stopped.
     */
    ~Context()
    {
        stop();
    }

    /**
     * @brief Spawns worker threads to process ASIO events concurrently.
     *
     * @param num_threads The number of native OS threads to allocate to the event loop pool (default: 1).
     */
    void start(std::size_t num_threads = 1)
    {
        if (!threads_.empty())
        {
            return; // Context already started
        }
        
        // Spawn the requested number of worker threads to service the ASIO run loop.
        for (std::size_t i = 0; i < num_threads; ++i)
        {
            threads_.emplace_back([this]() { io_context_.run(); });
        }
    }

    /**
     * @brief Safely shuts down the context by releasing the work guard and joining all active threads.
     *
     * This forces the event loop to exit once current operations resolve.
     */
    void stop()
    {
        // Drop the work guard so `io_context_.run()` can naturally exit once queues are empty
        work_guard_.reset();
        
        // Hard-stop the context, cancelling pending async operations
        io_context_.stop();
        
        // Wait for all worker threads to safely terminate
        for (auto &t : threads_)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        threads_.clear();
    }

    /**
     * @brief Provides underlying access to the internal ASIO context.
     *
     * @return Reference to the internally managed boost::asio::io_context.
     */
    boost::asio::io_context &asio_context()
    {
        return io_context_;
    }

  private:
    boost::asio::io_context io_context_; ///< The core ASIO event loop.
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_; ///< Prevents premature exit.
    std::vector<std::thread> threads_; ///< Pool of native OS threads executing `io_context_.run()`.
};

} // namespace librim
