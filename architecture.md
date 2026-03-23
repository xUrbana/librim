# Librim Software Architecture

This document delves into the internal mechanics of `librim`. It is intended for software engineers and maintainers who need to fundamentally understand *how* the library achieves high-performance concurrent networking, thread safety, and lock-free execution without relying on explicit mutexes over the critical path.

---

## 1. Core Principles & Concurrency Model

### Boost.ASIO & The [Context](include/librim/context.hpp#L19-L95) Object
At the absolute center of the library sits the `librim::Context`. It manages a single `boost::asio::io_context` and a pool of native `std::thread` workers. 
Rather than creating separate threads for reading, writing, and polling explicitly, `librim` leans on the Proactor design pattern. The OS asynchronously notifies the `io_context` when physical sockets have data or when writes finish, and the context delegates the associated completion callbacks ("handlers") to *any* available thread in the [Context](include/librim/context.hpp#L19-L95) pool.

### The Problem with Mutexes
Because handlers can be invoked by any thread randomly, standard object state (like a client's specific socket buffer, active timers, or send queues) is highly susceptible to race conditions. The legacy approach is to wrap all state inside a large `std::mutex`. As identified in prior profiling, massive concurrency causes severe mutex contention (threads stall waiting to acquire locks to simply push a byte array to a socket).

### The Solution: ASIO Strands
Instead of `mutex`, `librim` assigns a `boost::asio::strand` to every single [TcpConnection](include/librim/tcp_server.hpp#L47-L69), [TcpClient](include/librim/tcp_client.hpp#L48-L67), and [UdpEndpoint](include/librim/udp_endpoint.hpp#L26-L316) internally.
A "strand" strictly guarantees that execution handlers posted to it will run **sequentially**, never concurrently. Even if 10 threads try to trigger a [do_write](include/librim/tcp_server.hpp#L151-L180) callback simultaneously, the strand explicitly orchestrates them so exactly 1 executes at a time for that specific socket. This inherently makes all internal socket state (buffers, queues, and timers) completely thread-safe without ever blocking execution via kernel-level `mutex` suspensions.

---

## 2. Lock-Free Memory Pipeline

When user code calls `client->send(data)`, the caller thread naturally executes much faster than the OS can actually push bytes through the physical NIC. Handlers must queue the outgoing memory chunks safely into an intermediate buffer sequence.

If 50 UI threads concurrently call [send()](include/librim/udp_endpoint.hpp#L205-L216), standard `std::deque` logic would break without a mutex. To resolve this, `librim` introduces a physical `boost::lockfree::queue<std::vector<std::byte>*>` alongside a contiguous `boost::circular_buffer`.

### Fast-Path Memory Allocation Sequence
When any thread attempts to broadcast, it asks the lock-free pool natively for an idle memory allocation (`pop`). If the pool is temporarily starved under high traffic, the calling thread simply yields its CPU slice explicitly (`std::this_thread::yield()`), organically backpressuring application rates to match OS kernel limits. Because the lock-free pool is strictly pre-allocated with exactly 8192 uniformly-sized buffers upon endpoint construction, `librim` completely eradicates dynamic heap allocations (`new`/`delete`) and allocator locks (`mmap`) entirely off the hot path! Once a buffer is acquired via `.pop()`, the data is statically assigned and passed to the Strand, which sequentially aggregates it inside a fixed-capacity `boost::circular_buffer` to eliminate L1 cache penalty traversals efficiently.

```mermaid
sequenceDiagram
    autonumber
    participant Thread as App Calling Thread
    participant Pool as Lock-Free Pool
    participant Strand as ASIO Socket Strand
    participant Socket as OS Kernel Socket

    Thread->>Pool: pop()
    alt Pool has an idle buffer
        Pool-->>Thread: Returns cached vector*
    else Pool is completely starved (High Traffic / Network Degraded)
        Thread->>Thread: Yields CPU execution explicitly (`std::this_thread::yield()`) securely halting bursts
    end
    Thread->>Thread: Copies payload bytes into vector*
    Thread->>Strand: boost::asio::post( push pointer to send_queue & trigger do_write() )
    
    Strand->>Socket: boost::asio::async_write( vector* sequence )
    Note right of Socket: The socket asynchronously drains bytes to the NIC in the background.
    
    Socket-->>Strand: Triggers completion handler when finished natively
    Strand->>Pool: push(vector*)
    alt Pool has capacity
        Note left of Pool: Buffer is instantly recycled for future send() calls.
    else Pool is physically full 
        Strand->>Strand: Explicitly calls `delete vector*` 
    end
    
    Strand->>Strand: Checks send_queue for more pending packets and recursively loops.
```

---

## 3. TCP Receiver Lifecycle & Framing Loop

TCP is a stream protocol; it has no concept of "messages." It only knows sequences of continuous bytes.
`librim`'s architecture explicitly requires users to prepend packets with a fixed structural Header indicating the exact dimension of the arriving packet (e.g., standard TLV structures).

The [TcpConnection](include/librim/tcp_server.hpp#L47-L69) instance intrinsically loops a continuous sliding-window ingestion mechanic bound cleanly within its explicit strand.

Instead of issuing tiny individual sequential asynchronous reads for headers followed by payloads—which violently maximizes identical expensive kernel syscall mappings under heavy load—`librim` aggressively leverages a highly efficient massive `async_read_some` block slurping contiguous frame aggregations indiscriminately safely.

```mermaid
sequenceDiagram
    autonumber
    participant App as RimApplication
    participant Target as TcpConnection Native Target
    participant Strand as Connection Strand
    participant Socket as OS TCP Socket

    App->>Target: start()
    Target->>Strand: start_receive() - Allocates completely reusable 64KB boundary arrays
    Strand->>Socket: boost::asio::async_read_some( available_buffer_capacity )
    
    Note right of Socket: OS passively dumps hundreds of chained identical packets natively...
    
    Socket-->>Strand: on_read_some(bytes_transferred)
    Strand->>Target: process_buffer() evaluates structural invariants entirely inside memory context
    Target->>App: Invokes user `on_receive` dynamically unpacking identically scaled `std::span` chunks seamlessly
    
    Note left of Strand: Fractional incomplete slices mechanically drop back tracking internally via `std::memmove`
    Target->>Strand: start_receive() (Loops seamlessly ingesting incoming datagram arrays)
```

---

## 4. Message Dispatcher Mechanics

To prevent the User from having to write massive chaotic `switch/case` byte casting cascades identically evaluating different data types, `librim` features the [MessageDispatcher](include/librim/message_dispatcher.hpp#L22-L167).

The Dispatcher is explicitly initialized with two templates: the custom `Enum` identifying packet classes securely, and the custom [Header](include/librim/async_logger.hpp#L28-L34) formatting defining the protocol natively.

```mermaid
sequenceDiagram
    autonumber
    participant Node as Network Link (TCP/UDP)
    participant Receiver as RimApplication Base
    participant Dispatcher as MessageDispatcher
    participant Map as Std::unordered_map
    participant CustomHandler as User Derived Class

    Node->>Receiver: raw_bytes arrived
    Receiver->>Dispatcher: dispatch(raw_bytes)
    
    Note right of Dispatcher: Directly copies generic Header layout
    Dispatcher->>Dispatcher: Executes `HeaderExtractor` fetching Enum Target & Size
    Dispatcher->>Map: Looks up std::function matching the Enum Target
    Map-->>Dispatcher: Returns strictly bound `<MsgType, Lambda>` callback
    
    Note right of Dispatcher: Generates strongly-typed wrapper translating the raw memory
    Dispatcher->>CustomHandler: handle_specific_data( CustomMessageStruct& msg )
```

---

## 5. Safely Shutting Down

Because ASIO strands process events indiscriminately across background threads, closing sockets manually crashes applications drastically natively if wait-timers or `async_send_to` calls overlap with the `delete` of the native File Descriptor securely routing them.

**Librim solves this structurally:**
All [close()](include/librim/udp_endpoint.hpp#L249-L266) hooks are explicitly redirected into the protective Strand execution layer natively via `boost::asio::dispatch(strand_, ...)`. This guarantees the asynchronous loop physically finishes resolving the active network block logic entirely before deleting the connection handles gracefully!
