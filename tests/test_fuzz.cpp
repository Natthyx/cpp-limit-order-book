// Differential ("model-based") testing: drive the real OrderBook and a
// deliberately naive reference implementation with the same random stream of
// operations and require identical results, trades, and book contents.
//
// The reference is O(n) per operation and written for obviousness, not speed:
// it keeps a flat vector of orders and, for every fill, linearly scans for
// the best price and then the earliest arrival. If the two ever disagree, one
// of them has a bug, and the failing seed/operation index is printed so the
// run is exactly reproducible.

#include <algorithm>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "minitest.hpp"
#include "ob/order_book.hpp"

using namespace ob;

namespace {

class ReferenceBook {
 public:
  struct Resting {
    OrderId id;
    Side side;
    Price price;
    Quantity remaining;
    std::uint64_t arrival;
  };

  explicit ReferenceBook(std::vector<Trade>* trades) : trades_(trades) {}

  SubmitResult submit(const Order& o) {
    if (o.quantity == 0) return rejected(RejectReason::ZeroQuantity);
    const bool market = o.type == OrderType::Market;
    if (!market && o.price <= 0) return rejected(RejectReason::InvalidPrice);
    if (find(o.id) != orders_.end()) return rejected(RejectReason::DuplicateOrderId);

    const TimeInForce tif = market ? TimeInForce::IOC : o.tif;

    if (tif == TimeInForce::FOK) {
      Quantity available = 0;
      for (const auto& r : orders_) {
        if (r.side != o.side && crosses(o, r)) available += r.remaining;
      }
      if (available < o.quantity) {
        return SubmitResult{OrderStatus::Cancelled, RejectReason::None, 0, o.quantity};
      }
    }

    Quantity remaining = o.quantity;
    while (remaining > 0) {
      auto best = orders_.end();
      for (auto it = orders_.begin(); it != orders_.end(); ++it) {
        if (it->side == o.side || !crosses(o, *it)) continue;
        if (best == orders_.end() || better(*it, *best, o.side)) best = it;
      }
      if (best == orders_.end()) break;
      const Quantity qty = std::min(remaining, best->remaining);
      trades_->push_back(Trade{++trade_seq_, o.id, best->id, o.side, best->price, qty});
      best->remaining -= qty;
      remaining -= qty;
      if (best->remaining == 0) orders_.erase(best);
    }

    const Quantity filled = o.quantity - remaining;
    if (remaining == 0) return SubmitResult{OrderStatus::Filled, RejectReason::None, filled, 0};
    if (tif == TimeInForce::GTC) {
      orders_.push_back(Resting{o.id, o.side, o.price, remaining, ++arrival_});
      return SubmitResult{OrderStatus::Resting, RejectReason::None, filled, remaining};
    }
    return SubmitResult{OrderStatus::Cancelled, RejectReason::None, filled, remaining};
  }

  bool cancel(OrderId id) {
    const auto it = find(id);
    if (it == orders_.end()) return false;
    orders_.erase(it);
    return true;
  }

  SubmitResult modify(OrderId id, Price price, Quantity qty) {
    const auto it = find(id);
    if (it == orders_.end()) return rejected(RejectReason::UnknownOrder);
    if (qty == 0) return rejected(RejectReason::ZeroQuantity);
    if (price <= 0) return rejected(RejectReason::InvalidPrice);
    if (price == it->price && qty <= it->remaining) {
      it->remaining = qty;
      return SubmitResult{OrderStatus::Resting, RejectReason::None, 0, qty};
    }
    const Side side = it->side;
    orders_.erase(it);
    return submit(Order::limit(id, side, price, qty));
  }

  // price -> (total quantity, number of orders), best price first.
  std::vector<Level> depth(Side side) const {
    std::map<Price, Level> levels;
    for (const auto& r : orders_) {
      if (r.side != side) continue;
      Level& l = levels[r.price];
      l.price = r.price;
      l.quantity += r.remaining;
      ++l.orders;
    }
    std::vector<Level> out;
    for (const auto& kv : levels) out.push_back(kv.second);
    if (side == Side::Buy) std::reverse(out.begin(), out.end());
    return out;
  }

  std::size_t size() const { return orders_.size(); }

 private:
  static SubmitResult rejected(RejectReason r) {
    return SubmitResult{OrderStatus::Rejected, r, 0, 0};
  }
  static bool crosses(const Order& taker, const Resting& maker) {
    if (taker.type == OrderType::Market) return true;
    return taker.side == Side::Buy ? maker.price <= taker.price : maker.price >= taker.price;
  }
  // Is `a` a better match than `b` for a taker on `taker_side`?
  static bool better(const Resting& a, const Resting& b, Side taker_side) {
    if (a.price != b.price) {
      return taker_side == Side::Buy ? a.price < b.price : a.price > b.price;
    }
    return a.arrival < b.arrival;
  }
  std::vector<Resting>::iterator find(OrderId id) {
    return std::find_if(orders_.begin(), orders_.end(),
                        [id](const Resting& r) { return r.id == id; });
  }

  std::vector<Resting> orders_;
  std::vector<Trade>* trades_;
  std::uint64_t arrival_{0};
  std::uint64_t trade_seq_{0};
};

bool same(const SubmitResult& a, const SubmitResult& b) {
  return a.status == b.status && a.reason == b.reason && a.filled == b.filled &&
         a.remaining == b.remaining;
}

std::string describe(const SubmitResult& r) {
  std::ostringstream os;
  os << to_string(r.status) << "/" << to_string(r.reason) << " filled=" << r.filled
     << " remaining=" << r.remaining;
  return os.str();
}

bool same_levels(const std::vector<Level>& a, const std::vector<Level>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].price != b[i].price || a[i].quantity != b[i].quantity ||
        a[i].orders != b[i].orders) {
      return false;
    }
  }
  return true;
}

