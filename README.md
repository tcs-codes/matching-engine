# Low-Latency Order Matching Engine

A limit order book / matching engine built in C++20, developed in phases:
correctness first, then performance, then concurrency. This is **Phase 6** —
the stretch phase: open-addressing cancel index, FIX-like order messaging,
market-data tap + TCP feed, PGO plumbing.

## Phase 6 scope (this repo, right now)

- **`FlatHashMap`** (`flat_hash_map.hpp`) — a from-scratch open-addressing
  map (linear probing, backward-shift erase) that replaces
  `std::unordered_map` as `FastOrderBook`'s id → location cancel index.
  Same semantics, but no per-node heap allocation: every insert/erase in the
  add/cancel path was doing a malloc/free, which the benchmarks show was a
  large share of the cost.
- **~3× faster than Phase 4 across every workload** from that swap alone
  (details below) — now **~5.5-7.6× over the original Phase 1/2 `std::map`
  baseline**. The matching hot path also frees per-fill map nodes, so
  removing heap allocation from erase helps everywhere, not just cancels.
- **Branch hints** — `[[likely]]`/`[[unlikely]]` on the hot-path
  conditions (e.g. "order at best price" is the common case).
- **FIX-like protocol layer** (`fix_like.hpp`) — a deliberate subset of
  FIX 4.2: `NewOrderSingle (35=D)`, `OrderCancelRequest (35=F)`,
  `OrderCancelReplaceRequest (35=G)` inbound, `ExecutionReport (35=8)`
  outbound. Fields use `|` instead of SOH so messages are terminal-friendly,
  but validation is exactly as strict as real FIX: a recomputed 8-byte sum
  checksum (the standard FIX algorithm) and an exact BodyLength check, so a
  tampered message is rejected. ClOrdIDs map deterministically to engine ids
  via splitmix64.
- **Market-data tap** (`market_data.hpp`) — the matcher is the single
  source of truth, so it publishes each fill plus the resulting best bid/ask
  into a `RingMarketDataSink` (an `SPSCRing`). The ring is intentionally
  *lossy* under overload (drop an old book update rather than stall the
  matching hot path); production feeds would backpressure instead.
- **`engine_feed`** — a TCP market-data server + streaming client
  (`tools/feed_server.cpp`): the matcher feed flows through the sink into a
  publisher thread that broadcasts top-of-book (`B <bid> <ask>`) and trade
  (`T <px> <qty> <bid> <ask>`) lines to every connected client, with an
  optional `--rate` to pace submissions so the book is watchable, and
  `--replay FILE` to feed a Phase 5 op log.
- **PGO plumbing** (`-DENGINE_PGO=GENERATE` / `USE`) — and the honest
  result: on this dev machine the PGO build was **within noise (±5%) of the
  plain Release build** on every workload. These hot paths are branch-light
  single-writer loops, so profile-guided layout has little to grab — a
  legitimate finding to report, not a failure.

### Phase 6 numbers (Release build, 8-core dev machine, n = 8192)

| Workload | OrderBook (Ph 1/2) | FastOrderBook Ph 4 | FastOrderBook now | vs. baseline |
|---|---|---|---|---|
| Add + cancel | 1.53M ops/s | 3.74M ops/s | 12.15M ops/s | 7.9× |
| Buy sweeps asks | 808k rounds/s | 1.63M rounds/s | 5.04M rounds/s | 6.2× |
| Mixed round | 1.17M ops/s | 3.37M ops/s | 8.99M ops/s | 7.7× |

Re-run locally with `./build/engine_bench`. The open-addressing map is the
Phase 6 story: ~3× from removing per-node malloc/free, on top of the ~2-3×
Phase 3 already got from the flat book + arena pool.

## Phase 1 scope

- Single-threaded limit order book, bids and asks.
- Strict price-time priority matching.
- `addLimitOrder` matches against the opposite side immediately; any
  unfilled remainder rests on the book.
- Partial fills: resting orders shrink in place and keep their queue
  position; incoming orders rest with the leftover quantity.
- `cancel(order_id)` by id, O(1) average via an id → location index.
- Invariant: the book is never left in a crossed state
  (`best_bid < best_ask` whenever both sides are non-empty).

## Phase 5 scope (this repo, right now)

