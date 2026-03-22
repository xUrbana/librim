#include "librim/context.hpp"
#include "librim/factory.hpp"
#include <chrono>
#include <future>
#include <gtest/gtest.h>

TEST(NetworkingTest, TcpClientServer)
{
    librim::Context context;

    // We'll use a simple 4-byte header representing the string size
    librim::ServerConfig sconfig{librim::Protocol::TCP, 12345, 4, [](std::span<const std::byte> hdr) {
                                     uint32_t size = 0;
                                     std::memcpy(&size, hdr.data(), 4);
                                     return size + 4; // payload + header
                                 }};
    auto server_var = librim::Factory::create_tcp_server(context, sconfig);
    ASSERT_TRUE(server_var.has_value());
    auto server = server_var.value();

    std::promise<std::string> received_promise;
    auto received_future = received_promise.get_future();

    server->on_receive([&](std::shared_ptr<librim::TcpConnection> conn, std::span<const std::byte> data) {
        // skip 4 bytes header
        std::string str(reinterpret_cast<const char *>(data.data() + 4), data.size() - 4);
        received_promise.set_value(str);
        conn->close();
    });

    ASSERT_TRUE(server->start().has_value());

    librim::ClientConfig cconfig{librim::Protocol::TCP, "127.0.0.1", 12345, 4, [](std::span<const std::byte> hdr) {
                                     uint32_t size = 0;
                                     std::memcpy(&size, hdr.data(), 4);
                                     return size + 4;
                                 }};
    auto client_var = librim::Factory::create_tcp_client(context, cconfig);
    ASSERT_TRUE(client_var.has_value());
    auto client = client_var.value();

    auto conn_res = client->connect("127.0.0.1", 12345, [&](librim::expected<void, boost::system::error_code> res) {
        EXPECT_TRUE(res.has_value());
        if (res)
        {
            std::string msg = "hello tcp";
            uint32_t size = msg.size();
            std::vector<std::byte> packet(4 + size);
            std::memcpy(packet.data(), &size, 4);
            std::memcpy(packet.data() + 4, msg.data(), size);
            auto send_res = client->send(packet);
            EXPECT_TRUE(send_res.has_value());
        }
    });
    EXPECT_TRUE(conn_res.has_value());

    context.start(1);

    auto status = received_future.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(status, std::future_status::ready);

    if (status == std::future_status::ready)
    {
        EXPECT_EQ(received_future.get(), "hello tcp");
    }

    client->close();
    context.stop();
}

TEST(NetworkingTest, UdpClientServer)
{
    librim::Context context;

    librim::ServerConfig sconfig{librim::Protocol::UDP, 12346};
    auto server_var = librim::Factory::create_udp_server(context, sconfig);
    ASSERT_TRUE(server_var.has_value());
    auto server = server_var.value();

    std::promise<std::string> received_promise;
    auto received_future = received_promise.get_future();

    server->on_receive([&](std::span<const std::byte> data, const boost::asio::ip::udp::endpoint &) {
        std::string str(reinterpret_cast<const char *>(data.data()), data.size());
        static bool flag = false;
        if (!flag)
        {
            received_promise.set_value(str);
            flag = true;
        }
    });
    server->start_receive();

    librim::ClientConfig cconfig{librim::Protocol::UDP, "127.0.0.1", 12346};
    auto client_var = librim::Factory::create_udp_client(context, cconfig);
    ASSERT_TRUE(client_var.has_value());
    auto client = client_var.value();

    std::string msg = "hello udp";
    std::span<const std::byte> data(reinterpret_cast<const std::byte *>(msg.data()), msg.size());
    auto res = client->send(data);
    EXPECT_TRUE(res.has_value());

    context.start(1);

    auto status = received_future.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(status, std::future_status::ready);

    if (status == std::future_status::ready)
    {
        EXPECT_EQ(received_future.get(), "hello udp");
    }

    server->close();
    client->close();
    context.stop();
}

TEST(NetworkingTest, ReadTimeout)
{
    librim::Context context;

    librim::ServerConfig sconfig{librim::Protocol::TCP, 12347, 4, [](std::span<const std::byte> hdr) {
                                     uint32_t size = 0;
                                     std::memcpy(&size, hdr.data(), 4);
                                     return size + 4;
                                 }};

    auto server_var = librim::Factory::create_tcp_server(context, sconfig);
    ASSERT_TRUE(server_var.has_value());
    auto server = server_var.value();
    server->on_receive([](std::shared_ptr<librim::TcpConnection>, std::span<const std::byte>) {});
    ASSERT_TRUE(server->start().has_value());

    // Client with 50ms read timeout
    librim::ClientConfig cconfig{librim::Protocol::TCP, "127.0.0.1", 12347, 4, [](std::span<const std::byte> hdr) {
                                     uint32_t size = 0;
                                     std::memcpy(&size, hdr.data(), 4);
                                     return size + 4;
                                 }};
    cconfig.read_timeout = std::chrono::milliseconds(50);
    
    auto client_var = librim::Factory::create_tcp_client(context, cconfig);
    ASSERT_TRUE(client_var.has_value());
    auto client = client_var.value();

    auto conn_res = client->connect("127.0.0.1", 12347, [](auto res) {
        EXPECT_TRUE(res.has_value());
    });
    EXPECT_TRUE(conn_res.has_value());

    context.start(1);

    // Give connection time to establish and then timeout due to inactivity
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Because the client timed out, it should have closed the connection internally.
    std::string msg = "too late";
    std::span<const std::byte> data(reinterpret_cast<const std::byte *>(msg.data()), msg.size());
    auto send_res = client->send(data);
    
    // send should fail because is_connected_ was set to false by the timeout handler
    EXPECT_FALSE(send_res.has_value());
    if (!send_res) {
        EXPECT_EQ(send_res.error(), boost::asio::error::not_connected);
    }

    client->close();
    context.stop();
}
