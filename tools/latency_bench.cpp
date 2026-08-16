// engine_latency -- Phase 5 latency measurement tool.
//
// Drives ConcurrentBook with a deterministic synthetic order stream (or a
// replayed op log) and reports the end-to-end synchronous round-trip latency
// a producer thread actually pays: ring push + busy-spin response wait +
// matcher work. Latency is captured with rdtsc (CycleClock) into an
// HdrHistogram per producer thread, then merged exactly and reported as
// mean / p50 / p90 / p99 / p999 / p9999 / max in both nanoseconds and cycles.
//
// Usage:
//   engine_latency [--ops N] [--producers P] [--matcher-core C] [--seed S]
//                  [--warmup N] [--dump FILE] [--replay FILE]
//
//   default        generate `ops` ops with a synthetic feed and measure
//   --dump FILE    write the generated op stream to FILE (no measurement)
//   --replay FILE  load ops from an op log (see order_stream.hpp) and measure
//
// Reproducibility: the matcher is pinned to `--matcher-core` when given, and
// the reported calibrated TSC rate lets you convert cycles to ns on your own
// machine.

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "engine/concurrent_book.hpp"
#include "engine/cycle_clock.hpp"
#include "engine/latency_histogram.hpp"
#include "engine/order_stream.hpp"

using namespace engine;

namespace {

struct Options {
    std::size_t ops = 1'000'000;
    std::size_t producers = 1;
    int matcher_core = -1;
    std::uint64_t seed = 1;
    std::size_t warmup = 100'000;
    bool dump = false;
    bool replay = false;
    std::string file;
};

void printUsage() {
    std::cout <<
        "usage: engine_latency [--ops N] [--producers P] [--matcher-core C]\n"
        "                      [--seed S] [--warmup N] [--dump FILE] [--replay FILE]\n"
        "\n"
        "  default        generate N synthetic ops and measure round-trip latency\n"
        "  --dump FILE    write the generated stream to FILE and exit\n"
        "  --replay FILE  measure on ops loaded from an op log\n";
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            printUsage();
            return false;
        }
        if (a == "--ops") opt.ops = std::stoull(argv[++i]);
        else if (a == "--producers") opt.producers = std::stoull(argv[++i]);
        else if (a == "--matcher-core") opt.matcher_core = std::stoi(argv[++i]);
        else if (a == "--seed") opt.seed = std::stoull(argv[++i]);
        else if (a == "--warmup") opt.warmup = std::stoull(argv[++i]);
        else if (a == "--dump") {
            opt.dump = true;
            opt.file = argv[++i];
        } else if (a == "--replay") {
            opt.replay = true;
            opt.file = argv[++i];
        } else {
            std::cerr << "engine_latency: unknown option '" << a << "'\n";
            printUsage();
            return false;
        }
    }
    return true;
}

// One producer thread: submit ops [begin, end) synchronously, timing each
// round trip in TSC cycles. The first `warmup` ops are not timed (they pay
// cold-cache / branch-predictor / ring-warmup costs that aren't steady state).
void runChunk(ConcurrentBook& book, const std::vector<OrderOp>& ops, std::size_t begin,
              std::size_t end, std::size_t warmup, LatencyHistogram& hist) {
    for (std::size_t i = begin; i < end; ++i) {
        const std::uint64_t t0 = CycleClock::now();
        book.submitOp(ops[i]);
        const std::uint64_t t1 = CycleClock::now();
        if (i - begin >= warmup) {
            hist.record(t1 - t0);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        return argc > 1 ? 1 : 0;
    }

    // --- Build the op stream ------------------------------------------------
    std::vector<OrderOp> ops;
    if (opt.replay) {
        std::ifstream in(opt.file);
        if (!in) {
            std::cerr << "engine_latency: cannot open '" << opt.file << "'\n";
            return 1;
        }
        ops = loadOps(in);
        std::cout << "loaded " << ops.size() << " ops from " << opt.file << "\n";
    } else {
        OpStreamGenerator gen(OpStreamGenerator::Config{.seed = opt.seed, .count = opt.ops});
        for (auto op = gen.next(); op; op = gen.next()) {
            ops.push_back(*op);
        }
        if (opt.dump) {
            std::ofstream out(opt.file);
            if (!out) {
                std::cerr << "engine_latency: cannot write '" << opt.file << "'\n";
                return 1;
            }
            dumpOps(out, ops);
            std::cout << "wrote " << ops.size() << " ops to " << opt.file << "\n";
            return 0;
        }
    }

    // --- Measure -------------------------------------------------------------
    if (opt.producers == 0) {
        std::cerr << "engine_latency: --producers must be >= 1\n";
        return 1;
    }

    const double cps = CycleClock::cyclesPerSecond();
    // --warmup is a *total* across all producers, split evenly per chunk and
    // clamped to the chunk size (a chunk can't warm up more ops than it has).
    const std::size_t warmup_total = std::min(opt.warmup, ops.size() / 2);
    const std::size_t per = ops.size() / opt.producers;
    const std::size_t warmup_per_producer = std::min(warmup_total / opt.producers, per);

    std::cout << "cycles/sec: " << std::fixed << std::setprecision(0) << cps << "\n"
              << "producers: " << opt.producers << ", matcher core: " << opt.matcher_core
              << ", ops: " << ops.size() << ", warmup: " << warmup_total << "\n\n";

    ConcurrentBook book(opt.producers, 1u << 16, opt.matcher_core);

    std::vector<LatencyHistogram> hists(opt.producers);
    std::vector<std::thread> threads;
    for (std::size_t p = 0; p < opt.producers; ++p) {
        const std::size_t begin = p * per;
        const std::size_t end = (p + 1 == opt.producers) ? ops.size() : (p + 1) * per;
        threads.emplace_back(runChunk, std::ref(book), std::cref(ops), begin, end,
                             warmup_per_producer, std::ref(hists[p]));
    }
    for (auto& t : threads) {
        t.join();
    }
    book.stop();

    LatencyHistogram all;
    for (auto& h : hists) {
        all.merge(h);
    }

    // --- Report --------------------------------------------------------------
    const auto fmt = [&](const char* name, std::int64_t cycles) {
        std::cout << std::left << std::setw(8) << name << std::right << std::fixed
                  << std::setprecision(2) << std::setw(12) << CycleClock::cyclesToNs(cycles)
                  << " ns  (" << cycles << " cycles)\n";
    };

    std::cout << "count:  " << all.count() << "\n";
    fmt("mean", static_cast<std::int64_t>(all.mean()));
    fmt("min", all.min());
    fmt("p50", all.valueAtPercentile(50));
    fmt("p90", all.valueAtPercentile(90));
    fmt("p99", all.valueAtPercentile(99));
    fmt("p999", all.valueAtPercentile(99.9));
    fmt("p9999", all.valueAtPercentile(99.99));
    fmt("max", all.max());

    if (opt.producers > 1) {
        std::cout << "\nper-producer (count, p50 ns, p99 ns):\n";
        for (std::size_t p = 0; p < opt.producers; ++p) {
            const auto& h = hists[p];
            std::cout << "  producer " << p << ": " << h.count() << "  "
                      << CycleClock::cyclesToNs(h.valueAtPercentile(50)) << "  "
                      << CycleClock::cyclesToNs(h.valueAtPercentile(99)) << "\n";
        }
    }

    return 0;
}
