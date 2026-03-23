#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <endian.h>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <span>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace librim
{

/**
 * @struct RimHeader
 * @brief Standardized network byte order header injected into the binary log.
 *
 * This struct is explicitly packed to 1 byte to prevent compiler padding
 * and ensure clean byte-exact serialization to disk.
 */
#pragma pack(push, 1)
struct RimHeader
{
    uint32_t record_id;    ///< Numeric identifier denoting the origin or type of the message.
    uint32_t length;       ///< Exact length of the ensuing payload in bytes.
    uint64_t timestamp_ns; ///< High-precision nanosecond timestamp of message arrival.
};
#pragma pack(pop)

/**
 * @class AsyncLogger
 * @brief Thread-safe asynchronous double-buffered binary logger.
 *
 * The AsyncLogger seamlessly decouples fast network I/O strands from the notoriously slow
 * hard-disk sequential write bottlenecks. It employs a Multi-Producer/Single-Consumer
 * double-buffering architecture native to game engine logging.
 */
class AsyncLogger
{
  public:
    /**
     * @brief Constructs an AsyncLogger and spins up its background flusher thread.
     *
     * @param max_capacity Maximum bytes the RAM buffer can hold before entering backpressure block/drop modes.
     */
    explicit AsyncLogger(size_t max_capacity = 10 * 1024 * 1024) : max_capacity_(max_capacity)
    {
        // Thread-safe POSIX time formatting for the generated `.bin` log filename
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm;
        localtime_r(&now_c, &now_tm);
        std::stringstream ss;
        ss << "librim_log_" << std::put_time(&now_tm, "%Y%m%d_%H%M%S") << ".bin";
        std::string filename = ss.str();

        file_.open(filename, std::ios::binary | std::ios::app);
        if (!file_.is_open())
        {
            spdlog::error("[AsyncLogger] Failed to open log file: {}", filename);
            return; // Soft fail; logs will simply discard instead of crashing
        }

        // Pre-allocate 1MB dynamically to prevent resizing thrash on boot
        active_buffer_.reserve(1024 * 1024);
        flush_buffer_.reserve(1024 * 1024);

        // Spin up the background I/O flush worker
        running_.store(true, std::memory_order_seq_cst);
        worker_ = std::thread(&AsyncLogger::process_loop, this);
    }

    /**
     * @brief Gracefully terminates the logging worker and flushes any pending RAM allocations to disk.
     */
    ~AsyncLogger()
    {
        if (running_.exchange(false, std::memory_order_seq_cst))
        {
            cv_.notify_one(); // Wake up worker if it is sleeping on the cond-var
            if (worker_.joinable())
                worker_.join();
        }

        // One final manual flush of any residual bytes remaining in the active vector
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_buffer_.empty() && file_.is_open())
            {
                file_.write(reinterpret_cast<const char *>(active_buffer_.data()), active_buffer_.size());
                file_.flush();
            }
        }

        // Report dropped logs to standard out if backpressure killed data
        if (dropped_logs_.load() > 0)
        {
            spdlog::warn("[AsyncLogger] Dropped {} log(s) due to backpressure threshold.", dropped_logs_.load());
        }
    }

    /**
     * @brief Safely injects a record into the active RAM buffer alongside a generated RimHeader.
     *
     * This method instantly calculates a timestamp, formats a `RimHeader` into Network Byte Order
     * (Big Endian), appends both the header and the raw payload into the generic byte buffer,
     * and signals the background worker to awaken.
     *
     * @param record_id Origin or category ID of this network buffer.
     * @param data The raw contiguous message bytes to log to disk.
     */
    void log(uint32_t record_id, std::span<const std::byte> data)
    {
        if (!running_.load(std::memory_order_relaxed) || data.empty())
            return;

        // Obtain a high-precision nanosecond timestamp
        auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
        uint64_t timestamp_ns = now.time_since_epoch().count();

        // Populate and cleanly Endian-Swap the unified cross-platform header
        RimHeader hdr;
        hdr.record_id = htobe32(record_id);
        hdr.length = htobe32(static_cast<uint32_t>(data.size()));
        hdr.timestamp_ns = htobe64(timestamp_ns);

        std::span<const std::byte> hdr_span(reinterpret_cast<const std::byte *>(&hdr), sizeof(hdr));

        {
            // Lock the active buffer precisely long enough for a contiguous bulk copy
            std::lock_guard<std::mutex> lock(mutex_);

            // Backpressure check: aggressively drop packets instead of blowing up server RAM
            if (active_buffer_.size() + hdr_span.size() + data.size() > max_capacity_)
            {
                dropped_logs_.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Emplace both the newly forged header and the raw stream bytes
            active_buffer_.insert(active_buffer_.end(), hdr_span.begin(), hdr_span.end());
            active_buffer_.insert(active_buffer_.end(), data.begin(), data.end());
        }

        // Nudge the dedicated logger thread that there is work
        cv_.notify_one();
    }

  private:
    /**
     * @brief Background thread routine responsible for interacting with incredibly slow Disk I/O.
     */
    void process_loop()
    {
        while (running_.load(std::memory_order_relaxed))
        {
            {
                // Sleep until there is data in the buffer OR until the destructor instructs us to stop
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock,
                         [this]() { return !active_buffer_.empty() || !running_.load(std::memory_order_relaxed); });

                // Core Double-Buffering mechanic:
                // Instantly steal the active buffer's backing pointer array and give it an empty one.
                // This blocks the network threads for approximately 3 nanoseconds.
                std::swap(active_buffer_, flush_buffer_);
            }

            // At this point we drop the mutex entirely so active_buffer_ is unblocked.
            // Now we can take 15ms+ performing the slow posix write exclusively on flush_buffer_.
            if (!flush_buffer_.empty())
            {
                if (file_.is_open())
                {
                    file_.write(reinterpret_cast<const char *>(flush_buffer_.data()), flush_buffer_.size());
                    file_.flush();
                }

                // Clear the buffer to reset its size to 0, but retain its underlying
                // raw heap capacity to prevent `new`/`delete` thrash!
                flush_buffer_.clear();
            }
        }
    }

    size_t max_capacity_;                  ///< Hard limit of the RAM buffer before dropping incoming data.
    std::atomic<size_t> dropped_logs_{0};  ///< Thread-safe counter of intentionally dropped records.
    std::ofstream file_;                   ///< Binary file output stream.
    std::vector<std::byte> active_buffer_; ///< Buffer receiving raw network inputs immediately.
    std::vector<std::byte> flush_buffer_;  ///< Buffer held by the background thread actively writing to disk.
    std::mutex mutex_;                     ///< Protects the `active_buffer_` memory boundary.
    std::condition_variable cv_;           ///< Signals the worker thread out of sleep state.
    std::atomic<bool> running_{false};     ///< Flag cleanly shutting down the application.
    std::thread worker_;                   ///< The allocated background POSIX thread context.
};

} // namespace librim