// Returns an empty string on success, otherwise a description of the first
// divergence between the real book and the reference.
std::string run_one_seed(std::uint64_t seed, int operations, Price lo, Price hi) {
  std::mt19937_64 rng(seed);
  const auto uniform = [&rng](std::uint64_t a, std::uint64_t b) {
    return std::uniform_int_distribution<std::uint64_t>(a, b)(rng);
  };

  std::vector<Trade> real_trades;
  std::vector<Trade> ref_trades;
  OrderBook book("FUZZ", [&](const Trade& t) { real_trades.push_back(t); });
  ReferenceBook ref(&ref_trades);

  OrderId next_id = 1;
  const auto price = [&]() {
    return static_cast<Price>(uniform(static_cast<std::uint64_t>(lo),
                                      static_cast<std::uint64_t>(hi)));
  };
  const auto side = [&]() { return uniform(0, 1) == 0 ? Side::Buy : Side::Sell; };
  // Mostly fresh ids; occasionally recycle an old one to exercise duplicates.
  const auto order_id = [&]() {
    if (next_id > 1 && uniform(0, 99) < 4) return static_cast<OrderId>(uniform(1, next_id - 1));
    return next_id++;
  };
  const auto existing_id = [&]() {
    return static_cast<OrderId>(uniform(1, next_id + 2));  // sometimes unknown
  };

  for (int op = 0; op < operations; ++op) {
    std::ostringstream what;
    SubmitResult got;
    SubmitResult want;
    const std::uint64_t kind = uniform(0, 99);

    if (kind < 50) {  // limit order
      const std::uint64_t t = uniform(0, 99);
      const TimeInForce tif = t < 70   ? TimeInForce::GTC
                              : t < 85 ? TimeInForce::IOC
                                       : TimeInForce::FOK;
      Quantity qty = uniform(1, 25);
      if (uniform(0, 99) < 2) qty = 0;
      Price p = price();
      if (uniform(0, 99) < 2) p = -static_cast<Price>(uniform(0, 3));
      const Order o = Order::limit(order_id(), side(), p, qty, tif);
      what << "limit " << to_string(o.side) << " id=" << o.id << " px=" << o.price
           << " qty=" << o.quantity << " " << to_string(o.tif);
      got = book.submit(o);
      want = ref.submit(o);
    } else if (kind < 60) {  // market order
      const Order o = Order::market(order_id(), side(), uniform(0, 40));
      what << "market " << to_string(o.side) << " id=" << o.id << " qty=" << o.quantity;
      got = book.submit(o);
      want = ref.submit(o);
    } else if (kind < 80) {  // cancel
      const OrderId id = existing_id();
      what << "cancel id=" << id;
      const bool a = book.cancel(id);
      const bool b = ref.cancel(id);
      if (a != b) return "seed " + std::to_string(seed) + " op " + std::to_string(op) +
                         " (" + what.str() + "): cancel result differs";
      got = want = SubmitResult{};
    } else {  // modify
      const OrderId id = existing_id();
      Price p = price();
      if (uniform(0, 99) < 3) p = 0;
      const Quantity qty = uniform(0, 30);
      what << "modify id=" << id << " px=" << p << " qty=" << qty;
      got = book.modify(id, p, qty);
      want = ref.modify(id, p, qty);
    }

    const std::string where =
        "seed " + std::to_string(seed) + " op " + std::to_string(op) + " (" + what.str() + "): ";

    if (!same(got, want)) {
      return where + "result differs: book=" + describe(got) + " reference=" + describe(want);
    }
    if (real_trades.size() != ref_trades.size()) {
      return where + "trade count differs: " + std::to_string(real_trades.size()) + " vs " +
             std::to_string(ref_trades.size());
    }
    for (std::size_t i = 0; i < real_trades.size(); ++i) {
      if (real_trades[i] != ref_trades[i]) {
        return where + "trade #" + std::to_string(i) + " differs";
      }
    }
    std::string why;
    if (!book.check_invariants(&why)) return where + "invariant broken: " + why;
    if (book.order_count() != ref.size()) return where + "resting order count differs";

    if (op % 25 == 0) {
      if (!same_levels(book.depth(Side::Buy, 1000), ref.depth(Side::Buy)) ||
          !same_levels(book.depth(Side::Sell, 1000), ref.depth(Side::Sell))) {
        return where + "book depth differs";
      }
    }
  }
  if (!same_levels(book.depth(Side::Buy, 1000), ref.depth(Side::Buy)) ||
      !same_levels(book.depth(Side::Sell, 1000), ref.depth(Side::Sell))) {
    return "seed " + std::to_string(seed) + ": final book depth differs";
  }
  return {};
}

}  // namespace

TEST(fuzz_matches_reference_model_tight_price_band) {
  // A narrow band forces constant crossing, partial fills and sweeps.
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const std::string err = run_one_seed(seed, 4000, 95, 105);
    CHECK_MSG(err.empty(), err);
    if (!err.empty()) return;
  }
}

TEST(fuzz_matches_reference_model_wide_price_band) {
  // A wide band builds deep books with many levels and fewer crosses.
  for (std::uint64_t seed = 1000; seed < 1020; ++seed) {
    const std::string err = run_one_seed(seed, 6000, 50, 150);
    CHECK_MSG(err.empty(), err);
    if (!err.empty()) return;
  }
}
