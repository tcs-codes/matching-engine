# Low-Latency Order Matching Engine

A high-performance limit order book / matching engine written in C++20. Strict price-time priority matching with partial fills, IOC orders, cancel-replace, a single-writer concurrency model, FIX-like protocol support, and a TCP market-data feed.

## Features

- **Price-time priority matching** — the book is never left in a crossed state (`best_bid < best_ask`)
- **Order types** — Limit, IOC (Immediate-Or-Cancel), Cancel, Cancel-Replace
- **Performance-optimized internals** — flat price-indexed arrays, intrusive linked lists in a fixed-capacity arena pool (zero hot-path allocation), open-addressing cancel index, per-side bitsets for fast level traversal
- **Single-writer concurrency** — one matching thread owns the book; producer threads submit via lock-free SPSC rings; busy-spin polling, no OS wakeups on the critical path
- **FIX-like protocol layer** — `NewOrderSingle (35=D)`, `OrderCancelRequest (35=F)`, `OrderCancelReplaceRequest (35=G)` inbound; `ExecutionReport (35=8)` outbound; strict checksum + BodyLength validation
- **Market-data feed** — matcher-side tap publishes fills and top-of-book into a lock-free ring; TCP server broadcasts to connected clients
- **Latency measurement** — HdrHistogram-based reporting with rdtsc calibration, warmup, and optional core pinning
- **Deterministic op-log** — `OrderStream` generates reproducible synthetic order traffic from a seed, with dump/parse round-trip fidelity for recording and replaying sessions
- **Profile-Guided Optimization** plumbing (`-DENGINE_PGO=GENERATE` / `USE`)
- **Comprehensive test suite** — 13 test files covering book correctness, differential fuzzing, IOC/cancel-replace edge cases, SPSC ring, flat hash map, FIX protocol, market data, order stream, and latency histogram

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # or: ./build/engine_tests
```

GoogleTest, Google Benchmark, and HdrHistogram are fetched automatically via `FetchContent` — no manual install needed.

To skip optional targets:

```bash
cmake -S . -B build -DENGINE_BUILD_BENCHMARKS=OFF -DENGINE_BUILD_LATENCY_TOOL=OFF
```

### Executables

| Target | Description |
|---|---|
| `engine_bench` | Data-structure throughput (baseline `OrderBook` vs optimized `FastOrderBook`) |
| `engine_bench_concurrent` | Concurrency round-trip latency + batched throughput |
| `engine_latency` | HdrHistogram latency reporting (`--ops 1000000`) |
| `engine_feed` | TCP market-data server (`--rate 2000`) and client (`--client`) |
| `latency_tests` | Latency histogram + order-stream unit tests (requires `ENGINE_BUILD_LATENCY_TOOL`) |

### PGO builds (GCC/Clang)

```bash
cmake -S . -B build-pgo -DENGINE_PGO=GENERATE && cmake --build build-pgo -j
./build-pgo/engine_bench --benchmark_min_time=0.05s   # training run
cmake -S . -B build-pgo -DENGINE_PGO=USE && cmake --build build-pgo -j
```

## Benchmarks

Measured on an 8-core dev machine with `n = 8192` orders per round (Release build):

| Workload | `OrderBook` (baseline) | `FastOrderBook` | Speedup |
|---|---|---|---|
| Add + cancel | 1.53M ops/s | 12.15M ops/s | 7.9× |
| Buy sweeps asks | 808k rounds/s | 5.04M rounds/s | 6.2× |
| Mixed round | 1.17M ops/s | 8.99M ops/s | 7.7× |

**Concurrent round-trip** (1 producer, sync): 774k ops/s (~1.29 µs). **Batched**: 2.33M ops/s (~430 ns).

**Latency** (1M ops, 1 producer, matcher unpinned): p50 = 1.01 µs, p99 = 3.02 µs, p9999 = 152 µs. Tail is OS scheduling jitter on a shared laptop, not the engine.

Re-run locally with `./build/engine_bench` and `./build/engine_bench_concurrent`.

## Project Layout

```
include/engine/              Public headers
  order.hpp                  Order struct
  types.hpp                  Price, Quantity, OrderId, Sequence, Side, OrderType
  trade.hpp                  Fill, CancelResult, ReplaceResult, CancelReplaceOutcome
  order_book.hpp             Baseline book (std::map + std::list)
  fast_order_book.hpp        Optimized book (flat array + intrusive list + arena pool)
  concurrent_book.hpp        Single-writer matcher + SPSC rings + response mailboxes
  spsc_ring.hpp              Lock-free SPSC ring
  flat_hash_map.hpp          Open-addressing hash map
  fix_like.hpp               FIX 4.2 subset parser/builder
  market_data.hpp            Market-data tap + SPSC ring sink
  net.hpp                    Portable TCP wrapper (WinSock / POSIX)
  cycle_clock.hpp            rdtsc with calibration
  latency_histogram.hpp      HdrHistogram RAII wrapper
  order_stream.hpp           Synthetic order feed + op-log dump/replay

src/                         Implementations + CLI demo
tools/
  latency_bench.cpp          Latency measurement tool
  feed_server.cpp            TCP market-data server + client

tests/
  test_order_book.cpp        Baseline book unit tests
  test_fast_book.cpp         Optimized book unit tests
  test_invariants.cpp        Book invariant checks (crossed-book, etc.)
  test_differential.cpp      Differential fuzz: both books on same op stream
  test_ioc.cpp               IOC order edge cases
  test_cancel_replace.cpp    Cancel-replace edge cases
  test_concurrent_book.cpp   Concurrent stress tests
  test_spsc_ring.cpp         Lock-free SPSC ring tests
  test_flat_hash_map.cpp     Open-addressing hash map tests
  test_fix_like.cpp          FIX protocol parser/builder tests
  test_market_data.cpp       Market-data tap + broadcast tests
  test_latency_histogram.cpp Latency histogram stats + merge (requires HdrHistogram)
  test_order_stream.cpp      Op-log generation, round-trip, replay equivalence
bench/                       Google Benchmark harnesses
```

## Architecture

**Matching algorithm**: Price-time priority, partial fills, IOC, cancel-replace. The algorithm is decoupled from the data structures — a differential fuzz test feeds the same op stream to both books and asserts byte-for-byte agreement after every operation.

**Data structures**:

| Component | Implementation |
|---|---|
| Price levels | Flat `vector<PriceLevel>` indexed by `(price - min_price)` — O(1) lookup |
| Non-empty level walk | Per-side bitset (`uint64_t` words) — skips 64 empty levels per word via `ctz` |
| FIFO queue per level | Intrusive doubly-linked list of pool indices — O(1) cancel, no per-node allocation |
| Order storage | Fixed-capacity arena `OrderPool` with free-list — zero allocation in the hot path |
| Cancel index | `FlatHashMap` — open-addressing, linear probing, backward-shift erase |

**Concurrency model**:

| Component | Implementation |
|---|---|
| Matching thread | Single writer owns the book — no locks or atomics on the book itself |
| Producer ingestion | One `SPSCRing<OrderOp>` per producer — lock-free, cheapest cross-thread primitive |
| Response path | Atomic sequence number + result buffer per producer — no second ring needed |
| Polling | Busy-spin with `cpuPause()` backoff — no scheduler latency on the critical thread |
| Core pinning | Best-effort (POSIX/Windows APIs) — isolates matcher from OS scheduling jitter |

**Protocol**: FIX 4.2 subset with `|` instead of SOH for terminal readability. Validation is as strict as real FIX: exact `BodyLength` check and a recomputed byte-sum `CheckSum`. ClOrdIDs map deterministically to engine ids via splitmix64.
