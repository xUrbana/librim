#include "librim/factory.hpp"
#include "librim/message_dispatcher.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <endian.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <thread>

enum class MsgType : uint32_t
{
    Ping = 1,
    TextData = 2
};

// Ensure structures are packed tightly without padding
#pragma pack(push, 1)

struct AppHeader
{
    uint32_t type;   // Network byte order
    uint32_t length; // Network byte order
};

struct Ping
{
    AppHeader header;
    uint64_t timestamp; // Network byte order
};

// We will map TextData into this struct, sizing is dynamically bounded by packet size
struct TextMsg
{
    AppHeader header;
    char text[0]; // C++ standard flexible-array hack replacement
};

#pragma pack(pop)

#include "librim/application.hpp"

// -------------------------------------------------------------
// Concrete Application Example
// -------------------------------------------------------------
class DemoApp : public librim::RimApplication<MsgType, AppHeader>
{
  public:
    DemoApp(librim::AppConfig config) : librim::RimApplication<MsgType, AppHeader>(std::move(config))
    {
    }

    // Custom handling logic is NOT virtual; these are normal member functions
    void handle_ping(Ping &ping)
    {
        ping.header.type = be32toh(ping.header.type);
        ping.header.length = be32toh(ping.header.length);
        ping.timestamp = be64toh(ping.timestamp);
        spdlog::info("[{}] Processed PING {}", (config_.role == librim::AppRole::Server ? "ServerApp" : "ClientApp"),
                     ping.timestamp);
    }

    void handle_text(TextMsg &msg)
    {
        msg.header.type = be32toh(msg.header.type);
        msg.header.length = be32toh(msg.header.length);

        std::string text(msg.text, msg.header.length);
        spdlog::info("[{}] Processed TEXT: '{}'", (config_.role == librim::AppRole::Server ? "ServerApp" : "ClientApp"),
                     text);
    }

  protected:
    // --- Virtual overrides for setup ---
    typename librim::MessageDispatcher<MsgType, AppHeader>::HeaderExtractor get_extractor() override
    {
        return [](const AppHeader &h) -> std::pair<MsgType, std::size_t> {
            return {static_cast<MsgType>(be32toh(h.type)), be32toh(h.length) + sizeof(AppHeader)};
        };
    }

    void setup_handlers(librim::MessageDispatcher<MsgType, AppHeader> &dispatcher) override
    {
        dispatcher.add_handler(MsgType::Ping, this, &DemoApp::handle_ping);
        dispatcher.add_handler(MsgType::TextData, this, &DemoApp::handle_text);
    }

    void teardown() override
    {
        spdlog::info("[{}] Shutting down.", (config_.role == librim::AppRole::Server ? "ServerApp" : "ClientApp"));
    }

    void wait_for_shutdown() override
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        stop();
    }
};

void run_demo_client()
{
    librim::Context context;

    spdlog::info("[Client] Starting TCP Client...");

    // We need an extractor lambda for the client size callback
    auto size_lambda = [](std::span<const std::byte> data) -> std::size_t {
        AppHeader h;
        std::memcpy(&h, data.data(), sizeof(AppHeader));
        return be32toh(h.length) + sizeof(AppHeader);
    };

    librim::ClientConfig tcp_client_cfg{librim::Protocol::TCP, "127.0.0.1", 8080, sizeof(AppHeader), size_lambda};
    auto tcp_client_var = librim::Factory::create_tcp_client(context, tcp_client_cfg);
    if (!tcp_client_var)
        return;
    auto tcp_client = tcp_client_var.value();

    auto conn_res = tcp_client->connect("127.0.0.1", 8080, [&](librim::expected<void, boost::system::error_code> res) {
        if (res)
        {
            spdlog::info("[Client] Connected.");

            // Send PING
            spdlog::info("[Client] Sending PING with byte-swapped structure...");
            Ping ping_msg;
            ping_msg.header.type = htobe32(static_cast<uint32_t>(MsgType::Ping));
            ping_msg.header.length = htobe32(sizeof(uint64_t)); // Payload length
            ping_msg.timestamp = htobe64(123456789);

            std::vector<std::byte> ping_packet(sizeof(Ping));
            std::memcpy(ping_packet.data(), &ping_msg, sizeof(Ping));
            auto send_res1 = tcp_client->send(ping_packet);
            if (!send_res1)
                spdlog::error("[Client] Failed to send PING: {}", send_res1.error().message());

            // Send TEXT DATA
            spdlog::info("[Client] Sending TEXT DATA with byte-swapped structure...");
            std::string text = "Hello librim, callback routing works!";
            AppHeader h_text;
            h_text.type = htobe32(static_cast<uint32_t>(MsgType::TextData));
            h_text.length = htobe32(static_cast<uint32_t>(text.size()));

            std::vector<std::byte> text_packet(sizeof(AppHeader) + text.size());
            std::memcpy(text_packet.data(), &h_text, sizeof(AppHeader));
            std::memcpy(text_packet.data() + sizeof(AppHeader), text.data(), text.size());
            auto send_res2 = tcp_client->send(text_packet);
            if (!send_res2)
                spdlog::error("[Client] Failed to send TEXT: {}", send_res2.error().message());
        }
    });

    if (!conn_res)
        spdlog::error("[Client] Immediate connect failure: {}", conn_res.error().message());

    context.start(1);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    tcp_client->close();
    context.stop();
}

void run_demo()
{
    // Start client in background thread so it can connect to the server once it's up
    std::thread client_thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // wait for server to start
        run_demo_client();
    });

    librim::AppConfig server_cfg{librim::AppRole::Server, librim::Protocol::TCP, "127.0.0.1", 8080};
    DemoApp app(server_cfg);
    app.run(); // Blocks for 2 seconds

    client_thread.join();
}

int main()
{
    spdlog::info("======================================");
    spdlog::info("      Librim Networking GUI Demo      ");
    spdlog::info("======================================");
    run_demo();
    return 0;
}
