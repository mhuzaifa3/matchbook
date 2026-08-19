#pragma once

#include <cstdint>

namespace matchbook {

using OrderId = std::uint64_t;
using Price = std::int64_t;     // integer ticks, never floating point
using Quantity = std::uint64_t;
using Sequence = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t { Limit, Market };

enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

struct NewOrder {
    OrderId id{};
    Side side{};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    Price price{};
    Quantity quantity{};
};

}  // namespace matchbook
