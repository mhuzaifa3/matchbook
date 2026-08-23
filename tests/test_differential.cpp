#include <cstdio>
#include <random>
#include <vector>

#include "harness.hpp"
#include "matchbook/order_book.hpp"
#include "recorder.hpp"
#include "reference_book.hpp"

using namespace matchbook;
using matchbook::testing::Recorder;
using matchbook::testing::ReferenceBook;

namespace {

// Random operation sequences are applied to both implementations and their
// event streams compared after every step, so a divergence is reported at the
// operation that caused it rather than at the end.
struct Divergence {
    bool diverged = false;
    std::size_t step = 0;
    std::string detail;
};

Divergence run_sequence(std::uint64_t seed, int operations) {
    Recorder fast_log;
    Recorder slow_log;
    constexpr Price kBandMin = 90, kBandMax = 110;
    OrderBook fast{[&](const Event& e) { fast_log(e); }, {kBandMin, kBandMax}};
    ReferenceBook slow{[&](const Event& e) { slow_log(e); }, kBandMin, kBandMax};

    std::mt19937_64 rng(seed);
    std::vector<OrderId> live;
    OrderId next_id = 1;

    for (int step = 0; step < operations; ++step) {
        const int action = static_cast<int>(rng() % 100);

        if (action < 60) {
            NewOrder order{};
            order.id = next_id++;
            order.side = (rng() % 2) ? Side::Buy : Side::Sell;
            const int kind = static_cast<int>(rng() % 10);
            order.type = kind < 8 ? OrderType::Limit : OrderType::Market;
            const int tif = static_cast<int>(rng() % 10);
            order.tif = tif < 7 ? TimeInForce::GTC : (tif < 9 ? TimeInForce::IOC : TimeInForce::FOK);
            order.price = static_cast<Price>(88 + rng() % 26);
            order.quantity = static_cast<Quantity>(1 + rng() % 20);
            if (order.type == OrderType::Market) order.price = 0;

            // Prices run past both edges of the band, and one submission in
            // eight is malformed, so every rejection path runs in sequence.
            switch (static_cast<int>(rng() % 8)) {
                case 0: order.quantity = 0; break;
                case 1: if (!live.empty()) order.id = live[rng() % live.size()]; break;
                case 2: order.type = OrderType::Limit, order.price = 0; break;
                default: break;
            }

            fast.submit(order);
            slow.submit(order);
            if (order.type == OrderType::Limit && order.tif == TimeInForce::GTC)
                live.push_back(order.id);
        } else if (action < 85 && !live.empty()) {
            const std::size_t which = rng() % live.size();
            const OrderId id = live[which];
            live.erase(live.begin() + static_cast<long>(which));
            fast.cancel(id);
            slow.cancel(id);
        } else if (!live.empty()) {
            const std::size_t which = rng() % live.size();
            const OrderId old_id = live[which];
            OrderId new_id = next_id++;
            auto price = static_cast<Price>(88 + rng() % 26);
            auto qty = static_cast<Quantity>(1 + rng() % 20);

            // Valid fresh arguments never reach the rejection paths.
            switch (static_cast<int>(rng() % 8)) {
                case 0: qty = 0; break;
                case 1: price = 0; break;
                case 2: new_id = live[(which + 1) % live.size()]; break;
                case 3: new_id = old_id; break;
                default: break;
            }

            const bool accepted = fast.replace(old_id, new_id, price, qty);
            if (slow.replace(old_id, new_id, price, qty) != accepted)
                return {true, static_cast<std::size_t>(step), "replace return value diverged"};
            if (accepted) {
                live.erase(live.begin() + static_cast<long>(which));
                live.push_back(new_id);
            }
        }

        if (fast_log.log != slow_log.log) {
            std::string detail;
            const std::size_t common = std::min(fast_log.log.size(), slow_log.log.size());
            std::size_t at = 0;
            while (at < common && fast_log.log[at] == slow_log.log[at]) ++at;
            detail = "first difference at event " + std::to_string(at) + ": book=" +
                     (at < fast_log.log.size() ? fast_log.log[at] : "<none>") +
                     " reference=" + (at < slow_log.log.size() ? slow_log.log[at] : "<none>");
            return {true, static_cast<std::size_t>(step), detail};
        }

        if (fast.best_bid() != slow.best_bid() || fast.best_ask() != slow.best_ask() ||
            fast.resting_orders() != slow.resting_orders())
            return {true, static_cast<std::size_t>(step), "book state diverged"};

        for (Price price = kBandMin - 2; price <= kBandMax + 2; ++price) {
            if (fast.quantity_at(Side::Buy, price) != slow.quantity_at(Side::Buy, price) ||
                fast.quantity_at(Side::Sell, price) != slow.quantity_at(Side::Sell, price))
                return {true, static_cast<std::size_t>(step),
                        "depth diverged at price " + std::to_string(price)};
        }
    }
    return {};
}

}  // namespace

TEST(matches_reference_across_many_seeds) {
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        const Divergence result = run_sequence(seed, 300);
        ++harness::checks;
        if (result.diverged) {
            harness::fail(__FILE__, __LINE__,
                          "seed " + std::to_string(seed) + " step " +
                              std::to_string(result.step) + ": " + result.detail);
            return;
        }
    }
}

TEST(matches_reference_on_long_sequences) {
    for (std::uint64_t seed = 1000; seed <= 1010; ++seed) {
        const Divergence result = run_sequence(seed, 5000);
        ++harness::checks;
        if (result.diverged) {
            harness::fail(__FILE__, __LINE__,
                          "seed " + std::to_string(seed) + " step " +
                              std::to_string(result.step) + ": " + result.detail);
            return;
        }
    }
}

TEST_MAIN
