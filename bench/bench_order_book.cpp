// Google Benchmark harness: OrderBook (Phase 1/2 baseline) vs. FastOrderBook
// (Phase 3) on three representative workloads.
//
// Both books expose an identical public API, so each workload is written once
// as a template over the book type and instantiated for both. Every workload
// is a "round" that returns the book to a clean state, so benchmark
// iterations are independent and repeatable:
//
//   * AddCancelResting  -- add N resting orders, then cancel all N. Measures
//     the insert-into-level + location-index path with no matching at all.
//   * SweepMatch        -- rest N asks, then one big buy that fills every one
//     of them. Measures the matching loop end to end (level walking, partial
//     then full fills, level removal, cursor advancement).
//   * Mixed             -- a round combining resting adds on both sides,
//     cancels, and an IOC order that sweeps the ask side. Representative of
//     the mixed op stream the randomized stress test throws at the books.
//
// Build & run:
//   cmake -S . -B build && cmake --build build -j
//   ./build/engine_bench            # or engine_bench.exe on Windows
//   ./build/engine_bench --benchmark_filter=SweepMatch

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "engine/fast_order_book.hpp"
#include "engine/order_book.hpp"

using namespace engine;

namespace {

// --- Workload 1: add N resting orders, then cancel all N ---------------------
template <typename Book>
void BM_AddCancelResting(benchmark::State& state) {
    const int64_t n = state.range(0);
    Book book;
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        ids.push_back(static_cast<OrderId>(i + 1));
    }

    for (auto _ : state) {
        for (OrderId id : ids) {
            book.addLimitOrder(id, Side::Buy, 10000 + static_cast<Price>(id % 1000), 10);
        }
        for (OrderId id : ids) {
            book.cancel(id);
        }
    }
    state.SetItemsProcessed(state.iterations() * 2 * n);
}

// --- Workload 2: one big buy sweeps N resting asks --------------------------
template <typename Book>
void BM_SweepMatch(benchmark::State& state) {
    const int64_t n = state.range(0);
    Book book;
    std::vector<OrderId> ids;
    ids.reserve(static_cast<std::size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        ids.push_back(static_cast<OrderId>(i + 1));
    }

    for (auto _ : state) {
        for (OrderId id : ids) {
            book.addLimitOrder(id, Side::Sell, 10000 + static_cast<Price>(id % 1000), 10);
        }
        // Priced above every resting ask; quantity matches the total exactly,
        // so the buy fully fills and never rests. All ids are released.
        const auto fills =
            book.addLimitOrder(static_cast<OrderId>(n + 1), Side::Buy, 11000, 10 * n);
        benchmark::DoNotOptimize(fills.size());
    }
    state.SetItemsProcessed(state.iterations() * n);
}

// --- Workload 3: mixed round (resting adds, cancels, IOC sweep) -------------
template <typename Book>
void BM_Mixed(benchmark::State& state) {
    const int64_t n = state.range(0);
    Book book;

    for (auto _ : state) {
        // Resting asks at 20000..20000+n-1 (never crossed by the bids below).
        for (int64_t i = 0; i < n; ++i) {
            book.addLimitOrder(static_cast<OrderId>(i + 1), Side::Sell, 20000 + i, 10);
        }
        // Resting bids at 10000..10000+n-1, far below the asks.
        for (int64_t i = 0; i < n; ++i) {
            book.addLimitOrder(static_cast<OrderId>(n + 1 + i), Side::Buy, 10000 + i, 10);
        }
        // Cancel every other bid (n/2 cancels).
        for (int64_t i = 0; i < n; i += 2) {
            book.cancel(static_cast<OrderId>(n + 1 + i));
        }
        // One IOC buy sweeps all remaining asks; leftover is discarded.
        const auto fills = book.addIOCOrder(static_cast<OrderId>(2 * n + 1), Side::Buy, 30000,
                                            10 * n);
        benchmark::DoNotOptimize(fills.size());
        // Cancel the bids that survived the earlier cancels.
        for (int64_t i = 1; i < n; i += 2) {
            book.cancel(static_cast<OrderId>(n + 1 + i));
        }
    }
    state.SetItemsProcessed(state.iterations() * (4 * n));
}

} // namespace

BENCHMARK_TEMPLATE(BM_AddCancelResting, OrderBook)
    ->Name("AddCancelResting/OrderBook")
    ->RangeMultiplier(2)
    ->Range(64, 8192);
BENCHMARK_TEMPLATE(BM_AddCancelResting, FastOrderBook)
    ->Name("AddCancelResting/FastOrderBook")
    ->RangeMultiplier(2)
    ->Range(64, 8192);

BENCHMARK_TEMPLATE(BM_SweepMatch, OrderBook)
    ->Name("SweepMatch/OrderBook")
    ->RangeMultiplier(2)
    ->Range(64, 8192);
BENCHMARK_TEMPLATE(BM_SweepMatch, FastOrderBook)
    ->Name("SweepMatch/FastOrderBook")
    ->RangeMultiplier(2)
    ->Range(64, 8192);

BENCHMARK_TEMPLATE(BM_Mixed, OrderBook)
    ->Name("Mixed/OrderBook")
    ->RangeMultiplier(2)
    ->Range(64, 8192);
BENCHMARK_TEMPLATE(BM_Mixed, FastOrderBook)
    ->Name("Mixed/FastOrderBook")
    ->RangeMultiplier(2)
    ->Range(64, 8192);

BENCHMARK_MAIN();
