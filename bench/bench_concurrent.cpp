// Phase 4 benchmarks: the cost of the concurrency layer.
//
//   * RoundTrip/Concurrent  -- one producer thread does synchronous
//     add+cancel round trips through ConcurrentBook (ring push + busy-spin
//     response wait + matcher work). This is the honest end-to-end latency
//     number: everything a client thread pays per operation.
//   * RoundTrip/Direct      -- the same add+cancel work directly on
//     FastOrderBook from the same thread, for contrast. The gap between the
//     two is the cost of the ring + mailbox + matcher hop.
//   * Throughput/Batched    -- the producer enqueues a batch of adds and
//     cancels without waiting per op, then waits once for the whole batch.
//     This is the Disruptor-style batching win: amortize the synchronization
//     over many ops instead of paying it per op.
//
// All numbers use real time (UseRealTime) because the interesting part is
// wall-clock latency as seen by the producer, which includes time spent on
// the matcher thread.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "engine/concurrent_book.hpp"
#include "engine/fast_order_book.hpp"

using namespace engine;

namespace {

// --- Workload A: synchronous add+cancel round trips -------------------------

template <bool Concurrent>
void BM_RoundTripAddCancel(benchmark::State& state) {
    const int64_t n = state.range(0);
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        ids.push_back(static_cast<OrderId>(i + 1));
    }

    if constexpr (Concurrent) {
        ConcurrentBook book(2, 1024);
        for (auto _ : state) {
            for (OrderId id : ids) {
                book.addLimitOrder(id, Side::Buy, 10000 + static_cast<Price>(id % 1000), 10);
            }
            for (OrderId id : ids) {
                book.cancel(id);
            }
        }
        book.stop();
    } else {
        FastOrderBook book;
        for (auto _ : state) {
            for (OrderId id : ids) {
                book.addLimitOrder(id, Side::Buy, 10000 + static_cast<Price>(id % 1000), 10);
            }
            for (OrderId id : ids) {
                book.cancel(id);
            }
        }
    }
    state.SetItemsProcessed(state.iterations() * 2 * n);
}

// --- Workload B: batched (pipelined) ingestion ------------------------------

void BM_BatchedThroughput(benchmark::State& state) {
    const int64_t n = state.range(0);
    ConcurrentBook book(2, 1u << 16); // big ring so a batch never blocks
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        ids.push_back(static_cast<OrderId>(i + 1));
    }

    for (auto _ : state) {
        // Fire-and-forget the whole batch: n resting adds, then n cancels.
        for (OrderId id : ids) {
            book.enqueue(id, Side::Buy, 10000 + static_cast<Price>(id % 1000), 10);
        }
        for (OrderId id : ids) {
            book.enqueueCancel(id);
        }
        book.waitForLastOp(); // one wait for the entire batch
    }
    state.SetItemsProcessed(state.iterations() * 2 * n);
    book.stop();
}

} // namespace

BENCHMARK_TEMPLATE(BM_RoundTripAddCancel, false)
    ->Name("RoundTripAddCancel/Direct")
    ->UseRealTime()
    ->RangeMultiplier(2)
    ->Range(64, 8192);
BENCHMARK_TEMPLATE(BM_RoundTripAddCancel, true)
    ->Name("RoundTripAddCancel/Concurrent")
    ->UseRealTime()
    ->RangeMultiplier(2)
    ->Range(64, 8192);

BENCHMARK(BM_BatchedThroughput)
    ->Name("ThroughputBatched/Concurrent")
    ->UseRealTime()
    ->RangeMultiplier(2)
    ->Range(64, 8192);

BENCHMARK_MAIN();
