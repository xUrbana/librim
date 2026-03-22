/**
 * @file 02_sensor_network_udp.cpp
 * @brief Demonstrates a high-speed UDP telemetry aggregation node receiving data from multiple independent UDP clients.
 *
 * Architecture:
 * - 1 UDP "Aggregator" Server listening on Port 8050.
 * - 3 independent UDP "Sensor" Clients transmitting uniquely scoped telemetry.
 */

#include "librim/application.hpp"
#include <chrono>
#include <thread>

// ------------------------------------------------------------------
// 1. Definition of Network Protocols & Application Schemas
// ------------------------------------------------------------------

enum class SensorMsgType : uint32_t
{
    Telemetry = 100,
    StatusAlert = 101
};

#pragma pack(push, 1)

// Standard framing definition
struct SensorHeader
{
    uint32_t type;
    uint32_t length;
};

// 16-byte fixed telemetry packet
struct TelemetryPacket
{
    SensorHeader header;
    uint32_t sensor_id;
    float temperature;
    float humidity;
};

// Alert string container
struct AlertPacket
{
    SensorHeader header;
    uint32_t sensor_id;
};

#pragma pack(pop)

// ------------------------------------------------------------------
// 2. Central Aggregator Server
// ------------------------------------------------------------------

class TelemetryAggregator : public librim::RimApplication<SensorMsgType, SensorHeader>
{
  public:
    TelemetryAggregator(librim::AppConfig config) : librim::RimApplication<SensorMsgType, SensorHeader>(std::move(config))
    {
    }

    void handle_telemetry(TelemetryPacket &packet)
    {
        uint32_t s_id = be32toh(packet.sensor_id);
        
        // Network-to-Host float conversion workaround (since floats aren't standard ntohl directly in all environments)
        uint32_t temp_i = be32toh(*reinterpret_cast<uint32_t*>(&packet.temperature));
        uint32_t hum_i = be32toh(*reinterpret_cast<uint32_t*>(&packet.humidity));
        float t = *reinterpret_cast<float*>(&temp_i);
        float h = *reinterpret_cast<float*>(&hum_i);

        spdlog::info("[Aggregator] Received Telemetry -> Sensor: {} | Temp: {:.1f}C | Hum: {:.1f}%", s_id, t, h);
    }

    void handle_alert(AlertPacket &packet)
    {
        uint32_t length = be32toh(packet.header.length);
        uint32_t s_id = be32toh(packet.sensor_id);
        
        // Extract dynamically offset string explicitly avoiding zero-length-array warnings naturally
        const char *msg_ptr = reinterpret_cast<const char *>(&packet) + sizeof(AlertPacket);
        std::size_t str_len = length - sizeof(uint32_t); 
        std::string alert_text(msg_ptr, str_len);

        spdlog::warn("[Aggregator] ALERT from Sensor: {} -> '{}'", s_id, alert_text);
    }

  protected:
    typename librim::MessageDispatcher<SensorMsgType, SensorHeader>::HeaderExtractor get_extractor() override
    {
        return [](const SensorHeader &h) -> std::pair<SensorMsgType, std::size_t> {
            return {static_cast<SensorMsgType>(be32toh(h.type)), be32toh(h.length) + sizeof(SensorHeader)};
        };
    }

    void setup_handlers(librim::MessageDispatcher<SensorMsgType, SensorHeader> &dispatcher) override
    {
        dispatcher.add_handler(SensorMsgType::Telemetry, this, &TelemetryAggregator::handle_telemetry);
        dispatcher.add_handler(SensorMsgType::StatusAlert, this, &TelemetryAggregator::handle_alert);
    }

    void teardown() override
    {
        spdlog::info("[Aggregator] Safely shutting down UDP aggregator...");
    }

    void wait_for_shutdown() override
    {
        // Keep the aggregator alive for precisely 5 seconds, then stop automatically (for the sake of the demo ending)
        std::this_thread::sleep_for(std::chrono::seconds(5));
        stop(); 
    }
};

// ------------------------------------------------------------------
// 3. Client Sensor Node Logic
// ------------------------------------------------------------------

void run_sensor_node(uint32_t id, float base_temp)
{
    librim::Context context;

    auto size_lambda = [](std::span<const std::byte> data) -> std::size_t {
         SensorHeader h;
         std::memcpy(&h, data.data(), sizeof(SensorHeader));
         return be32toh(h.length) + sizeof(SensorHeader);
    };

    librim::ClientConfig udp_client_cfg{librim::Protocol::UDP, "127.0.0.1", 8050, sizeof(SensorHeader), size_lambda};
    auto udp_client = librim::Factory::create_udp_client(context, udp_client_cfg).value();

    spdlog::info("[Sensor {}] Waking up and transmitting...", id);

    context.start(1);

    for (int i = 0; i < 5; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250)); // Generate data every 250ms

        TelemetryPacket pkt;
        pkt.header.type = htobe32(static_cast<uint32_t>(SensorMsgType::Telemetry));
        pkt.header.length = htobe32(sizeof(uint32_t) + sizeof(float) + sizeof(float)); 
        pkt.sensor_id = htobe32(id);
        
        float t = base_temp + i;
        float h = 45.0f + (id * 2.0f);
        
        uint32_t t_net = htobe32(*reinterpret_cast<uint32_t*>(&t));
        uint32_t h_net = htobe32(*reinterpret_cast<uint32_t*>(&h));
        
        pkt.temperature = *reinterpret_cast<float*>(&t_net);
        pkt.humidity    = *reinterpret_cast<float*>(&h_net);

        std::vector<std::byte> buffer(sizeof(TelemetryPacket));
        std::memcpy(buffer.data(), &pkt, sizeof(TelemetryPacket));
        
        if (auto res = udp_client->send(buffer); !res)
             spdlog::error("[Sensor {}] Transmission failed: {}", id, res.error().message());
    }

    // Send closing alert
    std::string closing_msg = "Shutting down gracefully.";
    std::vector<std::byte> alert_buf(sizeof(AlertPacket) + closing_msg.size());
    
    AlertPacket alert_h;
    alert_h.header.type = htobe32(static_cast<uint32_t>(SensorMsgType::StatusAlert));
    alert_h.header.length = htobe32(sizeof(uint32_t) + closing_msg.size());
    alert_h.sensor_id = htobe32(id);

    std::memcpy(alert_buf.data(), &alert_h, sizeof(AlertPacket));
    std::memcpy(alert_buf.data() + sizeof(AlertPacket), closing_msg.data(), closing_msg.size());
    (void)udp_client->send(alert_buf);

    udp_client->close();
    context.stop();
}

// ------------------------------------------------------------------
// 4. Main Launch Sequence
// ------------------------------------------------------------------

int main()
{
    spdlog::info("--- Starting UDP Sensor Aggregator Demo ---");

    // Launch background threads to act as totally independent Sensor Node applications
    std::thread s1(run_sensor_node, 100, 22.5f);
    std::thread s2(run_sensor_node, 101, 14.2f);
    std::thread s3(run_sensor_node, 102, 35.8f);

    // Run the primary single-process Server mapping the inbound UDP stream
    librim::AppConfig cfg{librim::AppRole::Server, librim::Protocol::UDP, "127.0.0.1", 8050};
    TelemetryAggregator server(cfg);
    server.run(); // Blocks for 5 seconds

    s1.join();
    s2.join();
    s3.join();

    spdlog::info("--- Demo Completed ---");
    return 0;
}
