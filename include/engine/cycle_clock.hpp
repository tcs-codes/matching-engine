#pragma once

#include <chrono>
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace engine {

// ---------------------------------------------------------------------------
// CycleClock -- cycle-accurate time for latency measurement (Phase 5).
//
// On x86 this reads the TSC via rdtsc, which on any CPU from the last ~15
// years is *invariant*: it ticks at a constant rate regardless of frequency
// scaling, so it's a monotonic, sub-nanosecond-resolution clock with almost
// no overhead. The TSC rate is calibrated once at startup against a
// steady_clock window. On non-x86 we fall back to steady_clock nanoseconds
// and treat cycles == ns, so reporting code can be uniform everywhere.
//
// Caveat for reproducibility (the honest kind): calibration happens on this
// core at this moment; report the calibrated rate alongside results.
// ---------------------------------------------------------------------------
class CycleClock {
public:
    // Current time in "cycles" (TSC ticks on x86, ns elsewhere).
    static std::uint64_t now() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
        return __rdtsc();
#else
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // Calibrated once, lazily: cycles per second.
    static double cyclesPerSecond() {
        static const double cps = calibrateImpl();
        return cps;
    }

    static double cyclesToNs(std::uint64_t cycles) {
        return static_cast<double>(cycles) * 1e9 / cyclesPerSecond();
    }

private:
    static double calibrateImpl() {
        // Measure the TSC rate over a ~100ms steady_clock window (the TSC is
        // invariant, so a fixed window gives a stable rate).
        const auto start = std::chrono::steady_clock::now();
        const std::uint64_t c0 = now();
        std::chrono::nanoseconds elapsed{};
        std::uint64_t c1 = c0;
        while (elapsed < std::chrono::milliseconds(100)) {
            c1 = now();
            elapsed = std::chrono::steady_clock::now() - start;
        }
        const double ns = static_cast<double>(elapsed.count());
        return static_cast<double>(c1 - c0) * 1e9 / ns;
    }
};

} // namespace engine
