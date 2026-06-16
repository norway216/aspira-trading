# Aspira Trading Engine

A **high-performance, low-latency trading system** written in C and C++20, designed for market making, HFT, and institutional order routing.

## Performance

| Metric | Target | Measured |
|--------|--------|----------|
| Internal Latency | < 100 µs | **~12 µs** |
| Throughput | 100K–1M msg/s | 1,780 msg/s (simulation-limited) |
| Order Acceptance | — | **100%** |
| Memory Allocation (hot path) | Zero | ✓ Pre-allocated pools |
| Garbage Collection | Zero | ✓ No GC runtime |

*Measured on Intel i9-12900H (20 cores), Linux 6.17, simulated feed at 2,000 msg/s.*

## Architecture

```
┌──────────────────────┐
│   Market Data Feed   │
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐
│  Feed Handler (C)    │  ← epoll, UDP, lock-free queue
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐
│  Order Book (C++)    │  ← price-time priority matching
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐
│  Risk Engine (C++)   │  ← position, exposure, price bands
└──────────┬───────────┘
           │
           ▼
┌──────────────────────┐
│  Execution Gateway   │  ← order dispatch, ack tracking
└──────────────────────┘
```

### Event Pipeline

```
Market Data → Parser → Strategy → Risk → Order Book → Execution
                                                    ↓
                                               Journal (mmap)
```

Each stage communicates via **lock-free SPSC ring buffers** with cache-line padding. Zero-copy message passing throughout.

## Project Structure

```
aspira-trading/
├── include/                     # Public headers
│   ├── ring_buffer.h            #   Lock-free SPSC ring buffer
│   ├── memory_pool.h            #   Pre-allocated memory pool
│   ├── logger.h                 #   Async logging
│   ├── feed_handler.h           #   Market data receiver (epoll)
│   ├── order.h                  #   Order/trade data types
│   ├── order_book.h             #   Price-time priority order book
│   ├── risk_engine.h            #   Pre-trade risk validation
│   ├── execution_gateway.h      #   Order dispatch gateway
│   ├── event_pipeline.h         #   Pipeline orchestrator
│   └── journal.h                #   Persistence journal (mmap)
├── src/                         # Implementation
│   ├── ring_buffer.c            #   C — GCC __atomic builtins
│   ├── memory_pool.c            #   C — __sync fetch-and-op
│   ├── logger.c                 #   C — async writer thread
│   ├── feed_handler.c           #   C — epoll + simulated feed
│   ├── order_book.cpp           #   C++20 — std::map levels
│   ├── risk_engine.cpp          #   C++20 — multi-check validation
│   ├── execution_gateway.c      #   C — FIX-like serialization
│   ├── event_pipeline.cpp       #   C++20 — thread orchestration
│   ├── journal.c                #   C — mmap + CRC32
│   └── main.cpp                 #   Entry point
├── tests/                       # Unit tests
│   ├── test_ring_buffer.c       #   4 tests (incl. 100K threaded)
│   ├── test_memory_pool.c       #   2 tests
│   └── test_order_book.cpp      #   7 tests
├── scripts/
│   └── build_deploy.sh          # Build, test, install, package
├── bench/
│   └── benchmark.sh             # Performance benchmark suite
├── docs/
│   └── high_performance_trading_system.md
├── CMakeLists.txt
└── README.md
```

## Key Components

### Lock-Free SPSC Ring Buffer (`ring_buffer.h/c`)

The core communication primitive. Cache-line padded to prevent false sharing. Uses GCC `__atomic` builtins for C/C++ cross-language compatibility with `__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE` memory ordering.

- Power-of-2 capacity for fast modulo via bitmask
- Non-blocking push/pop — producer/consumer never wait
- Single-producer, single-consumer — no CAS loops in hot path

### Memory Pool (`memory_pool.h/c`)

Pre-allocated, fixed-size object pool. Alloc and free are O(1) stack push/pop. Uses `__sync_fetch_and_add`/`__sync_fetch_and_sub` for thread safety. No `malloc`/`free` in the hot path.

### Async Logger (`logger.h/c`)

Log messages are pushed to a lock-free ring buffer in the hot path and consumed by a dedicated background thread that writes to disk. The hot path **never blocks on I/O**.

- Nanosecond-precision timestamps (`CLOCK_REALTIME`)
- Configurable log levels (TRACE/DEBUG/INFO/WARN/ERROR)
- Drops messages silently when buffer is full (no back-pressure)

### Feed Handler (`feed_handler.h/c`)

Receives and normalizes market data. Supports:
- **Production mode**: epoll-based UDP receiver with configurable socket buffer
- **Simulation mode**: synthetic market data with random-walk pricing

### Order Book (`order_book.h/cpp`)

Price-time priority limit order book with matching engine.
- **Bids**: descending price, then time priority
- **Asks**: ascending price, then time priority
- **Matching**: aggressive orders cross the book immediately
- **Order types**: Limit and Market
- **Operations**: Add, Cancel, Modify
- O(1) order lookup via ID index

### Risk Engine (`risk_engine.h/cpp`)

Pre-trade risk validation with configurable limits:
- Maximum order size
- Maximum net position (per symbol)
- Maximum gross exposure
- Maximum notional value
- Price band (reject orders too far from mid)
- Rate limiting (orders per second)
- Master kill switch

