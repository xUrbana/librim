/**
 * @file 03_hybrid_gateway.cpp
 * @brief Demonstrates a "Gateway/Relay" pattern: The app operates a TCP Server collecting commands, 
 *        while simultaneously operating a UDP client relay broadcast stream natively hooked inside the same ASIO context!
 *
 * Architecture:
 * - 1 Primary RimApplication acting as a TCP Server.
 * - 1 manually instantiated `UdpEndpoint` client actively pushing events to a multicast target.
 */

#include "librim/application.hpp"
#include <chrono>
#include <thread>

enum class GatewayMsgType : uint32_t
{
    StateChange = 50,
};

#pragma pack(push, 1)
struct GatewayHeader { uint32_t type; uint32_t length; };

struct StateDirective
{
    GatewayHeader header;
    uint32_t new_state; // Network bytes
};
#pragma pack(pop)

// ------------------------------------------------------------------
// Hybrid Gateway Structure
// ------------------------------------------------------------------

class HybridRelay : public librim::RimApplication<GatewayMsgType, GatewayHeader>
{
  private:
    std::shared_ptr<librim::UdpEndpoint> relay_client_;
    librim::Context background_ctx_; // Custom dedicated context for the relay to not stall the main Acceptor logic!
    
  public:
    HybridRelay(librim::AppConfig config) : librim::RimApplication<GatewayMsgType, GatewayHeader>(std::move(config))
    {
        // Instantiate our outbound relay immediately mapping the target destination. 
        relay_client_ = std::make_shared<librim::UdpEndpoint>(background_ctx_);
        if (auto res = relay_client_->connect("127.0.0.1", 9999); !res)
        {
            spdlog::error("[Hybrid] Failed to map local loopback for relay!");
        }
        else 
        {
            spdlog::info("[Hybrid] Background UDP Gateway connected exclusively to Port 9999.");
            background_ctx_.start(1); // Boot background context thread inherently independent of the App's primary Context
        }
    }

    void handle_state_change(StateDirective &dir)
    {
        uint32_t state = be32toh(dir.new_state);
        spdlog::info("[Hybrid] Inbound TCP Command Received -> Switching State to: {}", state);

        // Dynamically relay this event into the localized UDP bus seamlessly
        std::string alert = "STATE_CHANGE_" + std::to_string(state);
        std::vector<std::byte> payload(alert.size());
        std::transform(alert.begin(), alert.end(), payload.begin(), [](char c) { return static_cast<std::byte>(c); });
        
        if (auto res = relay_client_->send(payload); !res)
            spdlog::error("[Hybrid] UDP relay drop: {}", res.error().message());
    }

  protected:
    typename librim::MessageDispatcher<GatewayMsgType, GatewayHeader>::HeaderExtractor get_extractor() override
    {
        return [](const GatewayHeader &h) -> std::pair<GatewayMsgType, std::size_t> {
            return {static_cast<GatewayMsgType>(be32toh(h.type)), be32toh(h.length) + sizeof(GatewayHeader)};
        };
    }

    void setup_handlers(librim::MessageDispatcher<GatewayMsgType, GatewayHeader> &dispatcher) override
    {
        dispatcher.add_handler(GatewayMsgType::StateChange, this, &HybridRelay::handle_state_change);
    }

    void teardown() override
    {
        relay_client_->close();
        background_ctx_.stop();
        spdlog::info("[Hybrid] Shutdown complete.");
    }

    void wait_for_shutdown() override
    {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        stop();
    }
};

// ------------------------------------------------------------------
// Dummy Implementations simulating interaction topologies natively.
// ------------------------------------------------------------------

void run_tcp_user_interface()
{
    librim::Context context;
    
    // Simulates an end-user driving the gateway configuration.
    auto size_lambda = [](std::span<const std::byte>) -> std::size_t { return sizeof(StateDirective); };
    librim::ClientConfig tcp_cfg{librim::Protocol::TCP, "127.0.0.1", 8100, sizeof(GatewayHeader), size_lambda};
    auto ui_client = librim::Factory::create_tcp_client(context, tcp_cfg).value();

    (void)ui_client->connect("127.0.0.1", 8100, [&](auto res) {
        if (!res) return;

        spdlog::info("[UserUI] Connected to Gateway. Sending directives...");
        
        for (uint32_t active_state = 1; active_state <= 3; ++active_state)
        {
            StateDirective pkt;
            pkt.header.type = htobe32(static_cast<uint32_t>(GatewayMsgType::StateChange));
            pkt.header.length = htobe32(sizeof(uint32_t));
            pkt.new_state = htobe32(active_state);

            std::vector<std::byte> buffer(sizeof(StateDirective));
            std::memcpy(buffer.data(), &pkt, sizeof(StateDirective));
            (void)ui_client->send(buffer);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    context.start(1);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ui_client->close();
    context.stop();
}

void run_udp_downstream_target()
{
    librim::Context context;

    // Simulates a low-level microcontroller on Port 9999 passively receiving relayed packets blindly from the Hybrid gateway.
    librim::ServerConfig target_cfg{librim::Protocol::UDP, 9999, 0, nullptr};
    auto endpoint = librim::Factory::create_udp_server(context, target_cfg).value();

    endpoint->on_receive([](std::span<const std::byte> data, const boost::asio::ip::udp::endpoint &) {
        std::string str(reinterpret_cast<const char*>(data.data()), data.size());
        spdlog::info("[Local MCU] Native Hardware Interrupt Received Relay: '{}'", str);
    });

    endpoint->start_receive();
    context.start(1);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    endpoint->close();
    context.stop();
}

// ------------------------------------------------------------------
// Execution Framework 
// ------------------------------------------------------------------

int main()
{
    // The passive system capturing the hybrid telemetry streams (Server listening UDP)
    std::thread mcu(run_udp_downstream_target);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Sleep organically letting servers hook TCP limits
    
    // The active system commanding the Hybrid node natively (Client hitting TCP)
    std::thread ui(run_tcp_user_interface);

    // Run the primary hybrid gateway organically driving both
    librim::AppConfig cfg{librim::AppRole::Server, librim::Protocol::TCP, "127.0.0.1", 8100};
    HybridRelay relay(cfg);
    relay.run(); // Blocks for 2 seconds

    mcu.join();
    ui.join();

    return 0;
}
