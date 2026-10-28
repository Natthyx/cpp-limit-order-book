#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ob {

// Prices are integers measured in "ticks" (the smallest price increment).
// Using integers avoids floating-point comparison bugs in matching logic:
// e.g. price 10050 with a tick of 0.01 means 100.50.
using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// GTC: good-till-cancelled, any unfilled remainder rests on the book.
// IOC: immediate-or-cancel, fill what is possible now, drop the rest.
// FOK: fill-or-kill, fill the entire quantity now or do nothing at all.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

struct Order {
  OrderId id{0};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TimeInForce tif{TimeInForce::GTC};
  Price price{0};  // ignored for market orders
  Quantity quantity{0};

  static constexpr Order limit(OrderId id, Side side, Price price, Quantity qty,
                               TimeInForce tif = TimeInForce::GTC) noexcept {
    return Order{id, side, OrderType::Limit, tif, price, qty};
  }

  // Market orders never rest: they are always treated as IOC.
  static constexpr Order market(OrderId id, Side side, Quantity qty) noexcept {
    return Order{id, side, OrderType::Market, TimeInForce::IOC, 0, qty};
  }
};

struct Trade {
  std::uint64_t sequence{0};  // strictly increasing per order book
  OrderId taker_id{0};        // the incoming (aggressing) order
  OrderId maker_id{0};        // the resting order that was hit
  Side taker_side{Side::Buy};
  Price price{0};  // always the resting (maker) order's price
  Quantity quantity{0};
};

constexpr bool operator==(const Trade& a, const Trade& b) noexcept {
  return a.sequence == b.sequence && a.taker_id == b.taker_id &&
         a.maker_id == b.maker_id && a.taker_side == b.taker_side &&
         a.price == b.price && a.quantity == b.quantity;
}
constexpr bool operator!=(const Trade& a, const Trade& b) noexcept {
  return !(a == b);
}

enum class OrderStatus : std::uint8_t {
  Resting,    // some or all of the order is now on the book
  Filled,     // completely executed
  Cancelled,  // IOC/market/FOK remainder dropped (possibly after partial fill)
  Rejected    // failed validation, see RejectReason
};

enum class RejectReason : std::uint8_t {
  None,
  DuplicateOrderId,
  ZeroQuantity,
  InvalidPrice,
  UnknownOrder
};

struct SubmitResult {
  OrderStatus status{OrderStatus::Rejected};
  RejectReason reason{RejectReason::None};
  Quantity filled{0};     // quantity executed during this call
  Quantity remaining{0};  // resting quantity (Resting) or dropped quantity (Cancelled)
};

// One aggregated price level of the book.
struct Level {
  Price price{0};
  Quantity quantity{0};
  std::size_t orders{0};
};

inline const char* to_string(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}
inline const char* to_string(TimeInForce t) noexcept {
  switch (t) {
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::IOC: return "IOC";
    case TimeInForce::FOK: return "FOK";
  }
  return "?";
}
inline const char* to_string(OrderStatus s) noexcept {
  switch (s) {
    case OrderStatus::Resting: return "RESTING";
    case OrderStatus::Filled: return "FILLED";
    case OrderStatus::Cancelled: return "CANCELLED";
    case OrderStatus::Rejected: return "REJECTED";
  }
  return "?";
}
inline const char* to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None: return "none";
    case RejectReason::DuplicateOrderId: return "duplicate_order_id";
    case RejectReason::ZeroQuantity: return "zero_quantity";
    case RejectReason::InvalidPrice: return "invalid_price";
    case RejectReason::UnknownOrder: return "unknown_order";
  }
  return "?";
}

}  // namespace ob
