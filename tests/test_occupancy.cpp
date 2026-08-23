#include <random>
#include <set>

#include "harness.hpp"
#include "matchbook/occupancy.hpp"

using matchbook::Occupancy;

namespace {

constexpr std::size_t kTicks = 4099;

std::size_t naive_next_above(const std::set<std::size_t>& live, std::size_t i) {
    auto it = live.upper_bound(i);
    return it == live.end() ? Occupancy::npos : *it;
}

std::size_t naive_next_below(const std::set<std::size_t>& live, std::size_t i) {
    auto it = live.lower_bound(i);
    return it == live.begin() ? Occupancy::npos : *std::prev(it);
}

}  // namespace

TEST(empty_bitmap_reports_no_occupied_tick) {
    const Occupancy occupancy(kTicks);
    CHECK(occupancy.empty());
    CHECK_EQ(occupancy.lowest(), Occupancy::npos);
    CHECK_EQ(occupancy.highest(), Occupancy::npos);
}

TEST(a_single_tick_is_both_lowest_and_highest) {
    Occupancy occupancy(kTicks);
    occupancy.set(4098);
    CHECK(!occupancy.empty());
    CHECK(occupancy.test(4098));
    CHECK_EQ(occupancy.lowest(), 4098u);
    CHECK_EQ(occupancy.highest(), 4098u);
    occupancy.clear(4098);
    CHECK(occupancy.empty());
}

TEST(ticks_spanning_word_boundaries_are_ordered) {
    Occupancy occupancy(kTicks);
    for (const std::size_t tick : {0u, 63u, 64u, 4095u, 4096u}) occupancy.set(tick);
    CHECK_EQ(occupancy.lowest(), 0u);
    CHECK_EQ(occupancy.highest(), 4096u);
    CHECK_EQ(occupancy.next_above(0), 63u);
    CHECK_EQ(occupancy.next_above(63), 64u);
    CHECK_EQ(occupancy.next_above(64), 4095u);
    CHECK_EQ(occupancy.next_below(4096), 4095u);
    CHECK_EQ(occupancy.next_below(0), Occupancy::npos);
    CHECK_EQ(occupancy.next_above(4096), Occupancy::npos);
}

TEST(matches_an_ordered_set_across_random_operations) {
    Occupancy occupancy(kTicks);
    std::set<std::size_t> live;
    std::mt19937_64 rng(7);

    for (int step = 0; step < 20000; ++step) {
        const std::size_t tick = rng() % kTicks;
        if (live.count(tick) != 0) {
            occupancy.clear(tick);
            live.erase(tick);
        } else {
            occupancy.set(tick);
            live.insert(tick);
        }

        ++harness::checks;
        if (occupancy.empty() != live.empty()) {
            harness::fail(__FILE__, __LINE__, "emptiness diverged at step " + std::to_string(step));
            return;
        }
        const std::size_t low = live.empty() ? Occupancy::npos : *live.begin();
        const std::size_t high = live.empty() ? Occupancy::npos : *std::prev(live.end());
        if (occupancy.lowest() != low || occupancy.highest() != high) {
            harness::fail(__FILE__, __LINE__, "extremes diverged at step " + std::to_string(step));
            return;
        }

        const std::size_t probe = rng() % kTicks;
        if (occupancy.next_above(probe) != naive_next_above(live, probe) ||
            occupancy.next_below(probe) != naive_next_below(live, probe)) {
            harness::fail(__FILE__, __LINE__,
                          "traversal diverged at step " + std::to_string(step) + " probe " +
                              std::to_string(probe));
            return;
        }
    }
}

TEST_MAIN
