# Librim Software Architecture

This document delves into the internal mechanics of `librim`. It is intended for software engineers and maintainers who need to fundamentally understand *how* the library achieves high-performance concurrent networking, thread safety, and lock-free execution without relying on explicit mutexes over the critical path.

---

## 1. Core Principles & Concurrency Model

### Boost.ASIO & The [Context](file:///home/urbana/Projects/librim/include/librim/context.hpp#19-95) Object
At the absolute center of the library sits the `librim::Context`. It manages a single `boost::asio::io_context` and a pool of native `std::thread` workers. 
Rather than creating separate threads for reading, writing, and polling explicitly, `librim` leans on the Proactor design pattern. The OS asynchronously notifies the `io_context` when physical sockets have data or when writes finish, and the context delegates the associated completion callbacks ("handlers") to *any* available thread in the [Context](file:///home/urbana/Projects/librim/include/librim/context.hpp#19-95) pool.

### The Problem with Mutexes
Because handlers can be invoked by any thread randomly, standard object state (like a client's specific socket buffer, active timers, or send queues) is highly susceptible to race conditions. The legacy approach is to wrap all state inside a large `std::mutex`. As identified in prior profiling, massive concurrency causes severe mutex contention (threads stall waiting to acquire locks to simply push a byte array to a socket).

### The Solution: ASIO Strands
Instead of `mutex`, `librim` assigns a `boost::asio::strand` to every single [TcpConnection](file:///home/urbana/Projects/librim/include/librim/tcp_server.hpp#47-69), [TcpClient](file:///home/urbana/Projects/librim/include/librim/tcp_client.hpp#48-67), and [UdpEndpoint](file:///home/urbana/Projects/librim/include/librim/udp_endpoint.hpp#26-316) internally.
A "strand" strictly guarantees that execution handlers posted to it will run **sequentially**, never concurrently. Even if 10 threads try to trigger a [do_write](file:///home/urbana/Projects/librim/include/librim/tcp_server.hpp#151-180) callback simultaneously, the strand explicitly orchestrates them so exactly 1 executes at a time for that specific socket. This inherently makes all internal socket state (buffers, queues, and timers) completely thread-safe without ever blocking execution via kernel-level `mutex` suspensions.

---

## 2. Lock-Free Memory Pipeline

When user code calls `client->send(data)`, the caller thread naturally executes much faster than the OS can actually push bytes through the physical NIC. Handlers must queue the outgoing memory chunks safely into an intermediate buffer sequence.

If 50 UI threads concurrently call [send()](file:///home/urbana/Projects/librim/include/librim/udp_endpoint.hpp#205-216), standard `std::deque` logic would break without a mutex. To resolve this, `librim` introduces a physical `boost::lockfree::queue<std::vector<std::byte>*>`.

### Fast-Path Memory Allocation Sequence
When any thread attempts to broadcast, it asks the lock-free pool natively for an idle memory allocation (`pop`). If successful, the thread rapidly copies the data and posts the pointer natively to the ASIO Strand. If the pool is temporarily starved under high traffic, the calling thread simply runs `new std::vector()` locally and posts that instead. This prevents the caller from *ever* being blocked.

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
    else Pool is completely starved (High Traffic)
        Thread->>Thread: Explicitly calls `new std::vector()`
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

The [TcpConnection](file:///home/urbana/Projects/librim/include/librim/tcp_server.hpp#47-69) instance intrinsically loops two core async behaviors bound cleanly within its explicit strand.

```mermaid
sequenceDiagram
    autonumber
    participant App as RimApplication
    participant Target as TcpConnection Native Target
    participant Strand as Connection Strand
    participant Socket as OS TCP Socket

    App->>Target: start()
    Target->>Strand: start_receive_header() - Issues read for exactly `header_size_`
    Strand->>Socket: boost::asio::async_read()
    
    Note right of Socket: OS passively buffers physical TCP frames over time...
    
    Socket-->>Strand: on_read_complete(header_bytes)
    Strand->>Target: Executes user Lambda: `get_total_size(header)`
    Target-->>Strand: Returns calculated overarching length (e.g., 1024 bytes)
    Strand->>Strand: start_receive_payload(1024 bytes)
    
    Strand->>Socket: boost::asio::async_read() requesting (1024 - header_size) bytes
    Socket-->>Strand: on_read_complete(payload_bytes)
    Strand->>Target: handle_full_message(1024)
    Target->>App: Invokes user `on_receive` passing contiguous span object
    
    Note left of Strand: Connection explicitly loops back identically reading for the NEXT header byte.
    Target->>Strand: start_receive_header() (Resets State Machine)
```

---

## 4. Message Dispatcher Mechanics

To prevent the User from having to write massive chaotic `switch/case` byte casting cascades identically evaluating different data types, `librim` features the [MessageDispatcher](file:///home/urbana/Projects/librim/include/librim/message_dispatcher.hpp#22-167).

The Dispatcher is explicitly initialized with two templates: the custom `Enum` identifying packet classes securely, and the custom [Header](file:///home/urbana/Projects/librim/include/librim/async_logger.hpp#28-34) formatting defining the protocol natively.

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
All [close()](file:///home/urbana/Projects/librim/include/librim/udp_endpoint.hpp#249-266) hooks are explicitly redirected into the protective Strand execution layer natively via `boost::asio::dispatch(strand_, ...)`. This guarantees the asynchronous loop physically finishes resolving the active network block logic entirely before deleting the connection handles gracefully!
