#include "librim/application.hpp"
#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <fmt/color.h>
#include <string>
#include <thread>
#include <vector>

using namespace librim;

struct BenchHeader { uint32_t type; uint32_t length; };
enum class BenchMsgType : uint32_t { BenchData = 1 };
struct BenchMsg { BenchHeader header; };

// ------------------------------------------------------------------
// Global Metrics State
// ------------------------------------------------------------------
std::atomic<uint64_t> g_total_received{0};
std::atomic<uint64_t> g_current_sec_received{0};

// ------------------------------------------------------------------
// Dedicated Processing Server Target
// ------------------------------------------------------------------
class BenchServer : public RimApplication<BenchMsgType, BenchHeader> {
public:
    BenchServer(AppConfig config) : RimApplication(std::move(config)) {}

    void handle_bench_msg(BenchMsg &) {
        g_total_received.fetch_add(1, std::memory_order_relaxed);
        g_current_sec_received.fetch_add(1, std::memory_order_relaxed);
        
        // Simulates realistic active workload preventing artificial cache-hit benchmarking speeds.
        volatile int x = 0;
        for (int i = 0; i < 75; ++i) {
            x += i; 
        }
    }

protected:
    typename MessageDispatcher<BenchMsgType, BenchHeader>::HeaderExtractor get_extractor() override {
        return [](const BenchHeader &h) -> std::pair<BenchMsgType, std::size_t> {
            return {static_cast<BenchMsgType>(h.type), h.length + sizeof(BenchHeader)};
        };
    }
    
    void setup_handlers(MessageDispatcher<BenchMsgType, BenchHeader> &dispatcher) override {
        dispatcher.add_handler(BenchMsgType::BenchData, this, &BenchServer::handle_bench_msg);
    }
};

int main(int argc, char* argv[]) {
    // Parse arguments or default 
    std::string proto_str = (argc > 1) ? argv[1] : "tcp";
    uint16_t port = (argc > 2) ? std::stoul(argv[2]) : 8081;

    Protocol protocol = (proto_str == "udp" || proto_str == "UDP") ? Protocol::UDP : Protocol::TCP;
    
    fmt::print(fmt::fg(fmt::color::cyan) | fmt::emphasis::bold, "=== LIBRIM BENCHMARK SERVER ===\n");
    fmt::print("Protocol: {}\nPort: {}\n", proto_str, port);
    fmt::print("Waiting for incoming client saturation limits...\n--------------------------------------------------------------\n");

    AppConfig server_cfg{AppRole::Server, protocol, "0.0.0.0", port};
    BenchServer server(server_cfg);
    
    std::thread server_thread([&]() { server.run(); });

    // Monitor output metrics endlessly 
    auto last_time = std::chrono::steady_clock::now();
    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count() / 1000.0;
        last_time = now;
        
        // Atomically exchange internal ingestion window counters instantly mitigating data loss
        uint64_t rcvd_sec = g_current_sec_received.exchange(0);
        
        if (rcvd_sec > 0) {
            double rcvd_mb = (rcvd_sec * 1032.0) / (1024.0 * 1024.0); // 1024 + 8 byte headers natively
            double tput_msg = rcvd_sec / dt;
            double bw = rcvd_mb / dt;
            
            fmt::print("Server [{}] | Rcvd Overall: {:>10} | {:>8.0f} msg/sec | {:>6.1f} MB/s\n", 
                       proto_str, g_total_received.load(), tput_msg, bw);
        }
    }

    server_thread.join();
    return 0;
}
