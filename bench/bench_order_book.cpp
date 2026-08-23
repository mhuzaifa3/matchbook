#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "matchbook/order_book.hpp"

using namespace matchbook;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t kOps = 1'000'000;
constexpr std::size_t kBatch = 32;
constexpr OrderBook::PriceBand kBand{1, 4096};

double nanos(Clock::duration d) { return std::chrono::duration<double, std::nano>(d).count(); }

// Smallest nonzero gap two back-to-back clock reads can report. On Apple
// silicon that is one tick of the 24MHz timebase, 41.667ns, which is the same
// order as a single submit. Timing one operation at a time would report the
// tick rather than the book, so every measurement below is built around this.
double tick() {
    double smallest = 1e9;
    for (int i = 0; i < 100'000; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        const double d = nanos(b - a);
        if (d > 0 && d < smallest) smallest = d;
    }
    return smallest;
}

struct Stats {
    double p50, p99, p999;
};

Stats percentiles(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) {
        return samples[static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1))];
    };
    return {at(0.50), at(0.99), at(0.999)};
}

std::string scaled(double ns) {
    char buffer[32];
    if (ns < 1e3) {
        std::snprintf(buffer, sizeof buffer, "%.0fns", ns);
    } else if (ns < 1e6) {
        std::snprintf(buffer, sizeof buffer, "%.2fus", ns / 1e3);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.2fms", ns / 1e6);
    }
    return buffer;
}

// Each workload builds a fresh book, pre-generates its operation stream so no
// random number generation lands inside a timed region, and hands the caller a
// closure that applies operation i. It runs once per measurement pass.
template <typename Workload>
void report(const char* name, const Workload& workload, double resolution) {
    double seconds = 0;
    workload(kOps, [&](auto&& apply) {
        const auto start = Clock::now();
        for (std::size_t i = 0; i < kOps; ++i) apply(i);
        seconds = nanos(Clock::now() - start) / 1e9;
    });
    const double mean = seconds * 1e9 / static_cast<double>(kOps);

    std::vector<double> samples;
    workload(kOps, [&](auto&& apply) {
        samples.reserve(kOps / kBatch);
        for (std::size_t i = 0; i + kBatch <= kOps; i += kBatch) {
            const auto start = Clock::now();
            for (std::size_t j = 0; j < kBatch; ++j) apply(i + j);
            samples.push_back(nanos(Clock::now() - start) / kBatch);
        }
    });

    // Per-operation timing is only trustworthy far above the tick, so this pass
    // reports outliers and nothing else.
    const double threshold = std::max(20 * mean, 4 * resolution);
    double worst = 0;
    std::size_t stalls = 0;
    workload(kOps, [&](auto&& apply) {
        for (std::size_t i = 0; i < kOps; ++i) {
            const auto start = Clock::now();
            apply(i);
            const double elapsed = nanos(Clock::now() - start);
            if (elapsed > threshold) ++stalls;
            worst = std::max(worst, elapsed);
        }
    });

    const Stats s = percentiles(samples);
    std::printf("  %-18s %8.1fM/s %9s %9s %9s %9s %9s %8zu\n", name,
                static_cast<double>(kOps) / seconds / 1e6, scaled(mean).c_str(),
                scaled(s.p50).c_str(), scaled(s.p99).c_str(), scaled(s.p999).c_str(),
                scaled(worst).c_str(), stalls);
}

}  // namespace

int main() {
    const double resolution = tick();

    const auto submit_resting = [](std::size_t ops, auto&& body) {
        OrderBook book{[](const Event&) {}, kBand, ops};
        std::mt19937_64 rng(1);
        std::vector<NewOrder> orders;
        orders.reserve(ops);
        for (std::size_t i = 0; i < ops; ++i) {
            const bool buy = rng() % 2 == 0;
            const auto price = static_cast<Price>(buy ? 1000 + rng() % 100 : 1100 + rng() % 100);
            orders.push_back({i + 1, buy ? Side::Buy : Side::Sell, OrderType::Limit,
                              TimeInForce::GTC, price, static_cast<Quantity>(1 + rng() % 100)});
        }
        body([&](std::size_t i) { book.submit(orders[i]); });
    };

    const auto submit_crossing = [](std::size_t ops, auto&& body) {
        OrderBook book{[](const Event&) {}, kBand, ops * 2};
        std::mt19937_64 rng(3);
        for (std::size_t i = 0; i < ops; ++i)
            book.submit({i + 1, Side::Sell, OrderType::Limit, TimeInForce::GTC,
                         static_cast<Price>(1000 + rng() % 50), 10});
        body([&](std::size_t i) {
            book.submit({ops + i + 1, Side::Buy, OrderType::Limit, TimeInForce::IOC, 1100, 10});
        });
    };

    const auto cancel_random = [](std::size_t ops, auto&& body) {
        OrderBook book{[](const Event&) {}, kBand, ops};
        std::mt19937_64 rng(2);
        std::vector<OrderId> ids;
        ids.reserve(ops);
        for (std::size_t i = 0; i < ops; ++i) {
            book.submit({i + 1, Side::Buy, OrderType::Limit, TimeInForce::GTC,
                         static_cast<Price>(1000 + rng() % 200),
                         static_cast<Quantity>(1 + rng() % 100)});
            ids.push_back(i + 1);
        }
        std::shuffle(ids.begin(), ids.end(), rng);
        body([&](std::size_t i) { book.cancel(ids[i]); });
    };

    std::printf("matchbook benchmark, %zu operations per case\n\n", kOps);
    std::printf("  clock          steady_clock, %.1fns tick\n", resolution);
    std::printf("  throughput     one clock read per run, not per operation\n");
    std::printf("  distribution   batches of %zu, putting the tick at %.2fns per operation\n",
                kBatch, resolution / kBatch);
    std::printf("  stalls         one clock read per operation, counting past 20x the mean\n\n");
    std::printf("  %-18s %12s %9s %9s %9s %9s %9s %8s\n", "operation", "throughput", "mean", "p50",
                "p99", "p99.9", "worst", "stalls");

    report("submit resting", submit_resting, resolution);
    report("submit crossing", submit_crossing, resolution);
    report("cancel random", cancel_random, resolution);
    return 0;
}
