#pragma once

#include <cstdint>
#include <functional>

#include "matchbook/types.hpp"

namespace matchbook {

enum class RejectReason : std::uint8_t {
    DuplicateOrderId,
    UnknownOrderId,
    ZeroQuantity,
    InvalidPrice,
    FillOrKillUnfillable,
    MarketOrderNoLiquidity,
};

struct Accepted {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity quantity{};
};

struct Rejected {
    OrderId id{};
    RejectReason reason{};
};

// Emitted once per match. `price` is the resting order's price, which is the
// convention that makes the passive side's limit honoured.
struct Trade {
    OrderId taker{};
    OrderId maker{};
    Side taker_side{};
    Price price{};
    Quantity quantity{};
};

struct Canceled {
    OrderId id{};
    Quantity remaining{};
};

struct Replaced {
    OrderId old_id{};
    OrderId new_id{};
    Price price{};
    Quantity quantity{};
};

struct Event {
    enum class Kind : std::uint8_t { Accepted, Rejected, Trade, Canceled, Replaced } kind{};
    Sequence sequence{};
    union {
        Accepted accepted;
        Rejected rejected;
        Trade trade;
        Canceled canceled;
        Replaced replaced;
    };
};

using EventSink = std::function<void(const Event&)>;

}  // namespace matchbook
