#include <stdexcept>

#include "harness.hpp"
#include "matchbook/order_book.hpp"
#include "recorder.hpp"

using namespace matchbook;
using matchbook::testing::Recorder;

namespace {

NewOrder limit(OrderId id, Side side, Price price, Quantity qty,
               TimeInForce tif = TimeInForce::GTC) {
    return NewOrder{id, side, OrderType::Limit, tif, price, qty};
}

NewOrder market(OrderId id, Side side, Quantity qty, TimeInForce tif = TimeInForce::IOC) {
    return NewOrder{id, side, OrderType::Market, tif, 0, qty};
}

constexpr OrderBook::PriceBand kBand{1, 4096};

struct Fixture {
    Recorder recorder;
    OrderBook book{[this](const Event& e) { recorder(e); }, kBand};
};

}  // namespace

TEST(empty_book_has_no_best_prices) {
    Fixture f;
    CHECK(!f.book.best_bid().has_value());
    CHECK(!f.book.best_ask().has_value());
    CHECK_EQ(f.book.resting_orders(), 0u);
}

TEST(limit_order_rests_and_sets_best_price) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 10));
    CHECK_EQ(f.book.best_bid().value(), 100);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 10u);
    CHECK_EQ(f.book.resting_orders(), 1u);
    CHECK_EQ(f.recorder.log.size(), 1u);
    CHECK(f.recorder.back() == "ACC 1 100 10");
}

TEST(full_match_removes_resting_order) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 10));
    f.recorder.clear();
    f.book.submit(limit(2, Side::Sell, 100, 10));
    CHECK_EQ(f.recorder.log.size(), 2u);
    CHECK(f.recorder.log[0] == "ACC 2 100 10");
    CHECK(f.recorder.log[1] == "TRD 2 1 100 10");
    CHECK_EQ(f.book.resting_orders(), 0u);
    CHECK(!f.book.best_bid().has_value());
}

TEST(partial_fill_leaves_remainder_resting) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 10));
    f.book.submit(limit(2, Side::Sell, 100, 4));
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 6u);
    CHECK_EQ(f.book.resting_orders(), 1u);
}

TEST(taker_remainder_rests_on_its_own_side) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 4));
    f.book.submit(limit(2, Side::Sell, 100, 10));
    CHECK_EQ(f.book.quantity_at(Side::Sell, 100), 6u);
    CHECK(!f.book.best_bid().has_value());
    CHECK_EQ(f.book.best_ask().value(), 100);
}

TEST(time_priority_is_fifo_within_a_price) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.book.submit(limit(2, Side::Buy, 100, 5));
    f.recorder.clear();
    f.book.submit(limit(3, Side::Sell, 100, 7));
    CHECK(f.recorder.log[1] == "TRD 3 1 100 5");
    CHECK(f.recorder.log[2] == "TRD 3 2 100 2");
}

TEST(price_priority_beats_time) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.book.submit(limit(2, Side::Buy, 101, 5));
    f.recorder.clear();
    f.book.submit(limit(3, Side::Sell, 100, 6));
    CHECK(f.recorder.log[1] == "TRD 3 2 101 5");
    CHECK(f.recorder.log[2] == "TRD 3 1 100 1");
}

TEST(trades_execute_at_the_resting_price) {
    Fixture f;
    f.book.submit(limit(1, Side::Sell, 100, 5));
    f.recorder.clear();
    f.book.submit(limit(2, Side::Buy, 105, 5));
    CHECK(f.recorder.log[1] == "TRD 2 1 100 5");
}

TEST(non_crossing_orders_do_not_trade) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 99, 5));
    f.book.submit(limit(2, Side::Sell, 101, 5));
    CHECK_EQ(f.book.resting_orders(), 2u);
    CHECK_EQ(f.book.best_bid().value(), 99);
    CHECK_EQ(f.book.best_ask().value(), 101);
}

TEST(market_order_sweeps_levels) {
    Fixture f;
    f.book.submit(limit(1, Side::Sell, 100, 3));
    f.book.submit(limit(2, Side::Sell, 101, 3));
    f.recorder.clear();
    f.book.submit(market(3, Side::Buy, 5));
    CHECK(f.recorder.log[1] == "TRD 3 1 100 3");
    CHECK(f.recorder.log[2] == "TRD 3 2 101 2");
    CHECK_EQ(f.book.quantity_at(Side::Sell, 101), 1u);
}

