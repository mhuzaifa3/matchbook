#include "matchbook/order_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace matchbook {

namespace {

// The ladders are sized from the band in the member init list, so the band has
// to be checked before that runs or an inverted band allocates a huge vector.
std::size_t checked_ticks(OrderBook::PriceBand band) {
    if (band.min > band.max) throw std::invalid_argument("price band is inverted");
    if (band.ticks() > Occupancy::max_ticks) throw std::invalid_argument("price band is too wide");
    return band.ticks();
}

}  // namespace

OrderBook::OrderBook(EventSink sink, PriceBand band, std::size_t expected_orders)
    : sink_(std::move(sink)),
      band_(band),
      bids_(checked_ticks(band)),
      asks_(checked_ticks(band)) {
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
    const Ladder& book = ladder(opposite(side));
    const bool ascending = side == Side::Buy;
    Quantity total = 0;
    std::size_t at = ascending ? book.occupied.lowest() : book.occupied.highest();
    while (at != Occupancy::npos) {
        if (!crosses(side, type, limit, price_at(at))) break;
        total += book.levels[at].total;
        at = ascending ? book.occupied.next_above(at) : book.occupied.next_below(at);
    }
    return total;
}

std::optional<RejectReason> OrderBook::price_rejection(OrderType type, Price price) const {
    if (type != OrderType::Limit) return std::nullopt;
    if (price <= 0) return RejectReason::InvalidPrice;
    if (!band_.contains(price)) return RejectReason::PriceOutsideBand;
    return std::nullopt;
}

Quantity OrderBook::match(const NewOrder& order, Quantity remaining) {
    Ladder& book = ladder(opposite(order.side));
    const bool ascending = order.side == Side::Buy;

    while (remaining > 0) {
        const std::size_t best = ascending ? book.occupied.lowest() : book.occupied.highest();
        if (best == Occupancy::npos) break;
        const Price price = price_at(best);
        if (!crosses(order.side, order.type, order.price, price)) break;

        Level& level = book.levels[best];
        while (remaining > 0 && level.head != npos) {
            Node& maker = pool_[level.head];
            const Quantity traded = std::min(remaining, maker.remaining);

            maker.remaining -= traded;
            remaining -= traded;
            level.total -= traded;
            emit_trade(order.id, maker.id, order.side, price, traded);

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
        if (level.head == npos) book.occupied.clear(best);
    }
    return remaining;
}

void OrderBook::rest(const NewOrder& order, Quantity remaining) {
    const std::uint32_t slot =
        acquire(Node{order.id, order.side, order.price, remaining, npos, npos});
    index_.emplace(order.id, slot);

    Ladder& book = ladder(order.side);
    const std::size_t at = index_of(order.price);
    Level& level = book.levels[at];

    if (level.tail == npos) {
        level.head = level.tail = slot;
        book.occupied.set(at);
    } else {
        pool_[level.tail].next = slot;
        pool_[slot].prev = level.tail;
        level.tail = slot;
    }
    level.total += remaining;
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
    if (const auto reason = price_rejection(order.type, order.price)) {
        emit_rejected(order.id, *reason);
        return;
    }
    if (order.tif == TimeInForce::FOK &&
        fillable(order.side, order.type, order.price) < order.quantity) {
        emit_rejected(order.id, RejectReason::FillOrKillUnfillable);
        return;
    }
    const bool no_liquidity = ladder(opposite(order.side)).occupied.empty();
    if (order.type == OrderType::Market && no_liquidity) {
        emit_rejected(order.id, RejectReason::MarketOrderNoLiquidity);
        return;
    }

    emit_accepted(order);

    const Quantity remaining = match(order, order.quantity);
    if (remaining == 0) return;

    if (order.type == OrderType::Limit && order.tif == TimeInForce::GTC) {
        rest(order, remaining);
    } else {
        emit_canceled(order.id, remaining);
    }
}

void OrderBook::unlink(Side side, Price price, std::uint32_t slot) {
    Ladder& book = ladder(side);
    const std::size_t at = index_of(price);
    Level& level = book.levels[at];
    Node& node = pool_[slot];

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
    if (level.head == npos) book.occupied.clear(at);
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
    // The resubmit below validates too late: by then the old order is gone.
    // Same order of checks as submit, so both reject for the same reason.
    if (quantity == 0) {
        emit_rejected(old_id, RejectReason::ZeroQuantity);
        return false;
    }
    if (new_id != old_id && index_.contains(new_id)) {
        emit_rejected(old_id, RejectReason::DuplicateOrderId);
        return false;
    }
    if (const auto reason = price_rejection(OrderType::Limit, price)) {
        emit_rejected(old_id, *reason);
        return false;
    }

    const std::uint32_t slot = it->second;
    const Node node = pool_[slot];
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
    const std::size_t at = bids_.occupied.highest();
    if (at == Occupancy::npos) return std::nullopt;
    return price_at(at);
}

std::optional<Price> OrderBook::best_ask() const {
    const std::size_t at = asks_.occupied.lowest();
    if (at == Occupancy::npos) return std::nullopt;
    return price_at(at);
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    if (!band_.contains(price)) return 0;
    return ladder(side).levels[index_of(price)].total;
}

}  // namespace matchbook
