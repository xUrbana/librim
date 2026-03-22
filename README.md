# librim

A modern, header-only C++20 networking library built on [Boost.ASIO](https://www.boost.org/doc/libs/release/libs/asio/) that provides high-performance, type-safe abstractions for TCP and UDP communication with integrated message dispatching.

## Features

- **Header-only** — include and go, no separate compilation step
- **TCP client & server** with automatic framing (header → payload protocol)
- **UDP endpoint** supporting client, server, and multicast roles
- **Type-safe message dispatcher** — routes raw bytes to strongly-typed handler callbacks
- **Async binary logger** — dual-buffer, non-blocking disk I/O so network threads never stall
- **`expected<T, E>` error handling** — no exceptions on hot paths
- **Buffer pooling** — reuses heap allocations using lock-free architecture to reduce GC pressure on high-frequency messaging
- **Strand-based concurrency** — serialized async operations without explicit locks on the data path
- **Factory API** — one-line construction of clients and servers with sensible defaults
- **Application framework** — optional high-level `RimApplication` base class for common lifecycle management

## Requirements

| Dependency | Version |
|---|---|
| C++ Standard | C++20 |
| CMake | 3.20+ |
| Boost (system) | Any recent |
| spdlog | 1.17.0 (fetched automatically) |
| GoogleTest | Fetched automatically (tests only) |
| Compiler | clang++ recommended |

## Building

```bash
git clone <repo>
cd librim
cmake -S . -B build
cmake --build build
```

This produces the following targets in `build/examples/` and `build/tests/`:

| Target | Description |
|---|---|
| `librim_demo` | End-to-end demo showing a TCP ping/text server & client |
| `librim_sensor_network` | UDP demonstration with an aggregator node and multiple remote clients |
| `librim_hybrid_gateway` | Relaying setup listening softly on TCP and broadcasting dynamically to UDP via a standalone endpoint |
| `librim_test` | GoogleTest suite |

```bash
cd build && ctest        # run tests
./examples/librim_demo            # run interactive demo
./examples/librim_sensor_network  # run multi-client UDP demo
./examples/librim_hybrid_gateway  # run multi-protocol gateway demo
```

## Examples

We provide several thoroughly documented examples in the `examples/` directory demonstrating different topologies:

1. **`01_basic_demo.cpp`**: Demonstrates the `RimApplication` framework instantiating a localized TCP Server and pairing it against a single TCP client to trigger explicitly defined dispatcher callbacks such as `Ping` and `TextData`.
2. **`02_sensor_network_udp.cpp`**: Demonstrates a high-speed aggregation topology tracking 3 independent UDP software clients natively streaming custom telemetry arrays concurrently to a single UDP gateway.
3. **`03_hybrid_gateway.cpp`**: Demonstrates a mixed-protocol "Relay" approach seamlessly translating inbound TCP commands into outbound multithreaded UDP streams spanning the active loopback securely.

## Quick Start

### TCP Server

```cpp
#include "librim/context.hpp"
#include "librim/tcp_server.hpp"

librim::Context ctx;

auto framing = [](std::span<const std::byte> header) -> std::size_t {
    uint32_t total;
    std::memcpy(&total, header.data(), 4);
    return be32toh(total);
};

librim::TcpServer server(ctx, /*port=*/9000, /*header_size=*/4, framing, /*max_msg=*/10*1024*1024);

server.on_connect([](std::shared_ptr<librim::TcpConnection> conn) {
    // new connection
});
server.on_receive([](std::shared_ptr<librim::TcpConnection> conn, std::span<const std::byte> data) {
    // handle message
    conn->send(data); // echo back
});

server.start();
ctx.start(4); // 4 worker threads
```

### TCP Client

```cpp
#include "librim/tcp_client.hpp"

librim::TcpClient client(ctx, /*header_size=*/4, framing, /*max_msg=*/10*1024*1024);

client.on_receive([](std::span<const std::byte> data) {
    // handle response
});

client.connect("127.0.0.1", 9000, [](auto result) {
    if (result) { /* connected */ }
});
```

### Message Dispatcher

```cpp
#include "librim/message_dispatcher.hpp"

enum class MsgType : uint32_t { Ping = 1, Text = 2 };

struct Header { uint32_t type; uint32_t length; };
struct PingMsg { Header hdr; uint32_t seq; };

librim::MessageDispatcher<MsgType, Header> dispatcher(
    [](const Header& h) -> std::pair<MsgType, std::size_t> {
        return { static_cast<MsgType>(be32toh(h.type)), be32toh(h.length) };
    }
);

dispatcher.add_handler<PingMsg>(MsgType::Ping, [](PingMsg& msg) {
    // handle ping
});
```

### UDP Endpoint

```cpp
#include "librim/udp_endpoint.hpp"

// Server
librim::UdpEndpoint server(ctx);
server.bind(9001);
server.on_receive([](std::span<const std::byte> data, const boost::asio::ip::udp::endpoint& from) {
    // handle datagram
});
server.start_receive();

// Client
librim::UdpEndpoint client(ctx);
client.connect("127.0.0.1", 9001);
client.send(/* span<const byte> */);
```

### Application Framework

```cpp
#include "librim/application.hpp"

class MyServer : public librim::RimApplication<MsgType, Header> {
public:
    MyServer() : RimApplication(librim::AppConfig{
        librim::AppRole::Server, librim::Protocol::TCP, "", 9000
    }) {}

    librim::MessageDispatcher<MsgType, Header>::HeaderExtractor get_extractor() override { ... }
    void setup_handlers(librim::MessageDispatcher<MsgType, Header>& d) override { ... }
    void wait_for_shutdown() override { /* block until SIGINT etc. */ }
};

MyServer app;
app.run(); // blocking
```

## Architecture

```
librim/
├── context.hpp          — io_context + thread pool wrapper
├── tcp_server.hpp       — TcpServer (acceptor) + TcpConnection (per-client)
├── tcp_client.hpp       — TcpClient with async framed receive
├── udp_endpoint.hpp     — UdpEndpoint (client / server / multicast)
├── message_dispatcher.hpp — Type-safe message routing
├── async_logger.hpp     — Dual-buffer async binary logger
├── factory.hpp          — One-call construction helpers + Protocol enum
├── expected.hpp         — expected<T,E> / unexpected<E> error type
└── application.hpp      — RimApplication high-level lifecycle framework
```

### Framing Protocol

TCP connections use a two-phase receive loop:

```
[ Header (fixed size) ] → user framing lambda → total_size
[ Payload (total_size - header_size bytes) ]
→ on_receive callback with full message span
```

The framing lambda is user-defined, so any header format is supported.

### Concurrency Model

All per-socket operations are serialized through a `boost::asio::strand`. Multiple worker threads share the same `io_context`, giving parallel throughput without per-socket data races. The `AsyncLogger` offloads disk I/O to a dedicated thread using a swap-buffer pattern so network handlers are never blocked on I/O.

## Error Handling

Functions that can fail return `librim::expected<T, boost::system::error_code>`:

```cpp
auto result = client.connect("127.0.0.1", 9000);
if (!result) {
    std::cerr << result.error().message() << "\n";
}
```

## License

See `LICENSE` for details.
