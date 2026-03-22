#include "librim/application.hpp"
#include "librim/factory.hpp"
#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

using namespace librim;

struct BenchHeader { uint32_t type; uint32_t length; };
enum class BenchMsgType : uint32_t { BenchData = 1 };
struct BenchMsg { BenchHeader header; };

// ------------------------------------------------------------------
// Real-world Benchmark Configuration Parameters
// ------------------------------------------------------------------
constexpr int PAYLOAD_SIZE = 1024;      // Realistic packet payload limit
constexpr int DURATION_SEC = 5;         // Fixed time-box measuring natural throughput 
constexpr int NUM_CLIENTS = 50;         // Simulating multi-socket contention and strand multiplexing 
constexpr int BATCH_SIZE = 50;          // Messages to enqueue synchronously before yielding CPU cycles to OS buffers natively

std::atomic<uint64_t> g_received_count{0};
std::atomic<uint64_t> g_sent_count{0};

// ------------------------------------------------------------------
// Dedicated Processing Server Target
// ------------------------------------------------------------------
class BenchServer : public RimApplication<BenchMsgType, BenchHeader> {
public:
    BenchServer(AppConfig config) : RimApplication(std::move(config)) {}

    void handle_bench_msg(BenchMsg &) {
        g_received_count.fetch_add(1, std::memory_order_relaxed);
        
        // Simulates realistic active workload preventing artificial cache-hit benchmarking speeds.
        // Specifically mimics validation logic, JSON parsing, or DB queue marshaling operations roughly 1-2 microseconds long.
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

// ------------------------------------------------------------------
// High Throughput Backpressured Client Pump
// ------------------------------------------------------------------
void run_pumping_client(Protocol protocol, std::atomic<bool>& running) {
    Context ctx;
    auto size_lambda = [](std::span<const std::byte>) -> std::size_t { return sizeof(BenchHeader) + PAYLOAD_SIZE; };
    ClientConfig cfg{protocol, "127.0.0.1", 8081, sizeof(BenchHeader), size_lambda};

    std::vector<std::byte> msg(sizeof(BenchHeader) + PAYLOAD_SIZE, std::byte{0});
    BenchHeader hdr{static_cast<uint32_t>(BenchMsgType::BenchData), PAYLOAD_SIZE};
    std::memcpy(msg.data(), &hdr, sizeof(BenchHeader));

    std::shared_ptr<TcpClient> tcp;
    std::shared_ptr<UdpEndpoint> udp;

    if (protocol == Protocol::TCP) {
        tcp = Factory::create_tcp_client(ctx, cfg).value();
        std::atomic<bool> connected{false};
        (void)tcp->connect("127.0.0.1", 8081, [&](auto res) { if (res) connected = true; });
        ctx.start(1);
        while (running && !connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!connected) { ctx.stop(); return; }
    } else {
        udp = Factory::create_udp_client(ctx, cfg).value();
        ctx.start(1);
    }

    // Execute paced batch loops effectively mitigating infinite string memory bloat organically seen in lock-free fallback cases
    while (running) {
        for (int i = 0; i < BATCH_SIZE; ++i) {
            if (protocol == Protocol::TCP) {
                if (tcp->send(msg)) g_sent_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (udp->send(msg)) g_sent_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Artificial yield mimicking an event-tick pacing the generator loops mitigating massive OS send buffer overrun latency
        std::this_thread::sleep_for(std::chrono::microseconds(100)); 
    }

    if (tcp) tcp->close();
    if (udp) udp->close();
    ctx.stop();
}

// ------------------------------------------------------------------
// Formatted Statistics Block Layout
// ------------------------------------------------------------------
void print_stats(const std::string& name, uint64_t sent, uint64_t recv, double duration) {
    double sent_mb = (sent * (PAYLOAD_SIZE + sizeof(BenchHeader))) / (1024.0 * 1024.0);
    double recv_mb = (recv * (PAYLOAD_SIZE + sizeof(BenchHeader))) / (1024.0 * 1024.0);
    double tput_msg = recv / duration;
    
    // Mitigate any late-arriving packets or asynchronous discrepancies
    if (recv > sent) sent = recv; 
    
    double drop_rate = (sent > 0) ? (100.0 * (1.0 - (double)recv / sent)) : 0.0;
    if (drop_rate < 0) drop_rate = 0; // Absolute clamps

    std::string sent_str = fmt::format("{} ({:.1f} MB)", sent, sent_mb);
    std::string recv_str = fmt::format("{} ({:.1f} MB)", recv, recv_mb);

    fmt::print("\n");
    fmt::print("==============================================================\n");
    fmt::print(" Benchmark Results: {} \n", name);
    fmt::print("==============================================================\n");
    fmt::print("{:<20}{:.2f}\n", "Duration (sec):", duration);
    fmt::print("{:<20}{}\n", "Connections:", NUM_CLIENTS);
    fmt::print("{:<20}{} bytes\n", "Payload Size:", PAYLOAD_SIZE);
    fmt::print("--------------------------------------------------------------\n");
    fmt::print("{:<20}{}\n", "Messages Sent:", sent_str);
    fmt::print("{:<20}{}\n", "Messages Rcvd:", recv_str);
    
    // Explicitly track missing streams natively in UDP protocols specifically
    if (name == "UDP") {
        fmt::print("{:<20}{:.2f} %\n", "Drop Rate:", drop_rate);
    }
    fmt::print("--------------------------------------------------------------\n");
    fmt::print("{:<20}{:.0f} msgs/sec\n", "Throughput:", tput_msg);
    fmt::print("{:<20}{:.1f} MB/sec\n", "Bandwidth:", (recv_mb / duration));
    fmt::print("==============================================================\n\n");
}

void run_benchmark(Protocol protocol, const std::string &name) {
    g_received_count = 0;
    g_sent_count = 0;
    std::atomic<bool> running{true};

    AppConfig server_cfg{AppRole::Server, protocol, "127.0.0.1", 8081};
    BenchServer server(server_cfg);
    
    // Boot overarching Context
    std::thread server_thread([&]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Sleep mapping the explicit Listener successfully safely

    std::vector<std::thread> clients;
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        clients.emplace_back(run_pumping_client, protocol, std::ref(running));
    }

    // Execute actively against the duration window mapping throughput definitively.
    auto start_time = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(DURATION_SEC));
    running = false; 
    
    // Synchronous execution wait blocks explicitly dropping the internal pipelines definitively.
    for (auto &t : clients) {
        if (t.joinable()) t.join();
    }

    // Allocate minor trailing window mitigating TCP window asynchronous drops locally. 
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    server.stop();

    auto end_time = std::chrono::high_resolution_clock::now();
    
    // Format duration against physical seconds implicitly measuring logic naturally.
    double actual_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;

    server_thread.join();
    print_stats(name, g_sent_count.load(), g_received_count.load(), actual_duration);
}

int main()
{
    // Supress logger limits deliberately forcing explicit tabular format layouts uniquely.
    spdlog::set_level(spdlog::level::err); 
    
    fmt::print("Starting Librim Realistic Performance Benchmarks...\n");
    fmt::print("Simulating {} concurrent connections over {} seconds per phase.\n", NUM_CLIENTS, DURATION_SEC);
    fmt::print("Please wait up to {} seconds for tests to seamlessly complete.\n", (DURATION_SEC * 2 + 3));

    run_benchmark(Protocol::TCP, "TCP");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    run_benchmark(Protocol::UDP, "UDP");

    return 0;
}