- **`engine_latency`** — a latency measurement tool that drives
  `ConcurrentBook` and reports the synchronous round-trip a producer thread
  actually pays (ring push + busy-spin response wait + matcher work) as an
  **HdrHistogram**: mean / p50 / p90 / p99 / p999 / p9999 / max, in
  nanoseconds and TSC cycles. Uses the real HdrHistogram C library (Gil
  Tene's standard tool for latency work), fetched via CMake and wrapped in a
  small RAII class.
- **`CycleClock`** — rdtsc with a one-time calibration against a
  steady_clock window (the TSC is invariant on modern CPUs); falls back to
  steady_clock on non-x86 so reporting code stays uniform.
- **`OpStreamGenerator`** — a deterministic synthetic order feed with the
  properties that make a book interesting: prices cluster near a mid that
  performs a bounded random walk (locality), most adds are passive, a
  configurable fraction are aggressive (priced to cross), and cancels /
  cancel-replaces target believed-live ids. Same seed ⇒ bit-identical stream.
- **Op log + replay** — a one-line-per-op text format (`L`/`I`/`C`/`R`), so
  a generated session can be dumped, saved, and re-measured later:
  `engine_latency --dump session.log`, then `engine_latency --replay
  session.log`. The replay test proves a dumped+replayed stream reproduces
  the exact book state of applying the original stream.
- **Measurement rigor, the honest way**: per-producer histograms merged
  exactly at the end (hdr_add is bucket-exact), a configurable warmup that
  skips cold-cache / branch-predictor / ring-warmup ops, the calibrated TSC
  rate reported alongside every result, and an optional matcher core pin.

### Phase 5 numbers (Release build, 8-logical-core dev machine, 1M ops)

| Producer setup | p50 | p90 | p99 | p999 | p9999 | max |
|---|---|---|---|---|---|---|
| 1 producer, matcher unpinned | 1.01 µs | 1.89 µs | 3.02 µs | 34 µs | 152 µs | 4.5 ms |
| 1 producer, matcher pinned core 0 | 0.96 µs | — | 4.1 µs | 39 µs | 346 µs | 7.8 ms |
| 4 producers, matcher unpinned | 2.73 µs | — | 22.7 µs | 181 µs | 1.6 ms | 13 ms |

The shape is the story: the median round trip is ~1 µs, and everything past
p99 is OS scheduling jitter (timer interrupts, preemption) on a shared
laptop — not the engine. Note the pinning row: pinning the matcher to core 0
*tightened the median* but *worsened the tail*, because core 0 is a busy
system core on this machine. Pinning only pays off when the core is actually
isolated from the OS (`isolcpus`), which is exactly the kind of caveat worth
knowing for an interview. Re-run locally:
`./build/engine_latency --ops 1000000 --producers 1`.

## Phase 4 scope (done)

- **`ConcurrentBook`** — the single-writer design. One dedicated matching
  thread owns a `FastOrderBook`; no locks, no atomics on the book itself.
  Client (producer) threads never touch the book: each producer gets its own
  lock-free **SPSC ring** (`SPSCRing<T>`) of order ops, the matcher thread
  **busy-spins** polling every ring (no condition variables, no OS wakeups),
  and results come back through a lock-free per-producer response mailbox
  (an atomic sequence number + result buffer).
- **`SPSCRing<T>`** — the lock-free single-producer/single-consumer ring:
  two monotonic counters, one acquire-load + one release-store per push/pop,
  cache-line-padded to avoid false sharing, capacity must be a power of two,
  payload must be trivially copyable. Nothing lost, nothing reordered — the
  concurrent stress test pushes 200k items through a 1024-slot ring.
- **Best-effort core pinning** for the matcher thread (`SetThreadAffinityMask`
  on Windows, `pthread_setaffinity_np` on POSIX) — a latency optimization,
  never a correctness requirement.
- **Two client modes**: a synchronous API with the exact same semantics as
  the sequential books (add/cancel/replace/queries, thread-safe from any
  producer thread), and a pipelined `enqueue`/`waitForLastOp()` mode for
  batching.
- **Tests**: SPSC ring stress; a differential test (single producer, same
  randomized op stream vs. `FastOrderBook`, full agreement after every op —
  deterministic because one producer means FIFO application); a multi-
  producer volume-conservation test where 4 producers hammer the book on
  disjoint id ranges but overlapping prices (so they match against each
  other) and a global inserted − filled − cancelled = resting identity must
  hold exactly.

### Phase 4 numbers (Release build, 8-core dev machine, n = 8192 orders/round)

| Mode | Throughput | Per op |
|---|---|---|
| `FastOrderBook` direct (baseline) | 3.05M ops/s | ~330 ns |
| `ConcurrentBook` synchronous round trip | 774k ops/s | ~1.29 µs |
| `ConcurrentBook` pipelined (batched) | 2.33M ops/s | ~430 ns |

Batching recovers most of the per-op synchronization cost: ~3× faster than
per-op round trips, within ~25% of the direct single-threaded book. The
synchronous round-trip number (~1.3 µs) is the honest end-to-end latency a
client thread pays per operation — the ring hop + busy-spin wait + matcher
work — and is the number Phase 5's latency benchmarking will dissect with
HDR histograms. Re-run locally with `./build/engine_bench_concurrent`.

## Phase 3 scope (done)

- **`FastOrderBook`** — the same matching engine over data structures chosen
  for speed: a flat, price-indexed array instead of `std::map`, an intrusive
  doubly-linked list (pool indices, not pointers) instead of `std::list`, and
  a fixed-capacity arena/free-list pool so the hot path never allocates.
  Same public API as the Phase 1/2 `OrderBook`, same semantics.
- **Differential fuzz test** (`test_differential.cpp`): feeds a byte-identical
  5000-operation randomized stream (adds, cancels, IOC, cancel-replace) to
  both books and asserts full agreement — every Fill, every result, best
  bid/ask, level counts, quantity and order count at every touched price,
  and existence/quantity of every id ever issued — after every operation.
  This is the correctness proof for the rewrite: it's the same algorithm
  over different containers, so any divergence is a bug.
- **Google Benchmark harness** (`engine_bench`): three representative
  workloads (resting add+cancel, sweep match, mixed IOC/cancel round) run
  against both books. Result on the dev machine: **~1.7-2.9× faster** than
  the `std::map` baseline across the board (details below).

### Phase 3 numbers (Release build, 8-core dev machine)

| Workload | OrderBook | FastOrderBook | Speedup |
|---|---|---|---|
| Add + cancel, 8192 orders | 1.53M ops/s | 3.74M ops/s | 2.4× |
| Buy sweeps 8192 asks | 808k rounds/s | 1.63M rounds/s | 2.0× |
| Mixed round, 8192 orders | 1.17M ops/s | 3.37M ops/s | 2.9× |

Re-run locally with `./build/engine_bench` (or `engine_bench.exe`). The two
optimizations that mattered most: a per-side **bitset of non-empty levels**
so the "advance to the next price level" walk skips empty levels 64 at a
time instead of one struct at a time, and a **modestly-reserved hash index**
(reserving to arena capacity put every lookup on a cold cache line).

## Phase 2 scope (done)

- **IOC (Immediate-Or-Cancel) orders**: `addIOCOrder(...)` matches as much as
  possible right now and discards any unfilled remainder instead of
  resting it. Implemented as the same matching loop as `Limit`, just with
  a different "what to do with the leftover" policy — see `addOrder` in
  `order_book.hpp`.
- **Cancel-replace (modify)**: `cancelReplace(id, new_price, new_qty)`
  changes a resting order's price and/or quantity in place, keeping the
  same order id. Time-priority rule (mirrors real exchange behavior):
  - Pure size **decrease** at the **same price** → priority preserved,
    modified in place, no re-matching (a smaller order at an unchanged
    price can't newly cross).
  - Any **price change**, or any size **increase** → priority lost: the
    order is cancelled and reinserted as if newly submitted (fresh
    sequence number, goes to the back of the queue, matched against the
    opposite side again — so it may fill immediately if the new price
    now crosses).
  
  This "increase loses priority" rule exists so an order can't grow its
  size for free while keeping its queue position — a well-known gaming
  vector if left unguarded.
- Extended the randomized invariant stress test to interleave adds,
  cancels, IOC orders, and cancel-replaces (5000 iterations, fixed seed),
  asserting the book is never crossed and volume is conserved after every
  single operation.

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # or: ./build/engine_tests
./build/engine_bench                        # Google Benchmark: Phase 3/6 (data structures)
./build/engine_bench_concurrent             # Google Benchmark: Phase 4 (concurrency)
./build/engine_latency --ops 1000000        # Phase 5: HDR latency histograms
./build/engine_feed --rate 2000             # Phase 6: serve the market-data feed
./build/engine_feed --client                # ...and watch it live (second terminal)
```

GoogleTest, Google Benchmark, and HdrHistogram are pulled automatically via
`FetchContent` — no manual install needed. To skip the benchmark harnesses
or the latency tool, configure with `-DENGINE_BUILD_BENCHMARKS=OFF` /
`-DENGINE_BUILD_LATENCY_TOOL=OFF`.

PGO builds (GCC/Clang, two phases — train with a benchmark run, then
rebuild):

```bash
cmake -S . -B build-pgo -DENGINE_PGO=GENERATE && cmake --build build-pgo -j
./build-pgo/engine_bench --benchmark_min_time=0.05s   # training run
cmake -S . -B build-pgo -DENGINE_PGO=USE && cmake --build build-pgo -j
```

## Layout

```
include/engine/   public headers (types, Order, Fill, OrderBook, FastOrderBook,
                  SPSCRing, ConcurrentBook, CycleClock, LatencyHistogram,
                  OrderStreamGenerator, FlatHashMap, FixLike, MarketDataSink,
                  Net)
src/               implementations + CLI demo
  order_book.cpp          Phase 1/2 baseline: std::map + std::list
  fast_order_book.cpp     Phase 3: flat array + intrusive list + arena pool
  concurrent_book.cpp     Phase 4: matcher thread + SPSC rings + response mailboxes
  order_stream.cpp        Phase 5: synthetic feed generator + op-log dump/parse
  fix_like.cpp            Phase 6: FIX subset parse/validate + execution reports
  net.cpp                 Phase 6: portable TCP wrapper (WinSock / POSIX)
  main.cpp                CLI demo
tools/
  latency_bench.cpp         Phase 5: engine_latency (measure / dump / replay)
  feed_server.cpp           Phase 6: engine_feed (TCP market-data server + client)
tests/             GoogleTest unit tests
  test_order_book.cpp       Phase 1: price-time priority, fills, cancel
  test_invariants.cpp       crossed-book + randomized stress test
  test_ioc.cpp              Phase 2: IOC order behavior
  test_cancel_replace.cpp   Phase 2: cancel-replace behavior
  test_fast_book.cpp        Phase 3: FastOrderBook unit tests
  test_differential.cpp     Phase 3: differential fuzz vs. baseline
  test_spsc_ring.cpp        Phase 4: SPSC ring stress tests
  test_concurrent_book.cpp  Phase 4: concurrent differential + multi-producer invariants
  test_latency_histogram.cpp  Phase 5: HdrHistogram wrapper (percentiles, merge)
  test_order_stream.cpp      Phase 5: feed determinism + op-log round trip + replay
  test_flat_hash_map.cpp     Phase 6: open-addressing map vs. std::unordered_map
  test_fix_like.cpp          Phase 6: FIX parse/validate/convert round trips
  test_market_data.cpp       Phase 6: sink sees fills + book updates in order
bench/             Google Benchmark harnesses
  bench_order_book.cpp      Phase 3/6: OrderBook vs. FastOrderBook
  bench_concurrent.cpp      Phase 4: round-trip latency + batched throughput
```

## Design decisions

Phase 1 chose correctness-first containers; Phase 3 (`FastOrderBook`) swapped
out the data structures underneath the same, already-tested algorithm.

| Choice | Phase 1/2 (baseline `OrderBook`) | Phase 3 (`FastOrderBook`) |
|---|---|---|
| Price levels | `std::map<Price, PriceLevel>` per side — sorted by construction, `begin()` is always best price | Flat `std::vector<PriceLevel>` indexed by `(price - min_price)`; O(1) level lookup, O(1) `bestBid()`/`bestAsk()` via a cursor per side |
| "Next non-empty level" walk | Map iteration — no walk needed | Per-side **bitset of non-empty levels**: skips empty levels 64 at a time, resolves a found bit with `std::countr_zero` (the simple form of the bit-trie idea) |
| FIFO queue per level | `std::list<Order>` — stable iterators for O(1) cancel from the hash index | Intrusive doubly-linked list of **pool indices** (head/tail in the level) — no per-node heap allocation, contiguous storage |
| Order storage | Heap-allocated `std::list` nodes | Fixed-capacity arena `OrderPool` with a free-list — zero allocation in the hot path; exhausting the pool throws |
| Cancel lookup | `std::unordered_map<OrderId, std::list<Order>::iterator>` | `std::unordered_map<OrderId, int32_t>` (pool index), reserved modestly so lookups stay cache-hot |
| Integer `Price` (ticks) | Exact comparisons; floating point price levels are a well-known correctness bug | Unchanged — correct from day one |
| `Sequence` counter for time priority | Insertion order, not wall-clock | Unchanged |

The matching *algorithm* (price-time priority, partial-fill semantics, Fill
event shape) is intentionally decoupled from container choice: Phase 3 was a
data-structure swap underneath an already-tested algorithm, proven equivalent
by the differential fuzz test rather than by re-deriving correctness.

Phase 4 adds the concurrency layer without touching that algorithm either:

| Choice | Why |
|---|---|
| One matching thread owns the book (single-writer) | The fastest lock is no lock: no locks or atomics on the book itself, matching is unchanged from Phases 1-3 |
| One `SPSCRing<OrderOp>` per producer thread | SPSC is the cheapest lock-free structure (two ordering points per op); multiple producers each get their own ring, all drained by the matcher — no MPSC complexity needed |
| Busy-spin polling with `cpuPause()` backoff | No condition variables / futex wakeups means no scheduler latency on the critical thread; `_mm_pause` on x86 avoids pipeline thrash while spinning |
| Response mailboxes (atomic seq + result buffer) | Lock-free results without a second ring; the producer spins until its op's seq appears (the `>=` compare also supports pipelining) |
| Best-effort core pinning | Isolates the matcher from OS scheduling jitter; best-effort so tests never depend on it |

Phase 5 adds the measurement layer:

| Choice | Why |
|---|---|
| HdrHistogram (Gil Tene's C library) | The standard latency tool in low-latency trading; ~3 significant figures across the whole range, exact bucket-wise merge for per-producer histograms |
| rdtsc via `CycleClock` | Cycle-accurate, sub-ns resolution, near-zero overhead; calibrated once against steady_clock because the TSC is invariant on modern CPUs |
| Per-op synchronous round trip as the unit of measurement | It's the honest end-to-end cost a client thread pays (ring + spin + matcher), not just the book's own cost |
| Warmup + reported TSC rate + optional pinning | The reproducibility checklist: skip cold-start ops, report the calibration, and never claim pinning helps without an isolated core |

Phase 6 adds the final layer:

| Choice | Why |
|---|---|
| Open-addressing `FlatHashMap` for the cancel index | `std::unordered_map` mallocs a node per insert and frees per erase — the add/cancel path was paying heap allocation on every operation. Linear probing + backward-shift erase is cache-friendly and allocates nothing; ~3× on all workloads |
| Strict FIX checksum + BodyLength validation | Tampered messages are the failure mode a gateway must catch; the standard FIX byte-sum algorithm and an exact body-length check make the parser as strict as the real thing (and the round-trip tests prove `makeFixMessage` ↔ `parseFixMessage` agree) |
| Matcher-side market-data tap into an SPSC ring | The matcher already knows every fill and the true top of book; publishing from there means the feed can't disagree with the book. Lossy-by-design under overload keeps the matching hot path unblocked |
| `|` instead of SOH as the field separator | Terminal-friendly and printable while keeping validation strict — a documented deviation, not a sloppy one |

## Roadmap

- **Phase 2**: ✅ order types (IOC, cancel-replace), extended property-based
  invariant tests (no crossed book, volume conservation across a mixed
  add/cancel/IOC/replace op stream).
- **Phase 3**: ✅ `FastOrderBook` — flat price-indexed array book, intrusive
  doubly-linked lists in an arena pool, per-side bitset of non-empty levels,
  differential fuzz test vs. the baseline, Google Benchmark harness
  (~1.7-2.9× vs. `std::map` baseline on the dev machine).
- **Phase 4**: ✅ `ConcurrentBook` — single-writer matcher thread, lock-free
  SPSC rings for ingestion, busy-spin polling, response mailboxes, core
  pinning; differential + multi-producer invariant tests; round-trip latency
  and batched-throughput benchmarks (batching ≈ 3× vs. per-op round trips).
- **Phase 5**: ✅ `engine_latency` — HdrHistogram latency reporting
  (p50/p99/p999/p9999, rdtsc + calibration, warmup), deterministic synthetic
  order feed with price locality, op-log dump/replay. Found: median round
  trip ~1 µs; tail is OS jitter, and pinning needs an isolated core to help.
- **Phase 6**: ✅ open-addressing `FlatHashMap` cancel index (~3× more),
  branch hints, FIX-like order layer (D/F/G in, execution reports out,
  strict checksum + BodyLength), market-data tap + `engine_feed` TCP
  server/client, PGO plumbing (measured: within noise here — reported
  honestly). The roadmap is complete; further gains need kernel-bypass
  networking and dedicated hardware (the defensible stopping point).