### Execution Gateway (`execution_gateway.h/c`)

Outbound order dispatch. Simulated mode logs orders to file; production mode connects to exchanges via TCP. Supports FIX-like text protocol serialization.

### Persistence Journal (`journal.h/c`)

Append-only binary journal for audit and recovery.
- Memory-mapped files (`mmap`) for high-throughput writes
- CRC32 integrity checks on every record
- Fixed-size headers with type-specific payloads
- Records: orders (new/filled/cancelled/rejected) and trades

## Quick Start

### Prerequisites

- **Linux** (bare-metal recommended for production)
- **GCC ≥ 10** (C11 + C++20 support)
- **CMake ≥ 3.16**
- GNU Make

```bash
# Install on Ubuntu/Debian
sudo apt install build-essential cmake

# Install on RHEL/Fedora
sudo dnf install gcc gcc-c++ cmake make
```

### Build

```bash
# Release build (optimized)
cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

# Debug build (with AddressSanitizer)
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc)
```

Or use the build script:

```bash
./scripts/build_deploy.sh -t Release -T    # Build + run tests
./scripts/build_deploy.sh -t Release -p     # Build + create package
./scripts/build_deploy.sh --help            # Show all options
```

### Run Tests

```bash
./build/test_ring_buffer
./build/test_memory_pool
./build/test_order_book
```

### Run the Engine

```bash
# Run for 10 seconds with symbol AAPL
./build/trading_engine --duration 10 --symbol AAPL

# Run indefinitely (Ctrl+C to stop)
./build/trading_engine --duration 0 --symbol AAPL

# Custom log and journal paths
./build/trading_engine --duration 30 --symbol MSFT \
    --log msft_engine.log --journal msft_engine.jrnl

# Show help
./build/trading_engine --help
```

### Benchmark

```bash
# Quick benchmark (~10 seconds)
./bench/benchmark.sh --quick

# Full benchmark (3 runs × 10 seconds)
./bench/benchmark.sh -d 10 -r 3

# Multi-symbol benchmark
./bench/benchmark.sh -s "AAPL,GOOG,MSFT" -d 5 -r 2
```

The benchmark report includes latency percentiles, throughput metrics, CPU/memory usage, and a latency histogram.

## Design Decisions

### C for Low-Level, C++ for Logic

- **C** components: ring buffer, memory pool, feed handler, logger, journal, execution gateway — these require direct memory control, minimal overhead, and deterministic behavior.
- **C++20** components: order book, risk engine, event pipeline — these benefit from STL containers, RAII, and stronger type safety.

### GCC Builtins over `<stdatomic.h>`

We use GCC `__atomic_*` and `__sync_*` builtins instead of C11 `<stdatomic.h>` to ensure seamless C/C++ cross-compilation. The `_Atomic` qualifier and `<stdatomic.h>` types are not available in C++ mode, while GCC builtins work identically in both languages.

### Lock-Free over Mutex

All inter-thread communication uses lock-free ring buffers. No mutexes, no condition variables, no contention in the critical path. This guarantees deterministic latency regardless of load.

### Pre-Allocation

Memory pools are allocated at initialization. The hot path performs zero dynamic allocations — no `malloc`, no `new`, no garbage collection pauses.

## Latency Optimization Techniques

- **CPU pinning** (affinity) — assign threads to dedicated cores
- **Cache-line padding** — prevent false sharing between producer/consumer
- **Branch prediction** — hot-path checks ordered by likelihood
- **Inline hot functions** — compiler `-O3 -march=native` with LTO
- **Zero-copy** — pass pointers through ring buffers, never copy payloads
- **No syscalls in hot path** — logging and persistence are async

## Configuration

### Risk Limits

Edit `src/event_pipeline.cpp` to adjust:

```cpp
RiskLimits limits;
limits.max_order_qty     = 10000;        // Max single order size
limits.max_position      = 10000000;     // Max net position
limits.max_exposure      = 50000000;     // Max gross exposure
limits.max_notional      = 500000000.0;  // Max notional value
limits.price_band_pct    = 10.0;         // Max price deviation (%)
limits.max_orders_per_sec = 50000;       // Rate limit
limits.enabled           = true;         // Master enable
```

### Feed Simulation

Edit `src/event_pipeline.cpp`:

```cpp
feed_cfg.sim_interval_us = 500;   // 2,000 msgs/sec
feed_cfg.output_queue_size = 4096; // Ring buffer capacity
```

Set `feed_cfg.simulation_mode = false` and configure `listen_addr`/`listen_port` for live UDP feed.

## Output Files

| File | Description |
|------|-------------|
| `trading_engine.log` | Async logger output (human-readable, timestamped) |
| `execution_gateway.log` | Order dispatch records |
| `trading_engine.jrnl` | Binary persistence journal (mmap, CRC32) |

## Future Enhancements

- **FPGA** market data parsing for sub-microsecond decode
- **Kernel bypass** (DPDK / AF_XDP) for direct NIC-to-userspace DMA
- **FIX protocol** full implementation for exchange connectivity
- **Distributed matching engine** with Raft consensus for fault tolerance
- **GPU-accelerated** risk analytics and backtesting
- **Real-time kernel** (PREEMPT_RT) for deterministic scheduling
- **Multi-symbol** concurrent order books with sharded architecture

## License

See [LICENSE](LICENSE) for details.
