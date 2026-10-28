#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ob/types.hpp"

namespace ob {

// A single-instrument limit order book with price-time priority matching.
//
// Data structures (n = resting orders, L = price levels):
//   * Each side is a std::map keyed by price (best price first), so finding
//     the best level is O(1) and inserting a new level is O(log L).
//   * Each level is a std::list of orders in arrival order (FIFO = time
//     priority). Appending and removing from the middle are O(1) and, unlike
//     vector/deque, never invalidate other orders' iterators.
//   * An unordered_map from order id to a "locator" (side, price, level,
//     list iterator) makes cancel O(1) average instead of a scan.
//
// Not thread-safe: use one book per thread/strand, or add external locking.
// The trade callback must not call back into the same book.
class OrderBook {
 public:
  using TradeCallback = std::function<void(const Trade&)>;

  explicit OrderBook(std::string symbol = {}, TradeCallback on_trade = {});

  // The book stores raw pointers into its own maps, so it must stay put.
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  void set_trade_callback(TradeCallback cb) { on_trade_ = std::move(cb); }

  // Pre-size the id index; avoids rehashing during steady-state operation.
  void reserve_orders(std::size_t n) { index_.reserve(n); }

  // Match the order against the book, then rest any GTC remainder.
  // Complexity: O(k log L) for k price levels consumed, plus O(1) per fill.
  SubmitResult submit(const Order& order);

  // Remove a resting order. Returns false if the id is not on the book
  // (never existed, already filled, or already cancelled). O(1) average.
  bool cancel(OrderId id);

  // Change the open quantity and/or price of a resting order.
  //  * Same price and new_quantity <= current open quantity: amended in place,
  //    time priority is KEPT (this matches most exchanges).
  //  * Otherwise (price change or quantity increase): cancel/replace. The
  //    order loses time priority, is re-submitted as a GTC limit order, and
  //    may match immediately if the new price crosses.
  // On any rejection the original order is left untouched.
  SubmitResult modify(OrderId id, Price new_price, Quantity new_quantity);

  // ---- Queries ----------------------------------------------------------
  const std::string& symbol() const noexcept { return symbol_; }
  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  Quantity volume_at(Side side, Price price) const;
  bool contains(OrderId id) const { return index_.count(id) != 0; }
  // The resting order with its *remaining* quantity, if present.
  std::optional<Order> find(OrderId id) const;
  std::size_t order_count() const noexcept { return index_.size(); }
  std::size_t level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
  }
  std::uint64_t trade_count() const noexcept { return trade_seq_; }
  // Top-of-book first. At most max_levels entries.
  std::vector<Level> depth(Side side, std::size_t max_levels) const;

  // Walks every internal structure and verifies they agree with each other
  // (used by tests; O(n)). Returns false and fills `why` on the first problem.
  bool check_invariants(std::string* why = nullptr) const;

 private:
  struct RestingOrder {
    OrderId id;
    Quantity remaining;
  };
  struct PriceLevel {
    std::list<RestingOrder> orders;
    Quantity total{0};
  };
  using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskMap = std::map<Price, PriceLevel, std::less<Price>>;
  struct Locator {
    Side side;
    Price price;
    PriceLevel* level;  // std::map nodes are address-stable
    std::list<RestingOrder>::iterator it;
  };

  template <class Map>
  Quantity match(Map& opposite_book, const Order& taker, Price limit);
  template <class Map>
  Quantity available(const Map& opposite_book, Side taker_side, Price limit,
                     Quantity needed) const;
  template <class Map>
  void rest_in(Map& book, Side side, OrderId id, Price price, Quantity qty);
  void remove_resting(const Locator& loc);

  std::string symbol_;
  TradeCallback on_trade_;
  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Locator> index_;
  std::uint64_t trade_seq_{0};
};

}  // namespace ob
