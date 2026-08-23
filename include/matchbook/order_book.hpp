#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "matchbook/events.hpp"
#include "matchbook/occupancy.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

// Price-time priority limit order book over a fixed band of ticks.
//
// Event ordering contract, which the differential tests pin: a submission
// emits Rejected, or else Accepted followed by zero or more Trades, followed
// by Canceled if any quantity is left unfilled and the order cannot rest.
class OrderBook {
public:
    // Instruments trade inside a band, the way a venue applies price banding or
    // limit-up limit-down. Bounding it turns a level lookup into an array index.
    struct PriceBand {
        Price min{};
        Price max{};

        [[nodiscard]] std::size_t ticks() const { return static_cast<std::size_t>(max - min) + 1; }
        [[nodiscard]] bool contains(Price price) const { return price >= min && price <= max; }
    };

    // Throws std::invalid_argument unless min <= max and the band fits
    // Occupancy::max_ticks. `expected_orders` pre-sizes the pool and index,
    // which is otherwise a tail-latency source: a rehash or pool reallocation
    // stalls one submit by milliseconds at a million resting orders.
    OrderBook(EventSink sink, PriceBand band, std::size_t expected_orders = 1024);

    void submit(const NewOrder& order);
    bool cancel(OrderId id);
    bool replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;
    [[nodiscard]] std::size_t resting_orders() const { return index_.size(); }
    [[nodiscard]] Sequence sequence() const { return sequence_; }
    [[nodiscard]] PriceBand band() const { return band_; }

private:
    static constexpr std::uint32_t npos = ~0U;

    struct Node {
        OrderId id{};
        Side side{};
        Price price{};
        Quantity remaining{};
        std::uint32_t prev{npos};
        std::uint32_t next{npos};
    };

    // Intrusive FIFO of pool indices, so unlinking an order is O(1).
    struct Level {
        std::uint32_t head{npos};
        std::uint32_t tail{npos};
        Quantity total{};
    };

    // One flat ladder per side, indexed by tick. The bitmap records which ticks
    // hold orders, so the best price and the next level in from it cost O(1).
    struct Ladder {
        std::vector<Level> levels;
        Occupancy occupied;

        explicit Ladder(std::size_t ticks) : levels(ticks), occupied(ticks) {}
    };

    std::uint32_t acquire(const Node& node);
    void release(std::uint32_t slot);

    void emit_accepted(const NewOrder& order);
    void emit_rejected(OrderId id, RejectReason reason);
    void emit_trade(OrderId taker, OrderId maker, Side taker_side, Price price, Quantity qty);
    void emit_canceled(OrderId id, Quantity remaining);

    [[nodiscard]] std::size_t index_of(Price price) const {
        return static_cast<std::size_t>(price - band_.min);
    }
    [[nodiscard]] Price price_at(std::size_t index) const {
        return band_.min + static_cast<Price>(index);
    }
    [[nodiscard]] Ladder& ladder(Side side) { return side == Side::Buy ? bids_ : asks_; }
    [[nodiscard]] const Ladder& ladder(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    [[nodiscard]] bool crosses(Side taker, OrderType type, Price limit, Price resting) const;
    [[nodiscard]] Quantity fillable(Side side, OrderType type, Price limit) const;
    [[nodiscard]] std::optional<RejectReason> price_rejection(OrderType type, Price price) const;

    Quantity match(const NewOrder& order, Quantity remaining);
    void rest(const NewOrder& order, Quantity remaining);
    void unlink(Side side, Price price, std::uint32_t slot);

    EventSink sink_;
    PriceBand band_;
    std::vector<Node> pool_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<OrderId, std::uint32_t> index_;
    Ladder bids_;
    Ladder asks_;
    Sequence sequence_{0};
};

}  // namespace matchbook
