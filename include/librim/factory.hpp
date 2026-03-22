#pragma once

#include "librim/context.hpp"
#include "librim/expected.hpp"
#include "librim/tcp_client.hpp"
#include "librim/tcp_server.hpp"
#include "librim/udp_endpoint.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <variant>

namespace librim
{

/**
 * @brief Enum defining explicitly supported base OSI transport layers.
 */
enum class Protocol
{
    TCP, ///< Transmission Control Protocol (Reliable streaming).
    UDP  ///< User Datagram Protocol (Fast datagram bursts).
};

/**
 * @struct ClientConfig
 * @brief Comprehensive properties defining an outbound Client socket connection.
 */
struct ClientConfig
{
    Protocol protocol; ///< Selected underlying protocol (e.g., TCP or UDP).
    std::string ip;    ///< Target domain name or direct IPv4/IPv6 target literal.
    uint16_t port;     ///< Target destination port bound on the remote server.

    // TCP Specific Framing logic
    std::size_t header_size = 0; ///< Known constant dimension of the unified application Header.
    std::function<std::size_t(std::span<const std::byte>)> get_total_size = {}; ///< Header dimension translating hook.
    std::size_t max_message_size = 10 * 1024 * 1024; ///< 10MB Default limit natively protecting against payload sizes.
    std::chrono::milliseconds read_timeout{0};       ///< 0 means no timeout natively. Drop connections stalling beyond context.
};

/**
 * @struct ServerConfig
 * @brief Parameters defining the active properties of a localized listener topology.
 */
struct ServerConfig
{
    Protocol protocol; ///< Enum defining the active binding target stack.
    uint16_t port;     ///< Local target port for resolving network adapters against.

    // TCP Specific Framing details inherited identically across streams
    std::size_t header_size = 0; ///< Constant dimension mapped dynamically from initial blocks.
    std::function<std::size_t(std::span<const std::byte>)> get_total_size = {}; ///< Executable hook bounding arrays.
    std::size_t max_message_size = 10 * 1024 * 1024; ///< 10MB Default DoS prevention mechanism statically applied.
    std::chrono::milliseconds read_timeout{0};       ///< 0 means no timeout. Controls native socket staleness overrides.
};

/**
 * @brief Convenient polymorphic wrapper bridging the distinct Client implementations natively in application logic.
 */
using ClientVariant = std::variant<std::shared_ptr<TcpClient>, std::shared_ptr<UdpEndpoint>>;

/**
 * @brief Variant defining the overarching Server root nodes routing connections dynamically.
 */
using ServerVariant = std::variant<std::shared_ptr<TcpServer>, std::shared_ptr<UdpEndpoint>>;

/**
 * @class Factory
 * @brief Centralized instantiation namespace natively deploying correctly initialized Networking domains.
 *
 * Utilizes `librim::expected` to natively fail forward dynamically over internal exception throwing models.
 */
class Factory
{
  public:
    /**
     * @brief Spawns a disconnected TcpClient logically. Call `connect` externally to instantiate physical socket bounds.
     * @param context Active ASIO loop mechanism logically running the system.
     * @param config Parameters.
     * @return Logically instantiated shared instance.
     */
    static librim::expected<std::shared_ptr<TcpClient>, boost::system::error_code> create_tcp_client(
        Context &context, const ClientConfig &config)
    {
        auto client = std::make_shared<TcpClient>(context, config.header_size, config.get_total_size,
                                                  config.max_message_size, config.read_timeout);
        return client;
    }

    /**
     * @brief Attempts an instant DNS resolution and generates a UDP sender strictly bound to the target natively.
     * @param context Active loop logic.
     * @param config Connection parameters.
     * @return Expected populated struct natively tracking the connection bindings.
     */
    static librim::expected<std::shared_ptr<UdpEndpoint>, boost::system::error_code> create_udp_client(
        Context &context, const ClientConfig &config)
    {
        auto client = std::make_shared<UdpEndpoint>(context);
        auto res = client->connect(config.ip, config.port);
        if (!res)
            return librim::unexpected(res.error());
        return client;
    }

    /**
     * @brief Hooks a TCP acceptor dynamically resolving multiple inputs natively. Call `start` to hook listeners.
     */
    static librim::expected<std::shared_ptr<TcpServer>, boost::system::error_code> create_tcp_server(
        Context &context, const ServerConfig &config)
    {
        auto server = std::make_shared<TcpServer>(context, config.port, config.header_size, config.get_total_size,
                                                  config.max_message_size, config.read_timeout);
        return server;
    }

    /**
     * @brief Explicitly binds a UDP target strictly listening for dynamic input frames out of external inputs.
     */
    static librim::expected<std::shared_ptr<UdpEndpoint>, boost::system::error_code> create_udp_server(
        Context &context, const ServerConfig &config)
    {
        auto server = std::make_shared<UdpEndpoint>(context);
        auto res = server->bind(config.port);
        if (!res)
            return librim::unexpected(res.error());
        return server;
    }
};

} // namespace librim
