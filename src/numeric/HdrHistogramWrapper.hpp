#pragma once

#include <hdr/hdr_histogram.h>
#include <cstdint>
#include <cstdlib>

namespace trade_bot {

/**
 * RAII wrapper for HdrHistogram_c.
 */
class HdrHistogram {
public:
    HdrHistogram(int64_t highest_val, int sig_digits) {
        hdr_alloc(highest_val, sig_digits, &histogram_);
    }

    ~HdrHistogram() {
        if (histogram_) {
            free(histogram_);
        }
    }

    // Disable copy
    HdrHistogram(const HdrHistogram&) = delete;
    HdrHistogram& operator=(const HdrHistogram&) = delete;

    // Enable move
    HdrHistogram(HdrHistogram&& other) noexcept : histogram_(other.histogram_) {
        other.histogram_ = nullptr;
    }
    HdrHistogram& operator=(HdrHistogram&& other) noexcept {
        if (this != &other) {
            if (histogram_) free(histogram_);
            histogram_ = other.histogram_;
            other.histogram_ = nullptr;
        }
        return *this;
    }

    void record(int64_t value) {
        hdr_record_value(histogram_, value);
    }

    int64_t value_at_percentile(double percentile) const {
        return hdr_value_at_percentile(histogram_, percentile);
    }

    int64_t min() const { return hdr_min(histogram_); }
    int64_t max() const { return hdr_max(histogram_); }
    double mean() const { return hdr_mean(histogram_); }

private:
    struct hdr_histogram* histogram_ = nullptr;
};

} // namespace trade_bot
