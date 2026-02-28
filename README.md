# Mini-SeaStar: High-Performance Asynchronous C++ Framework

Mini-SeaStar is a lightweight, high-performance C++ asynchronous networking framework inspired by the Seastar engine. It implements a Shared-Nothing and Thread-per-Core architecture, designed to eliminate lock contention and maximize hardware utilization on modern multi-core systems.

## This was independently replicated by the author based on Seastar, and is provided solely for learning purposes.

## 📂 Architecture & File Breakdown

### 1. Core Engine (The Reactor)

Seastar.h: The framework entry point. It handles the Engine initialization, spawns threads based on hardware concurrency, and uses pthread_setaffinity_np to pin each thread to a specific CPU core. This ensures cache locality and prevents OS thread migration.

Reactor.h / .cpp: The heart of each thread. It encapsulates a non-blocking Epoll event loop. It manages I/O events, high-resolution timers (timerfd), and a task scheduler (pending_tasks) for executing asynchronous callbacks.

SpscQueue.h: The cross-core highway. A lock-free Single-Producer Single-Consumer queue with power-of-two capacity and shadow indices to minimize cache-line bouncing. It is the only way cores communicate, maintaining the "Shared-Nothing" promise.

### 2. Memory & Object Lifecycle

Poolable.h: Implements a Thread-Local Slab Allocator. It provides $O(1)$ memory allocation for high-frequency objects (like TcpConnection and Promise), bypassing the global heap lock and reducing fragmentation.

IntrusivePtr.h: Defines LocalPtr (a non-atomic intrusive smart pointer) and RefCounted. By moving the counter inside the object and removing atomic increments, we eliminate bus-lock overhead during reference counting.

Packet.h: The Zero-Copy primitive. It manages byte buffers using shared_ptr<char[]> with offsets and lengths. It supports share() for broadcasting and slice() for protocol parsing without a single memcpy.

### 3. Asynchronous Primitives

Future.h: Provides Promise and Future for chainable asynchronous programming. It supports the .then() syntax, allowing complex I/O logic to be written in a linear, non-blocking style.

### 4. Networking Layer

Socket.h: A RAII wrapper for Linux sockets. It handles SO_REUSEPORT for multi-core listening and TCP_NODELAY for low-latency response.

TcpServer.h: Listens for connections and dispatches raw Socket handles to the user-defined handler.

TcpConnection.h: Manages the lifecycle of a TCP session. It handles Edge-Triggered (ET) events and implements the drain_socket logic to read data until EAGAIN.

## 📈 Evolutionary Milestones: V1 to V3

We didn't reach 450k+ QPS in one day. Here is how the project evolved:

### V1: The Baseline (Functional)

Model: Standard Reactor with std::shared_ptr.

I/O: Level-Triggered (LT) Epoll.

Bottleneck: Excessive epoll_ctl syscalls (one for every read/write cycle) and atomic reference counting overhead.

### V2: Throughput Optimization (The Syscall Killer)

Transition to ET: Moved to Edge-Triggered (ET) mode. By looping until EAGAIN and keeping FDs in the interest list, we reduced epoll_ctl calls by ~90%.

Zero-Copy Introduction: Introduced Packet slicing. Parsing headers no longer required copying data.

Lock-Free Sync: Replaced std::mutex in the task queue with the SpscQueue, enabling high-speed cross-core task submission.

### V3: Performance Perfection (The Memory & Atomic Killer)

Slab Allocation: Implemented Poolable memory pools. TcpConnection objects no longer trigger malloc, eliminating global heap contention.

Intrusive Pointers: Replaced std::shared_ptr with LocalPtr. We removed the atomic instructions (lock xadd) from the hot path, as objects stay within a single thread.

CPU Pinning: Finalized hardware affinity settings to ensure L1/L2 caches remain "hot" for the running reactor.

## 🏎️ Benchmarking

To reproduce the performance results:

Compile with full optimization:

g++ -O3 main.cpp Reactor.cpp -o mini_seastar -lpthread


Run with core isolation:

## Pressure test with wrk:

wrk -t4 -c1000 -d30s http://localhost:8080

## benchmark
Due to the author's insufficient capabilities, there is still a considerable gap compared to the mainstream technologies in the industry such as Boost.asio, Seastar, and the Go language.

Within a 16-core Docker container, Mini-Seastar can achieve over 450k QPS, Boost.asio can reach over 700k QPS, and the Go language can achieve over 650k QPS.
