#include <gtest/gtest.h>

#include <cstdint>

#include "engine/latency_histogram.hpp"

using namespace engine;

// ---------------------------------------------------------------------------
// LatencyHistogram tests (HdrHistogram C library wrapper).
//
// The interesting properties to pin down: percentiles land in the right
// bucket for a known distribution, min/max/mean/count are exact, and merge
// is an exact bucket-wise add (which is what makes the per-producer
// histograms in engine_latency trustworthy).
// ---------------------------------------------------------------------------

TEST(LatencyHistogram, EmptyHistogram) {
    // The C library's convention for an empty histogram: no recorded counts,
    // min sentinel INT64_MAX, max 0.
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0);
    EXPECT_EQ(h.min(), INT64_MAX);
    EXPECT_EQ(h.max(), 0);
}

TEST(LatencyHistogram, BasicStatsOnKnownDistribution) {
    LatencyHistogram h;
    // Uniform 1000..1999: mean 1499.5, p50 ~1500, p99 ~1990.
    for (int i = 0; i < 1000; ++i) {
        h.record(1000 + i);
    }
    EXPECT_EQ(h.count(), 1000);
    EXPECT_EQ(h.min(), 1000);
    EXPECT_EQ(h.max(), 1999);
    EXPECT_NEAR(h.mean(), 1499.5, 0.5);

    // HDR reports the highest value of the bucket containing the percentile;
    // with 3 significant figures and values ~1500 the bucket width is ~2, so
    // percentiles must be within a couple of ticks of the exact value.
    EXPECT_GE(h.valueAtPercentile(50), 1499);
    EXPECT_LE(h.valueAtPercentile(50), 1502);
    EXPECT_GE(h.valueAtPercentile(99), 1989);
    EXPECT_LE(h.valueAtPercentile(99), 2000);
    EXPECT_GE(h.valueAtPercentile(100), 1999);
}

TEST(LatencyHistogram, OneHistogramSpansNanosecondsToSeconds) {
    // The whole point of HDR: a single histogram captures 10 ns and 1 s with
    // the same ~3 significant figures of relative precision.
    LatencyHistogram h;
    h.record(10);
    h.record(1'000'000);        // 1 ms
    h.record(1'000'000'000);    // 1 s
    EXPECT_EQ(h.count(), 3);
    EXPECT_EQ(h.min(), 10);
    // max/percentiles are reported as the bucket's *highest equivalent value*,
    // so they land at/above the recorded max, within the bucket's ~0.04%
    // relative width at 1e9.
    EXPECT_GE(h.max(), 1'000'000'000);
    EXPECT_LT(h.max(), 1'000'500'000);
    const std::int64_t p100 = h.valueAtPercentile(100);
    EXPECT_GE(p100, 1'000'000'000);
    EXPECT_LT(p100, 1'000'500'000);
    const std::int64_t p34 = h.valueAtPercentile(100.0 / 3.0);
    EXPECT_GE(p34, 10); // first third lands in the low (ns) bucket
    EXPECT_LT(p34, 1'000'000);
}

TEST(LatencyHistogram, MergeIsExact) {
    LatencyHistogram a, b, expected;
    for (int i = 0; i < 500; ++i) {
        a.record(100 + i);
        expected.record(100 + i);
    }
    for (int i = 0; i < 500; ++i) {
        b.record(100 + i);
        expected.record(100 + i);
    }
    a.merge(b);

    EXPECT_EQ(a.count(), expected.count());
    EXPECT_EQ(a.min(), expected.min());
    EXPECT_EQ(a.max(), expected.max());
    EXPECT_DOUBLE_EQ(a.mean(), expected.mean());
    for (double p : {10.0, 50.0, 90.0, 99.0, 99.9}) {
        EXPECT_EQ(a.valueAtPercentile(p), expected.valueAtPercentile(p)) << "p" << p;
    }
}

TEST(LatencyHistogram, ResetClears) {
    LatencyHistogram h;
    h.record(1000);
    h.record(2000);
    EXPECT_EQ(h.count(), 2);
    h.reset();
    EXPECT_EQ(h.count(), 0);
    EXPECT_EQ(h.min(), INT64_MAX);
    EXPECT_EQ(h.max(), 0);
}
