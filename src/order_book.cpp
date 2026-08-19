#include "matchbook/order_book.hpp"

#include <algorithm>
#include <utility>

namespace matchbook {

OrderBook::OrderBook(EventSink sink, std::size_t expected_orders) : sink_(std::move(sink)) {
    pool_.reserve(expected_orders);
    free_slots_.reserve(expected_orders);
    index_.reserve(expected_orders);
}

std::uint32_t OrderBook::acquire(const Node& node) {
    if (!free_slots_.empty()) {
        const std::uint32_t slot = free_slots_.back();
        free_slots_.pop_back();
        pool_[slot] = node;
        return slot;
    }
    pool_.push_back(node);
    return static_cast<std::uint32_t>(pool_.size() - 1);
}

void OrderBook::release(std::uint32_t slot) { free_slots_.push_back(slot); }

void OrderBook::emit_accepted(const NewOrder& order) {
    Event event{};
    event.kind = Event::Kind::Accepted;
    event.sequence = ++sequence_;
    event.accepted = {order.id, order.side, order.price, order.quantity};
    sink_(event);
}

void OrderBook::emit_rejected(OrderId id, RejectReason reason) {
    Event event{};
    event.kind = Event::Kind::Rejected;
    event.sequence = ++sequence_;
    event.rejected = {id, reason};
    sink_(event);
}

void OrderBook::emit_trade(OrderId taker, OrderId maker, Side taker_side, Price price,
                           Quantity qty) {
    Event event{};
    event.kind = Event::Kind::Trade;
    event.sequence = ++sequence_;
    event.trade = {taker, maker, taker_side, price, qty};
    sink_(event);
}

void OrderBook::emit_canceled(OrderId id, Quantity remaining) {
    Event event{};
    event.kind = Event::Kind::Canceled;
    event.sequence = ++sequence_;
    event.canceled = {id, remaining};
    sink_(event);
}

bool OrderBook::crosses(Side taker, OrderType type, Price limit, Price resting) const {
    if (type == OrderType::Market) return true;
    return taker == Side::Buy ? limit >= resting : limit <= resting;
}

Quantity OrderBook::fillable(Side side, OrderType type, Price limit) const {
    Quantity total = 0;
    if (side == Side::Buy) {
        for (const auto& [price, level] : asks_) {
            if (!crosses(side, type, limit, price)) break;
            total += level.total;
        }
    } else {
        for (const auto& [price, level] : bids_) {
            if (!crosses(side, type, limit, price)) break;
            total += level.total;
        }
    }
    return total;
}

template <typename Ladder>
Quantity OrderBook::match(Ladder& ladder, const NewOrder& order, Quantity remaining) {
    while (remaining > 0 && !ladder.empty()) {
        auto best = ladder.begin();
        if (!crosses(order.side, order.type, order.price, best->first)) break;

        Level& level = best->second;
        while (remaining > 0 && level.head != npos) {
            Node& maker = pool_[level.head];
            const Quantity traded = std::min(remaining, maker.remaining);

            maker.remaining -= traded;
            remaining -= traded;
            level.total -= traded;
            emit_trade(order.id, maker.id, order.side, best->first, traded);

            if (maker.remaining == 0) {
                const std::uint32_t slot = level.head;
                level.head = maker.next;
                if (level.head == npos) {
                    level.tail = npos;
                } else {
                    pool_[level.head].prev = npos;
                }
                index_.erase(maker.id);
                release(slot);
            }
        }
        if (level.head == npos) ladder.erase(best);
    }
    return remaining;
}

void OrderBook::rest(const NewOrder& order, Quantity remaining) {
    const std::uint32_t slot = acquire(Node{order.id, order.side, order.price, remaining, npos, npos});
    index_.emplace(order.id, slot);

    Level* level = nullptr;
    if (order.side == Side::Buy) {
        level = &bids_[order.price];
    } else {
        level = &asks_[order.price];
    }

    if (level->tail == npos) {
        level->head = level->tail = slot;
    } else {
        pool_[level->tail].next = slot;
        pool_[slot].prev = level->tail;
        level->tail = slot;
    }
    level->total += remaining;
}

void OrderBook::submit(const NewOrder& order) {
    if (order.quantity == 0) {
        emit_rejected(order.id, RejectReason::ZeroQuantity);
        return;
    }
    if (index_.contains(order.id)) {
        emit_rejected(order.id, RejectReason::DuplicateOrderId);
        return;
    }
    if (order.type == OrderType::Limit && order.price <= 0) {
        emit_rejected(order.id, RejectReason::InvalidPrice);
        return;
    }
    if (order.tif == TimeInForce::FOK &&
        fillable(order.side, order.type, order.price) < order.quantity) {
        emit_rejected(order.id, RejectReason::FillOrKillUnfillable);
        return;
    }
    const bool no_liquidity = order.side == Side::Buy ? asks_.empty() : bids_.empty();
    if (order.type == OrderType::Market && no_liquidity) {
        emit_rejected(order.id, RejectReason::MarketOrderNoLiquidity);
        return;
    }

    emit_accepted(order);

    const Quantity remaining = order.side == Side::Buy ? match(asks_, order, order.quantity)
                                                       : match(bids_, order, order.quantity);
    if (remaining == 0) return;

    if (order.type == OrderType::Limit && order.tif == TimeInForce::GTC) {
        rest(order, remaining);
    } else {
        emit_canceled(order.id, remaining);
    }
}

void OrderBook::unlink(Side side, Price price, std::uint32_t slot) {
    Node& node = pool_[slot];
    auto detach = [&](auto& ladder) {
        auto it = ladder.find(price);
        if (it == ladder.end()) return;
        Level& level = it->second;
        if (node.prev != npos) {
            pool_[node.prev].next = node.next;
        } else {
            level.head = node.next;
        }
        if (node.next != npos) {
            pool_[node.next].prev = node.prev;
        } else {
            level.tail = node.prev;
        }
        level.total -= node.remaining;
        if (level.head == npos) ladder.erase(it);
    };
    if (side == Side::Buy) {
        detach(bids_);
    } else {
        detach(asks_);
    }
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        emit_rejected(id, RejectReason::UnknownOrderId);
        return false;
    }
    const std::uint32_t slot = it->second;
    const Node node = pool_[slot];
    unlink(node.side, node.price, slot);
    index_.erase(it);
    release(slot);
    emit_canceled(id, node.remaining);
    return true;
}

bool OrderBook::replace(OrderId old_id, OrderId new_id, Price price, Quantity quantity) {
    auto it = index_.find(old_id);
    if (it == index_.end()) {
        emit_rejected(old_id, RejectReason::UnknownOrderId);
        return false;
    }
    const Node node = pool_[it->second];
    const std::uint32_t slot = it->second;
    unlink(node.side, node.price, slot);
    index_.erase(it);
    release(slot);

    Event event{};
    event.kind = Event::Kind::Replaced;
    event.sequence = ++sequence_;
    event.replaced = {old_id, new_id, price, quantity};
    sink_(event);

    // A replace loses time priority, which is why it re-enters as a new order.
    submit(NewOrder{new_id, node.side, OrderType::Limit, TimeInForce::GTC, price, quantity});
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total;
}

}  // namespace matchbook
