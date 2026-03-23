#include "librim/application.hpp"
#include "librim/factory.hpp"
#include <atomic>
#include <chrono>
#include <fmt/color.h>
#include <fmt/core.h>
#include <string>
#include <thread>
#include <vector>

using namespace librim;

struct BenchHeader
{
    uint32_t type;
    uint32_t length;
};
enum class BenchMsgType : uint32_t
{
    BenchData = 1
};

constexpr int PAYLOAD_SIZE = 1024;
constexpr int BATCH_SIZE = 50;

std::atomic<uint64_t> g_sent_count{0};

void run_pumping_client(Protocol protocol, const std::string &ip, uint16_t port, std::atomic<bool> &running)
{
    Context ctx;
    auto size_lambda = [](std::span<const std::byte>) -> std::size_t { return sizeof(BenchHeader) + PAYLOAD_SIZE; };
    ClientConfig cfg{protocol, ip, port, sizeof(BenchHeader), size_lambda};

    std::vector<std::byte> msg(sizeof(BenchHeader) + PAYLOAD_SIZE, std::byte{0});
    BenchHeader hdr{static_cast<uint32_t>(BenchMsgType::BenchData), PAYLOAD_SIZE};
    std::memcpy(msg.data(), &hdr, sizeof(BenchHeader));

    std::shared_ptr<TcpClient> tcp;
    std::shared_ptr<UdpEndpoint> udp;

    if (protocol == Protocol::TCP)
    {
        tcp = Factory::create_tcp_client(ctx, cfg).value();
        std::atomic<bool> connected{false};
        (void)tcp->connect(ip, port, [&](auto res) {
            if (res)
                connected = true;
        });
        ctx.start(1);
        while (running && !connected)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!connected)
        {
            ctx.stop();
            return;
        }
    }
    else
    {
        udp = Factory::create_udp_client(ctx, cfg).value();
        ctx.start(1);
    }

    // Execute paced batch loops effectively mitigating infinite string memory bloat organically
    while (running)
    {
        for (int i = 0; i < BATCH_SIZE; ++i)
        {
            if (protocol == Protocol::TCP)
            {
                if (tcp->send(msg))
                    g_sent_count.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                if (udp->send(msg))
                    g_sent_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    if (tcp)
        tcp->close();
    if (udp)
        udp->close();
    ctx.stop();
}

int main(int argc, char *argv[])
{
    // Parse arguments or default
    std::string proto_str = (argc > 1) ? argv[1] : "tcp";
    std::string ip = (argc > 2) ? argv[2] : "127.0.0.1";
    uint16_t port = (argc > 3) ? std::stoul(argv[3]) : 8081;
    int duration_sec = (argc > 4) ? std::stoi(argv[4]) : 5;
    int num_clients = (argc > 5) ? std::stoi(argv[5]) : 50;

    Protocol protocol = (proto_str == "udp" || proto_str == "UDP") ? Protocol::UDP : Protocol::TCP;

    fmt::print(fmt::fg(fmt::color::green) | fmt::emphasis::bold, "=== LIBRIM BENCHMARK CLIENT ===\n");
    fmt::print("Protocol: {} | Host: {}:{}\nDuration: {}s | Clients: "
               "{}\n--------------------------------------------------------------\n",
               proto_str, ip, port, duration_sec, num_clients);

    g_sent_count = 0;
    std::atomic<bool> running{true};
    std::vector<std::thread> clients;

    for (int i = 0; i < num_clients; ++i)
    {
        clients.emplace_back(run_pumping_client, protocol, ip, port, std::ref(running));
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < duration_sec; ++i)
    {
        fmt::print("Running test... {} seconds remaining\n", duration_sec - i);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    running = false;

    // Synchronous execution wait blocks explicitly dropping the internal pipelines definitively.
    for (auto &t : clients)
    {
        if (t.joinable())
            t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double actual_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;

    double sent_mb = (g_sent_count.load() * (PAYLOAD_SIZE + sizeof(BenchHeader))) / (1024.0 * 1024.0);

    fmt::print("\n==============================================================\n");
    fmt::print("{:<20}{:.2f} seconds\n", "Actual Duration:", actual_duration);
    fmt::print("{:<20}{} (1024b payload)\n", "Messages Sent:", g_sent_count.load());
    fmt::print("{:<20}{:.1f} MB\n", "Gigabytes Out:", sent_mb);
    fmt::print("{:<20}{:.0f} msgs/sec\n", "Local T-Put:", g_sent_count.load() / actual_duration);
    fmt::print("==============================================================\n");

    return 0;
}
