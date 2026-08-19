#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "matchbook/events.hpp"
#include "matchbook/types.hpp"

namespace matchbook {

// Price-time priority limit order book.
//
// Event ordering contract, which the differential tests pin: a submission
// emits Rejected, or else Accepted followed by zero or more Trades, followed
// by Canceled if any quantity is left unfilled and the order cannot rest.
class OrderBook {
public:
    // `expected_orders` pre-sizes the pool and index. Growth is otherwise a
    // tail-latency source: a rehash or pool reallocation stalls one submit by
    // milliseconds at a million resting orders.
    explicit OrderBook(EventSink sink, std::size_t expected_orders = 1024);

    void submit(const NewOrder& order);
    bool cancel(OrderId id);
    bool replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;
    [[nodiscard]] std::size_t resting_orders() const { return index_.size(); }
    [[nodiscard]] Sequence sequence() const { return sequence_; }

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

    // Intrusive FIFO of pool indices, so cancel is O(1) once the id is resolved.
    struct Level {
        std::uint32_t head{npos};
        std::uint32_t tail{npos};
        Quantity total{};
    };

    using BidLadder = std::map<Price, Level, std::greater<>>;
    using AskLadder = std::map<Price, Level, std::less<>>;

    std::uint32_t acquire(const Node& node);
    void release(std::uint32_t slot);

    void emit_accepted(const NewOrder& order);
    void emit_rejected(OrderId id, RejectReason reason);
    void emit_trade(OrderId taker, OrderId maker, Side taker_side, Price price, Quantity qty);
    void emit_canceled(OrderId id, Quantity remaining);

    [[nodiscard]] bool crosses(Side taker, OrderType type, Price limit, Price resting) const;
    [[nodiscard]] Quantity fillable(Side side, OrderType type, Price limit) const;

    template <typename Ladder>
    Quantity match(Ladder& ladder, const NewOrder& order, Quantity remaining);

    void rest(const NewOrder& order, Quantity remaining);
    void unlink(Side side, Price price, std::uint32_t slot);

    EventSink sink_;
    std::vector<Node> pool_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<OrderId, std::uint32_t> index_;
    BidLadder bids_;
    AskLadder asks_;
    Sequence sequence_{0};
};

}  // namespace matchbook