TEST(market_order_without_liquidity_is_rejected) {
    Fixture f;
    f.book.submit(market(1, Side::Buy, 5));
    CHECK_EQ(f.recorder.log.size(), 1u);
    CHECK(f.recorder.back() == "REJ 1 5");
}

TEST(market_remainder_is_canceled_not_rested) {
    Fixture f;
    f.book.submit(limit(1, Side::Sell, 100, 2));
    f.recorder.clear();
    f.book.submit(market(2, Side::Buy, 5));
    CHECK(f.recorder.back() == "CXL 2 3");
    CHECK_EQ(f.book.resting_orders(), 0u);
}

TEST(ioc_cancels_its_unfilled_remainder) {
    Fixture f;
    f.book.submit(limit(1, Side::Sell, 100, 2));
    f.recorder.clear();
    f.book.submit(limit(2, Side::Buy, 100, 5, TimeInForce::IOC));
    CHECK(f.recorder.log[1] == "TRD 2 1 100 2");
    CHECK(f.recorder.back() == "CXL 2 3");
    CHECK_EQ(f.book.resting_orders(), 0u);
}

TEST(fok_is_rejected_when_not_fully_fillable) {
    Fixture f;
    f.book.submit(limit(1, Side::Sell, 100, 2));
    f.recorder.clear();
    f.book.submit(limit(2, Side::Buy, 100, 5, TimeInForce::FOK));
    CHECK_EQ(f.recorder.log.size(), 1u);
    CHECK(f.recorder.back() == "REJ 2 4");
    CHECK_EQ(f.book.quantity_at(Side::Sell, 100), 2u);
}

TEST(fok_executes_when_fully_fillable) {
    Fixture f;
    f.book.submit(limit(1, Side::Sell, 100, 5));
    f.recorder.clear();
    f.book.submit(limit(2, Side::Buy, 100, 5, TimeInForce::FOK));
    CHECK(f.recorder.log[1] == "TRD 2 1 100 5");
    CHECK_EQ(f.book.resting_orders(), 0u);
}

TEST(fok_only_counts_liquidity_within_its_limit) {
    Fixture f;
    f.book.submit(limit(1, Side::Sell, 100, 2));
    f.book.submit(limit(2, Side::Sell, 105, 5));
    f.recorder.clear();
    f.book.submit(limit(3, Side::Buy, 100, 5, TimeInForce::FOK));
    CHECK(f.recorder.back() == "REJ 3 4");
}

TEST(cancel_removes_the_order) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.recorder.clear();
    CHECK(f.book.cancel(1));
    CHECK(f.recorder.back() == "CXL 1 5");
    CHECK_EQ(f.book.resting_orders(), 0u);
    CHECK(!f.book.best_bid().has_value());
}

TEST(cancel_of_unknown_id_is_rejected) {
    Fixture f;
    CHECK(!f.book.cancel(42));
    CHECK(f.recorder.back() == "REJ 42 1");
}

TEST(cancel_preserves_other_orders_at_the_level) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.book.submit(limit(2, Side::Buy, 100, 7));
    f.book.cancel(1);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 7u);
    f.recorder.clear();
    f.book.submit(limit(3, Side::Sell, 100, 7));
    CHECK(f.recorder.log[1] == "TRD 3 2 100 7");
}

TEST(duplicate_order_id_is_rejected) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.recorder.clear();
    f.book.submit(limit(1, Side::Buy, 101, 5));
    CHECK(f.recorder.back() == "REJ 1 0");
    CHECK_EQ(f.book.resting_orders(), 1u);
}

TEST(zero_quantity_is_rejected) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 0));
    CHECK(f.recorder.back() == "REJ 1 2");
}

TEST(non_positive_limit_price_is_rejected) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 0, 5));
    CHECK(f.recorder.back() == "REJ 1 3");
}

TEST(replace_loses_time_priority) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.book.submit(limit(2, Side::Buy, 100, 5));
    f.book.replace(1, 3, 100, 5);
    f.recorder.clear();
    f.book.submit(limit(4, Side::Sell, 100, 10));
    CHECK(f.recorder.log[1] == "TRD 4 2 100 5");
    CHECK(f.recorder.log[2] == "TRD 4 3 100 5");
}

TEST(replace_of_unknown_id_is_rejected) {
    Fixture f;
    CHECK(!f.book.replace(9, 10, 100, 5));
    CHECK(f.recorder.back() == "REJ 9 1");
}

TEST(replace_with_zero_quantity_leaves_the_original_resting) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.recorder.clear();
    CHECK(!f.book.replace(1, 2, 100, 0));
    CHECK(f.recorder.back() == "REJ 1 2");
    CHECK_EQ(f.recorder.log.size(), 1u);
    CHECK_EQ(f.book.resting_orders(), 1u);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 5u);
}

