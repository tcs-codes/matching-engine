#pragma once

#include <cstdint>

extern "C" {
#include <hdr/hdr_histogram.h>
}

namespace engine {

// ---------------------------------------------------------------------------
// LatencyHistogram -- RAII wrapper around Gil Tene's HdrHistogram (the C
// library, fetched via CMake FetchContent). This is the standard latency
// measurement tool in low-latency trading: a high-dynamic-range histogram
// with ~3 significant figures of precision across the entire 64-bit range,
// so one histogram can capture nanosecond and millisecond tail values with
// the same relative accuracy.
//
// The C library is NOT thread-safe for recording, so the Phase 5 harness
// gives each producer thread its own histogram and merges them at the end
// (hdr_add is an exact bucket-wise merge). Values are recorded in raw TSC
// cycles (see CycleClock) and converted to nanoseconds only for reporting.
// ---------------------------------------------------------------------------
class LatencyHistogram {
public:
    explicit LatencyHistogram(int sigfigs = 3) {
        // lowest trackable 1, highest INT64_MAX, 3 significant figures.
        const int rc = hdr_init(1, INT64_MAX, sigfigs, &hist_);
        (void)rc; // hdr_init returns 0 on success; allocation failure would
                  // have thrown in the allocator anyway
    }
    ~LatencyHistogram() {
        if (hist_ != nullptr) hdr_close(hist_);
    }

    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;

    LatencyHistogram(LatencyHistogram&& other) noexcept : hist_(other.hist_) {
        other.hist_ = nullptr;
    }
    LatencyHistogram& operator=(LatencyHistogram&& other) noexcept {
        if (this != &other) {
            if (hist_ != nullptr) hdr_close(hist_);
            hist_ = other.hist_;
            other.hist_ = nullptr;
        }
        return *this;
    }

    void record(std::int64_t value) { hdr_record_value(hist_, value); }
    void recordValueWithCount(std::int64_t value, std::int64_t count) {
        hdr_record_values(hist_, value, count);
    }

    // Exact bucket-wise merge of `other` into this histogram.
    void merge(const LatencyHistogram& other) { hdr_add(hist_, other.hist_); }

    void reset() { hdr_reset(hist_); }

    std::int64_t count() const { return hist_->total_count; }
    std::int64_t min() const { return hdr_min(hist_); }
    std::int64_t max() const { return hdr_max(hist_); }
    double mean() const { return hdr_mean(hist_); }
    // Percentile in [0, 100]; returns the recorded value at that percentile
    // (the highest value of the bucket containing it).
    std::int64_t valueAtPercentile(double percentile) const {
        return hdr_value_at_percentile(hist_, percentile);
    }

private:
    hdr_histogram* hist_{nullptr};
};

} // namespace engine
