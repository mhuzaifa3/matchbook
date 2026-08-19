#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "matchbook/events.hpp"
#include "matchbook/types.hpp"

namespace matchbook::testing {

// Deliberately naive: a flat vector scanned linearly for the best eligible
// order. Obviously correct rather than fast, and used only as the oracle the
// real book is differentially tested against.
class ReferenceBook {
public:
    explicit ReferenceBook(EventSink sink) : sink_(std::move(sink)) {}

    void submit(const NewOrder& order) {
        if (order.quantity == 0) return reject(order.id, RejectReason::ZeroQuantity);
        if (find(order.id) != nullptr) return reject(order.id, RejectReason::DuplicateOrderId);
        if (order.type == OrderType::Limit && order.price <= 0)
            return reject(order.id, RejectReason::InvalidPrice);
        if (order.tif == TimeInForce::FOK && fillable(order) < order.quantity)
            return reject(order.id, RejectReason::FillOrKillUnfillable);
        if (order.type == OrderType::Market && !has_any(opposite(order.side)))
            return reject(order.id, RejectReason::MarketOrderNoLiquidity);

        emit_accepted(order);

        Quantity remaining = order.quantity;
        while (remaining > 0) {
            Resting* best = best_match(order);
            if (best == nullptr) break;
            const Quantity traded = std::min(remaining, best->remaining);
            best->remaining -= traded;
            remaining -= traded;
            emit_trade(order.id, best->id, order.side, best->price, traded);
            if (best->remaining == 0) erase(best->id);
        }

        if (remaining == 0) return;
        if (order.type == OrderType::Limit && order.tif == TimeInForce::GTC) {
            resting_.push_back({order.id, order.side, order.price, remaining, next_seq_++});
        } else {
            emit_canceled(order.id, remaining);
        }
    }

    bool cancel(OrderId id) {
        Resting* found = find(id);
        if (found == nullptr) {
            reject(id, RejectReason::UnknownOrderId);
            return false;
        }
        const Quantity remaining = found->remaining;
        erase(id);
        emit_canceled(id, remaining);
        return true;
    }

    bool replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity) {
        Resting* found = find(old_id);
        if (found == nullptr) {
            reject(old_id, RejectReason::UnknownOrderId);
            return false;
        }
        const Side side = found->side;
        erase(old_id);

        Event event{};
        event.kind = Event::Kind::Replaced;
        event.sequence = ++sequence_;
        event.replaced = {old_id, new_id, price, quantity};
        sink_(event);

        submit(NewOrder{new_id, side, OrderType::Limit, TimeInForce::GTC, price, quantity});
        return true;
    }

    [[nodiscard]] std::optional<Price> best_bid() const { return best_price(Side::Buy); }
    [[nodiscard]] std::optional<Price> best_ask() const { return best_price(Side::Sell); }

    [[nodiscard]] Quantity quantity_at(Side side, Price price) const {
        Quantity total = 0;
        for (const auto& order : resting_)
            if (order.side == side && order.price == price) total += order.remaining;
        return total;
    }

    [[nodiscard]] std::size_t resting_orders() const { return resting_.size(); }

private:
    struct Resting {
        OrderId id;
        Side side;
        Price price;
        Quantity remaining;
        std::uint64_t seq;
    };

    Resting* find(OrderId id) {
        for (auto& order : resting_)
            if (order.id == id) return &order;
        return nullptr;
    }

    void erase(OrderId id) {
        resting_.erase(std::remove_if(resting_.begin(), resting_.end(),
                                      [id](const Resting& o) { return o.id == id; }),
                       resting_.end());
    }

    [[nodiscard]] bool has_any(Side side) const {
        for (const auto& order : resting_)
            if (order.side == side) return true;
        return false;
    }

    [[nodiscard]] static bool crosses(const NewOrder& order, Price resting) {
        if (order.type == OrderType::Market) return true;
        return order.side == Side::Buy ? order.price >= resting : order.price <= resting;
    }

    [[nodiscard]] Quantity fillable(const NewOrder& order) const {
        Quantity total = 0;
        for (const auto& candidate : resting_)
            if (candidate.side == opposite(order.side) && crosses(order, candidate.price))
                total += candidate.remaining;
        return total;
    }

    Resting* best_match(const NewOrder& order) {
        Resting* best = nullptr;
        for (auto& candidate : resting_) {
            if (candidate.side != opposite(order.side)) continue;
            if (!crosses(order, candidate.price)) continue;
            if (best == nullptr) {
                best = &candidate;
                continue;
            }
            const bool better_price = order.side == Side::Buy ? candidate.price < best->price
                                                              : candidate.price > best->price;
            if (better_price || (candidate.price == best->price && candidate.seq < best->seq))
                best = &candidate;
        }
        return best;
    }

    [[nodiscard]] std::optional<Price> best_price(Side side) const {
        std::optional<Price> best;
        for (const auto& order : resting_) {
            if (order.side != side) continue;
            if (!best.has_value() || (side == Side::Buy ? order.price > *best : order.price < *best))
                best = order.price;
        }
        return best;
    }

    void reject(OrderId id, RejectReason reason) {
        Event event{};
        event.kind = Event::Kind::Rejected;
        event.sequence = ++sequence_;
        event.rejected = {id, reason};
        sink_(event);
    }

    void emit_accepted(const NewOrder& order) {
        Event event{};
        event.kind = Event::Kind::Accepted;
        event.sequence = ++sequence_;
        event.accepted = {order.id, order.side, order.price, order.quantity};
        sink_(event);
    }

    void emit_trade(OrderId taker, OrderId maker, Side side, Price price, Quantity qty) {
        Event event{};
        event.kind = Event::Kind::Trade;
        event.sequence = ++sequence_;
        event.trade = {taker, maker, side, price, qty};
        sink_(event);
    }

    void emit_canceled(OrderId id, Quantity remaining) {
        Event event{};
        event.kind = Event::Kind::Canceled;
        event.sequence = ++sequence_;
        event.canceled = {id, remaining};
        sink_(event);
    }

    EventSink sink_;
    std::vector<Resting> resting_;
    std::uint64_t next_seq_{0};
    Sequence sequence_{0};
};

}  // namespace matchbook::testing
