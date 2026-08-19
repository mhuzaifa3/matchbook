#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "matchbook/order_book.hpp"

using namespace matchbook;
using Clock = std::chrono::steady_clock;

namespace {

struct Percentiles {
    double p50, p99, p999, max;
};

Percentiles percentiles(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) {
        const auto index = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
        return samples[index];
    };
    return {at(0.50), at(0.99), at(0.999), samples.back()};
}

void report(const char* name, std::vector<double>& latencies, double seconds, std::size_t ops) {
    const Percentiles p = percentiles(latencies);
    std::printf("%-22s %10.0f ops/sec   p50 %6.0fns  p99 %7.0fns  p99.9 %8.0fns  max %9.0fns\n",
                name, static_cast<double>(ops) / seconds, p.p50, p.p99, p.p999, p.max);
}

}  // namespace

int main() {
    constexpr std::size_t kOps = 1'000'000;
    std::printf("matchbook benchmark, %zu operations each\n\n", kOps);

    {
        OrderBook book{[](const Event&) {}, kOps};
        std::mt19937_64 rng(1);
        std::vector<double> latencies;
        latencies.reserve(kOps);
        const auto start = Clock::now();
        for (std::size_t i = 0; i < kOps; ++i) {
            NewOrder order{i + 1, (rng() % 2) ? Side::Buy : Side::Sell, OrderType::Limit,
                           TimeInForce::GTC, static_cast<Price>(1000 + rng() % 200),
                           static_cast<Quantity>(1 + rng() % 100)};
            const auto t0 = Clock::now();
            book.submit(order);
            latencies.push_back(
                std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        report("submit (mixed)", latencies, seconds, kOps);
    }

    {
        OrderBook book{[](const Event&) {}, kOps};
        std::mt19937_64 rng(2);
        std::vector<OrderId> resting;
        resting.reserve(kOps);
        for (std::size_t i = 0; i < kOps; ++i) {
            const OrderId id = i + 1;
            book.submit({id, Side::Buy, OrderType::Limit, TimeInForce::GTC,
                         static_cast<Price>(1000 + rng() % 200),
                         static_cast<Quantity>(1 + rng() % 100)});
            resting.push_back(id);
        }
        std::shuffle(resting.begin(), resting.end(), rng);
        std::vector<double> latencies;
        latencies.reserve(resting.size());
        const auto start = Clock::now();
        for (const OrderId id : resting) {
            const auto t0 = Clock::now();
            book.cancel(id);
            latencies.push_back(
                std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        report("cancel (random)", latencies, seconds, resting.size());
    }

    {
        OrderBook book{[](const Event&) {}, kOps * 2};
        std::mt19937_64 rng(3);
        for (std::size_t i = 0; i < kOps; ++i)
            book.submit({i + 1, Side::Sell, OrderType::Limit, TimeInForce::GTC,
                         static_cast<Price>(1000 + rng() % 50), 10});
        std::vector<double> latencies;
        latencies.reserve(kOps);
        const auto start = Clock::now();
        for (std::size_t i = 0; i < kOps; ++i) {
            const auto t0 = Clock::now();
            book.submit({kOps + i + 1, Side::Buy, OrderType::Limit, TimeInForce::IOC, 1100, 10});
            latencies.push_back(
                std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        report("submit (crossing)", latencies, seconds, kOps);
    }

    return 0;
}
