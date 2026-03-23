#pragma once

#include "librim/async_logger.hpp"
#include "librim/context.hpp"
#include "librim/factory.hpp"
#include "librim/message_dispatcher.hpp"
#include <chrono>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

namespace librim
{

/**
 * @brief Role identifying whether the App natively manages active acceptor routing (Server) or outbound hooking
 * (Client).
 */
enum class AppRole
{
    Server,
    Client
};

/**
 * @struct AppConfig
 * @brief High-level orchestration settings injected uniformly at program layout.
 */
struct AppConfig
{
    AppRole role;              ///< Active operating hierarchy assignment.
    librim::Protocol protocol; ///< Network structure.
    std::string address;       ///< Domain or Target mappings for Client usages explicitly.
    uint16_t port;             ///< Target operating socket index natively.
    uint32_t record_id = 0;    ///< External identifier tracking origin structures generically.
};

// -------------------------------------------------------------
// The Application Abstraction
// -------------------------------------------------------------

/**
 * @class RimApplication
 * @brief Boilerplate-free execution framework for massive-concurrency network servers/clients.
 *
 * RimApplication consolidates Boost::ASIO Context configuration, factory layout generation,
 * background logger injection, and strongly-typed payload Dispatcher generation inherently
 * into a single cohesive override pattern.
 *
 * Inherit from this class logically, implement the virtual hooks to wire behaviors cleanly!
 *
 * @tparam MsgTypeEnum Custom application logic enumerator routing the dispatcher natively.
 * @tparam HeaderStruct Standard fixed array logically framing the layout natively.
 */
template <typename MsgTypeEnum, typename HeaderStruct> class RimApplication
{
  protected:
    AppConfig config_;             ///< Active static definitions explicitly routing the node mappings.
    librim::ServerVariant server_; ///< Dynamically instantiated acceptor structure cleanly.
    librim::ClientVariant client_; ///< Explicit connection outbound variant hooks tracking allocations inherently.
    std::shared_ptr<librim::AsyncLogger>
        logger_; ///< System-wide background logger securely isolating disk bottlenecks natively.

    std::atomic<bool> app_running_{true}; ///< State flag holding the main process alive explicitly.

  public:
    /**
     * @brief Bootstrapping logic explicitly hooking external dependencies immediately.
     * @param config Explicit AppConfig struct definitions.
     * @param shared_logger (Optional) Pre-instantiated logger target enabling Cross-Application file aggregation
     * cleanly.
     */
    RimApplication(const AppConfig &config, std::shared_ptr<librim::AsyncLogger> shared_logger = nullptr)
        : config_(config), logger_(std::move(shared_logger))
    {
    }

    /**
     * @brief Default explicit generic inheritance destructor.
     */
    virtual ~RimApplication() = default;

    /**
     * @brief PURE VIRTUAL: Forces implementations natively defining Header interpretation definitions logically.
     * @return Subordinate HeaderExtractor cleanly defining layout extraction schemas natively.
     */
    virtual typename librim::MessageDispatcher<MsgTypeEnum, HeaderStruct>::HeaderExtractor get_extractor() = 0;

    /**
     * @brief PURE VIRTUAL: Injection point strictly wiring network packet layouts into execution lambdas natively.
     * @param dispatcher Central router executing active callbacks mapped securely.
     */
    virtual void setup_handlers(librim::MessageDispatcher<MsgTypeEnum, HeaderStruct> &dispatcher) = 0;

    /**
     * @brief VIRTUAL: Execute arbitrary triggers natively the INSTANT a client logically initiates active OSI streams
     * successfully.
     * @param client Overarching Client connection parameters safely.
     */
    virtual void on_client_connected(librim::ClientVariant & /*client*/)
    {
    } // Optional hook for clients

    /**
     * @brief VIRTUAL: Clean up structures strictly before the underlying network teardown cycle destroys arrays
     * dynamically.
     */
    virtual void teardown()
    {
    }

    /**
     * @brief Sleeps logically until the system `stop()` boundary terminates the looping structure exclusively.
     */
    virtual void wait_for_shutdown()
    {
        while (app_running_)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    /**
     * @brief Initiates graceful network shutdown procedures organically terminating threads reliably.
     */
    void stop()
    {
        app_running_ = false;
    }

    /**
     * @brief Core synchronous blocking orchestrator implicitly executing the pipeline logically.
     *
     * Sets up the ASIO environment natively, launches the active dispatcher structures, hooks sockets securely,
     * provisions worker threads properly, blocks cleanly, and entirely handles context cleanup organically.
     */
    void run()
    {
        librim::Context context;

        // Populate independent Logger structure solely if the user didn't logically supply an override context
        if (!logger_)
        {
            logger_ = std::make_shared<librim::AsyncLogger>();
        }

        // 1. Setup Dispatcher via virtual extractor
        auto dispatcher = std::make_shared<librim::MessageDispatcher<MsgTypeEnum, HeaderStruct>>(get_extractor());

        // 2. Register handlers via virtual setup securely inherently
        setup_handlers(*dispatcher);

        // 3. Setup TCP/UDP Server or Client explicitly leveraging Factory architectures natively
        spdlog::info("[App] Starting {} {} on {}:{}...", (config_.protocol == librim::Protocol::TCP ? "TCP" : "UDP"),
                     (config_.role == AppRole::Server ? "Server" : "Client"), config_.address, config_.port);

        if (config_.role == AppRole::Server)
        {
            setup_server(context, dispatcher);
        }
        else
        {
            setup_client(context, dispatcher);
        }

        // 4. Spin up native threads and physically lock the main context dynamically until explicitly terminated safely
        context.start(4);
        wait_for_shutdown();

        // 5. Cleanup hooks sequentially terminating memory dependencies dynamically
        teardown();
        context.stop();

        // Critical: Sockets must be destroyed logically BEFORE the overarching ASIO Context is physically unmapped
        // successfully.
        server_ = librim::ServerVariant{};
        client_ = librim::ClientVariant{};
    }

  private:
    /**
     * @brief Subordinate routing implicitly deploying Server-centric listener mappings definitively.
     */
    void setup_server(librim::Context &context,
                      std::shared_ptr<librim::MessageDispatcher<MsgTypeEnum, HeaderStruct>> dispatcher)
    {
        librim::ServerConfig cfg{config_.protocol, config_.port, sizeof(HeaderStruct), dispatcher->get_size_lambda()};

        if (config_.protocol == librim::Protocol::TCP)
        {
            if (auto var = librim::Factory::create_tcp_server(context, cfg))
            {
                server_ = var.value();
                // Inject intermediate logging lambda capturing network bytes independently BEFORE logical execution
                // safely
                std::get<std::shared_ptr<librim::TcpServer>>(server_)->on_receive(
                    [logger = logger_, dispatcher](std::shared_ptr<librim::TcpConnection> /*conn*/,
                                                   std::span<const std::byte> data) {
                        // logger->log(record_id, data);
                        dispatcher->dispatch(data);
                    });

                if (auto res = std::get<std::shared_ptr<librim::TcpServer>>(server_)->start(); !res)
                    spdlog::error("[App] TCP Server failed to start: {}", res.error().message());
            }
        }
        else
        {
            if (auto var = librim::Factory::create_udp_server(context, cfg))
            {
                server_ = var.value();
                std::get<std::shared_ptr<librim::UdpEndpoint>>(server_)->on_receive(
                    [logger = logger_, dispatcher](std::span<const std::byte> data,
                                                   const boost::asio::ip::udp::endpoint & /*sender*/) {
                        // logger->log(record_id, data);
                        dispatcher->dispatch(data);
                    });
                std::get<std::shared_ptr<librim::UdpEndpoint>>(server_)->start_receive();
            }
        }
    }

    /**
     * @brief Dynamic execution instantiating Client components resolving external IP sockets successfully.
     */
    void setup_client(librim::Context &context,
                      std::shared_ptr<librim::MessageDispatcher<MsgTypeEnum, HeaderStruct>> dispatcher)
    {
        librim::ClientConfig cfg{config_.protocol, config_.address, config_.port, sizeof(HeaderStruct),
                                 dispatcher->get_size_lambda()};

        if (config_.protocol == librim::Protocol::TCP)
        {
            if (auto var = librim::Factory::create_tcp_client(context, cfg))
            {
                client_ = var.value();
                std::get<std::shared_ptr<librim::TcpClient>>(client_)->on_receive(
                    [logger = logger_, dispatcher](std::span<const std::byte> data) {
                        // logger->log(record_id, data);
                        dispatcher->dispatch(data);
                    });

                auto res = std::get<std::shared_ptr<librim::TcpClient>>(client_)->connect(
                    config_.address, config_.port, [&](auto res2) {
                        if (res2)
                            on_client_connected(client_);
                    });
                if (!res)
                    spdlog::error("[App] TCP Client failed to connect: {}", res.error().message());
            }
        }
        else
        {
            if (auto var = librim::Factory::create_udp_client(context, cfg))
            {
                client_ = var.value();
                std::get<std::shared_ptr<librim::UdpEndpoint>>(client_)->on_receive(
                    [logger = logger_, dispatcher](std::span<const std::byte> data,
                                                   const boost::asio::ip::udp::endpoint & /*sender*/) {
                        // logger->log(record_id, data);
                        dispatcher->dispatch(data);
                    });
                std::get<std::shared_ptr<librim::UdpEndpoint>>(client_)->start_receive();
                on_client_connected(client_); // UDP is connectionless so hook intrinsically
            }
        }
    }
};

} // namespace librim
