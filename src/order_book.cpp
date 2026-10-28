#include "ob/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace ob {

namespace {

// Does a resting level at `level_price` satisfy the taker's limit?
// A buyer accepts any ask at or below its limit; a seller any bid at or above.
constexpr bool crosses(Side taker_side, Price limit, Price level_price) noexcept {
  return taker_side == Side::Buy ? level_price <= limit : level_price >= limit;
}

SubmitResult reject(RejectReason reason) noexcept {
  return SubmitResult{OrderStatus::Rejected, reason, 0, 0};
}

template <class Map>
std::vector<Level> snapshot(const Map& book, std::size_t max_levels) {
  std::vector<Level> out;
  out.reserve(std::min(max_levels, book.size()));
  for (const auto& entry : book) {
    if (out.size() == max_levels) break;
    out.push_back(Level{entry.first, entry.second.total, entry.second.orders.size()});
  }
  return out;
}

}  // namespace

OrderBook::OrderBook(std::string symbol, TradeCallback on_trade)
    : symbol_(std::move(symbol)), on_trade_(std::move(on_trade)) {}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

// Consume liquidity from `opposite_book` (best price first, FIFO within a
// price) until the taker is filled or the next level no longer crosses.
// Returns the quantity filled. Trades execute at the MAKER's price, so a
// taker with a generous limit gets price improvement.
template <class Map>
Quantity OrderBook::match(Map& opposite_book, const Order& taker, Price limit) {
  Quantity remaining = taker.quantity;

  while (remaining > 0 && !opposite_book.empty()) {
    auto level_it = opposite_book.begin();
    const Price level_price = level_it->first;
    if (!crosses(taker.side, limit, level_price)) break;

    PriceLevel& level = level_it->second;
    while (remaining > 0 && !level.orders.empty()) {
      RestingOrder& maker = level.orders.front();
      const Quantity qty = std::min(remaining, maker.remaining);

      maker.remaining -= qty;
      level.total -= qty;
      remaining -= qty;

      if (on_trade_) {
        on_trade_(Trade{++trade_seq_, taker.id, maker.id, taker.side, level_price, qty});
      } else {
        ++trade_seq_;
      }

      if (maker.remaining == 0) {
        index_.erase(maker.id);
        level.orders.pop_front();
      }
    }
    if (level.orders.empty()) opposite_book.erase(level_it);
  }
  return taker.quantity - remaining;
}

// How much quantity could this taker execute right now (stopping early once
// `needed` is reached)? Used for fill-or-kill: we must know the answer BEFORE
// touching the book, because a FOK order may never produce partial trades.
template <class Map>
Quantity OrderBook::available(const Map& opposite_book, Side taker_side, Price limit,
                              Quantity needed) const {
  Quantity total = 0;
  for (const auto& entry : opposite_book) {
    if (!crosses(taker_side, limit, entry.first)) break;
    total += entry.second.total;
    if (total >= needed) break;
  }
  return total;
}

template <class Map>
void OrderBook::rest_in(Map& book, Side side, OrderId id, Price price, Quantity qty) {
  PriceLevel& level = book[price];  // creates the level if needed
  level.orders.push_back(RestingOrder{id, qty});
  level.total += qty;
  index_.emplace(id, Locator{side, price, &level, std::prev(level.orders.end())});
}

void OrderBook::remove_resting(const Locator& loc) {
  PriceLevel& level = *loc.level;
  level.total -= loc.it->remaining;
  level.orders.erase(loc.it);
  if (level.orders.empty()) {
    // `level` dangles after this; only the copied locator fields are used.
    if (loc.side == Side::Buy) {
      bids_.erase(loc.price);
    } else {
      asks_.erase(loc.price);
    }
  }
}

// ---------------------------------------------------------------------------
// Public operations
// ---------------------------------------------------------------------------