TEST(replace_to_non_positive_price_leaves_the_original_resting) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.recorder.clear();
    CHECK(!f.book.replace(1, 2, 0, 5));
    CHECK(f.recorder.back() == "REJ 1 3");
    CHECK_EQ(f.book.resting_orders(), 1u);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 5u);
}

TEST(replace_onto_a_live_id_leaves_both_orders_resting) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.book.submit(limit(2, Side::Buy, 99, 7));
    f.recorder.clear();
    CHECK(!f.book.replace(1, 2, 98, 5));
    CHECK(f.recorder.back() == "REJ 1 0");
    CHECK_EQ(f.book.resting_orders(), 2u);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 5u);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 99), 7u);
}

TEST(replace_may_keep_the_same_order_id) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    CHECK(f.book.replace(1, 1, 101, 7));
    CHECK_EQ(f.book.resting_orders(), 1u);
    CHECK_EQ(f.book.best_bid().value(), 101);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 101), 7u);
}

TEST(a_price_above_the_band_is_rejected) {
    Recorder recorder;
    OrderBook book{[&](const Event& e) { recorder(e); }, {100, 200}};
    book.submit(limit(1, Side::Buy, 201, 5));
    CHECK(recorder.back() == "REJ 1 6");
    CHECK_EQ(book.resting_orders(), 0u);
}

TEST(a_price_below_the_band_is_rejected) {
    Recorder recorder;
    OrderBook book{[&](const Event& e) { recorder(e); }, {100, 200}};
    book.submit(limit(1, Side::Sell, 99, 5));
    CHECK(recorder.back() == "REJ 1 6");
    CHECK_EQ(book.resting_orders(), 0u);
}

TEST(the_band_edges_are_tradable) {
    Recorder recorder;
    OrderBook book{[&](const Event& e) { recorder(e); }, {100, 200}};
    book.submit(limit(1, Side::Buy, 100, 5));
    book.submit(limit(2, Side::Sell, 200, 5));
    CHECK_EQ(book.best_bid().value(), 100);
    CHECK_EQ(book.best_ask().value(), 200);
    CHECK_EQ(book.resting_orders(), 2u);
}

TEST(a_market_order_is_not_band_checked) {
    Recorder recorder;
    OrderBook book{[&](const Event& e) { recorder(e); }, {100, 200}};
    book.submit(limit(1, Side::Sell, 150, 5));
    recorder.clear();
    book.submit(market(2, Side::Buy, 5));
    CHECK(recorder.log[1] == "TRD 2 1 150 5");
}

TEST(replace_outside_the_band_leaves_the_original_resting) {
    Recorder recorder;
    OrderBook book{[&](const Event& e) { recorder(e); }, {100, 200}};
    book.submit(limit(1, Side::Buy, 150, 5));
    recorder.clear();
    CHECK(!book.replace(1, 2, 201, 5));
    CHECK(recorder.back() == "REJ 1 6");
    CHECK_EQ(book.resting_orders(), 1u);
    CHECK_EQ(book.quantity_at(Side::Buy, 150), 5u);
}

TEST(an_inverted_band_is_rejected_at_construction) {
    bool threw = false;
    try {
        const OrderBook book{[](const Event&) {}, {200, 100}};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(level_is_removed_once_emptied) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.book.submit(limit(2, Side::Buy, 99, 5));
    f.book.submit(limit(3, Side::Sell, 100, 5));
    CHECK_EQ(f.book.best_bid().value(), 99);
    CHECK_EQ(f.book.quantity_at(Side::Buy, 100), 0u);
}

TEST(sequence_numbers_are_strictly_increasing) {
    Fixture f;
    f.book.submit(limit(1, Side::Buy, 100, 5));
    f.book.submit(limit(2, Side::Sell, 100, 5));
    f.book.submit(limit(3, Side::Buy, 99, 5));
    f.book.cancel(3);
    f.book.cancel(99);

    CHECK(!f.recorder.sequences.empty());
    for (std::size_t i = 1; i < f.recorder.sequences.size(); ++i)
        CHECK(f.recorder.sequences[i] > f.recorder.sequences[i - 1]);
    CHECK_EQ(f.book.sequence(), f.recorder.sequences.back());
    CHECK_EQ(f.recorder.sequences.size(), static_cast<std::size_t>(f.book.sequence()));
}

TEST_MAIN
