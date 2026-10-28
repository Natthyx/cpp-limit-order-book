#include "ob/matching_engine.hpp"

#include <algorithm>
#include <utility>

namespace ob {

OrderBook& MatchingEngine::book_for(const std::string& symbol) {
  auto it = books_.find(symbol);
  if (it == books_.end()) {
    auto book = std::make_unique<OrderBook>(
        symbol, [this](const Trade& t) { pending_.push_back(t); });
    it = books_.emplace(symbol, std::move(book)).first;
  }
  return *it->second;
}

MatchingEngine::Report MatchingEngine::finish(OrderId id, const SubmitResult& result) {
  Report report;
  report.id = id;
  report.result = result;
  report.trades.swap(pending_);  // hand over trades, leave pending_ empty
  return report;
}

MatchingEngine::Report MatchingEngine::submit_limit(const std::string& symbol, Side side,
                                                    Price price, Quantity qty,
                                                    TimeInForce tif) {
  OrderBook& book = book_for(symbol);
  const OrderId id = next_id_++;
  return finish(id, book.submit(Order::limit(id, side, price, qty, tif)));
}

MatchingEngine::Report MatchingEngine::submit_market(const std::string& symbol, Side side,
                                                     Quantity qty) {
  OrderBook& book = book_for(symbol);
  const OrderId id = next_id_++;
  return finish(id, book.submit(Order::market(id, side, qty)));
}

bool MatchingEngine::cancel(const std::string& symbol, OrderId id) {
  const auto it = books_.find(symbol);
  return it != books_.end() && it->second->cancel(id);
}

MatchingEngine::Report MatchingEngine::modify(const std::string& symbol, OrderId id,
                                              Price new_price, Quantity new_qty) {
  const auto it = books_.find(symbol);
  if (it == books_.end()) {
    return finish(id, SubmitResult{OrderStatus::Rejected, RejectReason::UnknownOrder, 0, 0});
  }
  return finish(id, it->second->modify(id, new_price, new_qty));
}

const OrderBook* MatchingEngine::find_book(const std::string& symbol) const {
  const auto it = books_.find(symbol);
  return it == books_.end() ? nullptr : it->second.get();
}

std::vector<std::string> MatchingEngine::symbols() const {
  std::vector<std::string> out;
  out.reserve(books_.size());
  for (const auto& entry : books_) out.push_back(entry.first);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace ob