SubmitResult OrderBook::submit(const Order& order) {
  if (order.quantity == 0) return reject(RejectReason::ZeroQuantity);
  const bool is_market = order.type == OrderType::Market;
  if (!is_market && order.price <= 0) return reject(RejectReason::InvalidPrice);
  if (index_.count(order.id) != 0) return reject(RejectReason::DuplicateOrderId);

  const bool is_buy = order.side == Side::Buy;
  const TimeInForce tif = is_market ? TimeInForce::IOC : order.tif;
  // A market order is a limit order with an infinitely generous limit.
  const Price limit = is_market ? (is_buy ? std::numeric_limits<Price>::max()
                                          : std::numeric_limits<Price>::min())
                                : order.price;

  if (tif == TimeInForce::FOK) {
    const Quantity avail = is_buy ? available(asks_, order.side, limit, order.quantity)
                                  : available(bids_, order.side, limit, order.quantity);
    if (avail < order.quantity) {
      return SubmitResult{OrderStatus::Cancelled, RejectReason::None, 0, order.quantity};
    }
  }

  const Quantity filled =
      is_buy ? match(asks_, order, limit) : match(bids_, order, limit);
  const Quantity remaining = order.quantity - filled;

  if (remaining == 0) {
    return SubmitResult{OrderStatus::Filled, RejectReason::None, filled, 0};
  }
  if (tif == TimeInForce::GTC) {
    if (is_buy) {
      rest_in(bids_, order.side, order.id, order.price, remaining);
    } else {
      rest_in(asks_, order.side, order.id, order.price, remaining);
    }
    return SubmitResult{OrderStatus::Resting, RejectReason::None, filled, remaining};
  }
  // IOC (or market) remainder is dropped.
  return SubmitResult{OrderStatus::Cancelled, RejectReason::None, filled, remaining};
}

bool OrderBook::cancel(OrderId id) {
  const auto found = index_.find(id);
  if (found == index_.end()) return false;
  const Locator loc = found->second;  // copy: the map entry is erased next
  index_.erase(found);
  remove_resting(loc);
  return true;
}

SubmitResult OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
  const auto found = index_.find(id);
  if (found == index_.end()) return reject(RejectReason::UnknownOrder);
  // Validate BEFORE touching anything so a bad amend never destroys the order.
  if (new_quantity == 0) return reject(RejectReason::ZeroQuantity);
  if (new_price <= 0) return reject(RejectReason::InvalidPrice);

  const Locator& loc = found->second;
  if (new_price == loc.price && new_quantity <= loc.it->remaining) {
    loc.level->total -= loc.it->remaining - new_quantity;
    loc.it->remaining = new_quantity;
    return SubmitResult{OrderStatus::Resting, RejectReason::None, 0, new_quantity};
  }

  const Side side = loc.side;
  cancel(id);
  return submit(Order::limit(id, side, new_price, new_quantity));
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Price> OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  return asks_.begin()->first;
}

Quantity OrderBook::volume_at(Side side, Price price) const {
  if (side == Side::Buy) {
    const auto it = bids_.find(price);
    return it == bids_.end() ? 0 : it->second.total;
  }
  const auto it = asks_.find(price);
  return it == asks_.end() ? 0 : it->second.total;
}

std::optional<Order> OrderBook::find(OrderId id) const {
  const auto found = index_.find(id);
  if (found == index_.end()) return std::nullopt;
  const Locator& loc = found->second;
  return Order::limit(id, loc.side, loc.price, loc.it->remaining);
}

std::vector<Level> OrderBook::depth(Side side, std::size_t max_levels) const {
  return side == Side::Buy ? snapshot(bids_, max_levels) : snapshot(asks_, max_levels);
}

bool OrderBook::check_invariants(std::string* why) const {
  const auto fail = [&](const char* message) {
    if (why != nullptr) *why = message;
    return false;
  };

  std::size_t counted = 0;
  const auto check_side = [&](const auto& book, Side side) {
    for (const auto& entry : book) {
      const Price price = entry.first;
      const PriceLevel& level = entry.second;
      if (level.orders.empty()) return fail("empty price level left in book");
      Quantity sum = 0;
      for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
        if (it->remaining == 0) return fail("resting order with zero quantity");
        sum += it->remaining;
        ++counted;
        const auto found = index_.find(it->id);
        if (found == index_.end()) return fail("resting order missing from id index");
        const Locator& loc = found->second;
        if (loc.side != side || loc.price != price || loc.level != &level ||
            loc.it != it) {
          return fail("id index locator disagrees with book");
        }
      }
      if (sum != level.total) return fail("level total != sum of its orders");
    }
    return true;
  };

  if (!check_side(bids_, Side::Buy)) return false;
  if (!check_side(asks_, Side::Sell)) return false;
  if (counted != index_.size()) return fail("id index has orders not on the book");
  if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
    return fail("book is crossed (best bid >= best ask)");
  }
  return true;
}

}  // namespace ob
