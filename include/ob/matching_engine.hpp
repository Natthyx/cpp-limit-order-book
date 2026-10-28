#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ob/order_book.hpp"

namespace ob {

// Multi-symbol front end: owns one OrderBook per symbol, assigns globally
// unique order ids, and returns the trades each call produced.
class MatchingEngine {
 public:
  struct Report {
    OrderId id{0};  // 0 when the request referred to an existing order only
    SubmitResult result{};
    std::vector<Trade> trades;
  };

  MatchingEngine() = default;
  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  Report submit_limit(const std::string& symbol, Side side, Price price, Quantity qty,
                      TimeInForce tif = TimeInForce::GTC);
  Report submit_market(const std::string& symbol, Side side, Quantity qty);

  // Cancel/modify need the symbol, like a FIX cancel request, so the engine
  // does not have to remember every id forever.
  bool cancel(const std::string& symbol, OrderId id);
  Report modify(const std::string& symbol, OrderId id, Price new_price, Quantity new_qty);

  // nullptr if the symbol has never been traded.
  const OrderBook* find_book(const std::string& symbol) const;
  std::vector<std::string> symbols() const;

 private:
  OrderBook& book_for(const std::string& symbol);
  Report finish(OrderId id, const SubmitResult& result);

  std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
  std::vector<Trade> pending_;  // filled by the per-book trade callbacks
  OrderId next_id_{1};
};

}  // namespace ob
