//=======================================================================
// Lightweight, dependency-free metrics registry: 
// atomic counters plus a simple bucketed latency histogram for order-processing time
// "how many orders/sec, how many trades/sec, what does p50/p99 order latency look like" without pulling in a full metrics library
//=======================================================================
#ifndef METRICS_HPP
#define METRICS_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

namespace sim {

class LatencyHistogram {
public:
    // upper bound (exclusive) of each bucket, in microseconds
    static constexpr std::array<std::int64_t, 7> kBucketBoundsUs = {10, 50, 100, 500, 1000, 5000, 50000};

    void record(std::chrono::microseconds duration) {
        std::int64_t us = duration.count();
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_us_.fetch_add(us, std::memory_order_relaxed);

        std::int64_t prev_max = max_us_.load(std::memory_order_relaxed);
        while (us > prev_max && !max_us_.compare_exchange_weak(prev_max, us, std::memory_order_relaxed)) {
        }

        for (std::size_t i = 0; i < kBucketBoundsUs.size(); ++i) {
            if (us < kBucketBoundsUs[i]) {
                buckets_[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        overflow_bucket_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    double mean_us() const {
        auto c = count();
        return c == 0 ? 0.0 : static_cast<double>(sum_us_.load(std::memory_order_relaxed)) / static_cast<double>(c);
    }
    std::int64_t max_us() const { return max_us_.load(std::memory_order_relaxed); }

    void reset() {
        count_ = 0;
        sum_us_ = 0;
        max_us_ = 0;
        for (auto& bucket : buckets_) bucket = 0;
        overflow_bucket_ = 0;
    }

    std::string report() const {
        std::ostringstream oss;
        oss << "count=" << count() << " mean_us=" << mean_us() << " max_us=" << max_us() << " buckets(<us:count)=[";
        for (std::size_t i = 0; i < kBucketBoundsUs.size(); ++i) {
            oss << kBucketBoundsUs[i] << ":" << buckets_[i].load(std::memory_order_relaxed);
            if (i + 1 < kBucketBoundsUs.size()) {
                oss << ", ";
            }
        }
        oss << ", overflow:" << overflow_bucket_.load(std::memory_order_relaxed) << "]";
        return oss.str();
    }

private:
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::int64_t> sum_us_{0};
    std::atomic<std::int64_t> max_us_{0};
    std::array<std::atomic<std::uint64_t>, 7> buckets_{};
    std::atomic<std::uint64_t> overflow_bucket_{0};
};

class MetricsRegistry {
public:
    static MetricsRegistry& instance() {
        static MetricsRegistry registry;
        return registry;
    }

    void order_submitted() { orders_submitted_.fetch_add(1, std::memory_order_relaxed); }
    void order_accepted() { orders_accepted_.fetch_add(1, std::memory_order_relaxed); }
    void order_rejected() { orders_rejected_.fetch_add(1, std::memory_order_relaxed); }
    void order_queued() { orders_queued_.fetch_add(1, std::memory_order_relaxed); }
    void order_expired() { orders_expired_.fetch_add(1, std::memory_order_relaxed); }
    void trades_executed(std::uint64_t n) { trades_executed_.fetch_add(n, std::memory_order_relaxed); }
    void volume_traded(std::uint64_t qty) { volume_traded_.fetch_add(qty, std::memory_order_relaxed); }
    void record_submit_latency(std::chrono::microseconds d) { submit_latency_.record(d); }

    std::uint64_t orders_submitted() const { return orders_submitted_.load(std::memory_order_relaxed); }
    std::uint64_t orders_accepted() const { return orders_accepted_.load(std::memory_order_relaxed); }
    std::uint64_t orders_rejected() const { return orders_rejected_.load(std::memory_order_relaxed); }
    std::uint64_t orders_queued() const { return orders_queued_.load(std::memory_order_relaxed); }
    std::uint64_t orders_expired() const { return orders_expired_.load(std::memory_order_relaxed); }
    std::uint64_t trades_executed() const { return trades_executed_.load(std::memory_order_relaxed); }
    std::uint64_t volume_traded() const { return volume_traded_.load(std::memory_order_relaxed); }
    const LatencyHistogram& submit_latency() const { return submit_latency_; }

    std::string report(double elapsed_seconds) const {
        std::ostringstream oss;
        oss << "orders_submitted=" << orders_submitted() << " orders_accepted=" << orders_accepted() << " orders_rejected=" << orders_rejected() << " orders_queued=" << orders_queued() << " orders_expired=" << orders_expired() << " trades_executed=" << trades_executed() << " volume_traded=" << volume_traded();
        if (elapsed_seconds > 0.0) {
            oss << " orders_per_sec=" << (static_cast<double>(orders_submitted()) / elapsed_seconds) << " trades_per_sec=" << (static_cast<double>(trades_executed()) / elapsed_seconds);
        }
        oss << " | submit_latency: " << submit_latency_.report();
        return oss.str();
    }

    // resets all counters: primarily useful so unit tests don't see state leak across cases, since this is a process-wide singleton
    void reset() {
        orders_submitted_ = 0;
        orders_accepted_ = 0;
        orders_rejected_ = 0;
        orders_queued_ = 0;
        orders_expired_ = 0;
        trades_executed_ = 0;
        volume_traded_ = 0;
        submit_latency_.reset();
    }

private:
    MetricsRegistry() = default;
    std::atomic<std::uint64_t> orders_submitted_{0};
    std::atomic<std::uint64_t> orders_accepted_{0};
    std::atomic<std::uint64_t> orders_rejected_{0};
    std::atomic<std::uint64_t> orders_queued_{0};
    std::atomic<std::uint64_t> orders_expired_{0};
    std::atomic<std::uint64_t> trades_executed_{0};
    std::atomic<std::uint64_t> volume_traded_{0};
    LatencyHistogram submit_latency_;
};

} // namespace sim

#endif // METRICS_HPP