#include <string>
#include <vector>

#include "minitest.hpp"
#include "ob/order_book.hpp"

using namespace ob;

namespace {

// Wraps a book, records every trade it emits, and re-validates the book's
// internal invariants on demand.
struct Fixture {
  OrderBook book{"TEST"};
  std::vector<Trade> trades;

  Fixture() {
    book.set_trade_callback([this](const Trade& t) { trades.push_back(t); });
  }

  SubmitResult buy(OrderId id, Price price, Quantity qty,
                   TimeInForce tif = TimeInForce::GTC) {
    return book.submit(Order::limit(id, Side::Buy, price, qty, tif));
  }
  SubmitResult sell(OrderId id, Price price, Quantity qty,
                    TimeInForce tif = TimeInForce::GTC) {
    return book.submit(Order::limit(id, Side::Sell, price, qty, tif));
  }
  void check() {
    std::string why;
    const bool ok = book.check_invariants(&why);
    CHECK_MSG(ok, why);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Resting and basic matching
// ---------------------------------------------------------------------------

TEST(empty_book_has_no_prices) {
  Fixture f;
  CHECK_EQ(f.book.best_bid(), std::nullopt);
  CHECK_EQ(f.book.best_ask(), std::nullopt);
  CHECK_EQ(f.book.order_count(), 0);
  CHECK_EQ(f.book.level_count(Side::Buy), 0);
  CHECK_EQ(f.book.level_count(Side::Sell), 0);
  CHECK(f.book.depth(Side::Buy, 5).empty());
  f.check();
}

TEST(limit_order_rests_when_nothing_to_match) {
  Fixture f;
  const auto r = f.buy(1, 100, 10);
  CHECK_EQ(r.status, OrderStatus::Resting);
  CHECK_EQ(r.filled, 0);
  CHECK_EQ(r.remaining, 10);
  CHECK_EQ(f.book.best_bid(), 100);
  CHECK_EQ(f.book.best_ask(), std::nullopt);
  CHECK_EQ(f.book.volume_at(Side::Buy, 100), 10);
  CHECK_EQ(f.book.order_count(), 1);
  CHECK(f.trades.empty());
  f.check();
}

TEST(non_crossing_orders_coexist) {
  Fixture f;
  f.buy(1, 99, 10);
  f.sell(2, 101, 10);
  CHECK(f.trades.empty());
  CHECK_EQ(f.book.best_bid(), 99);
  CHECK_EQ(f.book.best_ask(), 101);
  f.check();
}

TEST(buyer_and_seller_at_same_price_trade) {
  Fixture f;
  f.sell(1, 100, 10);
  const auto r = f.buy(2, 100, 10);
  CHECK_EQ(r.status, OrderStatus::Filled);
  CHECK_EQ(r.filled, 10);
  CHECK_EQ(r.remaining, 0);
  CHECK_EQ(f.trades.size(), 1);
  CHECK(f.trades[0] == (Trade{1, 2, 1, Side::Buy, 100, 10}));
  CHECK_EQ(f.book.order_count(), 0);
  CHECK_EQ(f.book.level_count(Side::Sell), 0);
  f.check();
}

TEST(trade_happens_at_the_resting_orders_price) {
  Fixture f;
  f.sell(1, 100, 5);
  f.buy(2, 105, 5);  // willing to pay 105, but gets 100
  CHECK_EQ(f.trades.size(), 1);
  CHECK_EQ(f.trades[0].price, 100);

  f.trades.clear();
  f.buy(3, 100, 5);
  f.sell(4, 95, 5);  // willing to accept 95, but gets 100
  CHECK_EQ(f.trades.size(), 1);
  CHECK_EQ(f.trades[0].price, 100);
  CHECK(f.trades[0].taker_side == Side::Sell);
  f.check();
}

TEST(incoming_order_remainder_rests_after_partial_fill) {
  Fixture f;
  f.sell(1, 100, 4);
  const auto r = f.buy(2, 100, 10);
  CHECK_EQ(r.status, OrderStatus::Resting);
  CHECK_EQ(r.filled, 4);
  CHECK_EQ(r.remaining, 6);
  CHECK_EQ(f.book.best_bid(), 100);
  CHECK_EQ(f.book.volume_at(Side::Buy, 100), 6);
  CHECK_EQ(f.book.best_ask(), std::nullopt);
  f.check();
}

TEST(resting_order_remainder_stays_after_partial_fill) {
  Fixture f;
  f.sell(1, 100, 10);
  const auto r = f.buy(2, 100, 4);
  CHECK_EQ(r.status, OrderStatus::Filled);
  CHECK_EQ(f.book.volume_at(Side::Sell, 100), 6);
  const auto resting = f.book.find(1);
  CHECK(resting.has_value());
  CHECK_EQ(resting->quantity, 6);
  f.check();
}

TEST(two_buyers_do_not_trade_with_each_other) {
  Fixture f;
  f.buy(1, 100, 5);
  f.buy(2, 105, 5);
  CHECK(f.trades.empty());
  CHECK_EQ(f.book.best_bid(), 105);
  f.check();
}

// ---------------------------------------------------------------------------
// Priority rules
// ---------------------------------------------------------------------------

TEST(price_priority_buyer_hits_cheapest_ask_first) {
  Fixture f;
  f.sell(1, 102, 5);
  f.sell(2, 101, 5);
  f.buy(3, 102, 5);
  CHECK_EQ(f.trades.size(), 1);
  CHECK_EQ(f.trades[0].maker_id, 2);
  CHECK_EQ(f.trades[0].price, 101);
  CHECK_EQ(f.book.best_ask(), 102);
  f.check();
}

TEST(price_priority_seller_hits_highest_bid_first) {
  Fixture f;
  f.buy(1, 98, 5);
  f.buy(2, 100, 5);
  f.buy(3, 99, 5);
  f.sell(4, 98, 12);
  CHECK_EQ(f.trades.size(), 3);
  CHECK_EQ(f.trades[0].maker_id, 2);
  CHECK_EQ(f.trades[0].quantity, 5);
  CHECK_EQ(f.trades[1].maker_id, 3);
  CHECK_EQ(f.trades[1].quantity, 5);
  CHECK_EQ(f.trades[2].maker_id, 1);
  CHECK_EQ(f.trades[2].quantity, 2);
  CHECK_EQ(f.book.volume_at(Side::Buy, 98), 3);
  f.check();
}

TEST(time_priority_is_fifo_within_a_price_level) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 100, 5);
  f.sell(3, 100, 5);
  f.buy(4, 100, 12);
  CHECK_EQ(f.trades.size(), 3);
  CHECK_EQ(f.trades[0].maker_id, 1);
  CHECK_EQ(f.trades[1].maker_id, 2);
  CHECK_EQ(f.trades[2].maker_id, 3);
  CHECK_EQ(f.trades[2].quantity, 2);
  CHECK_EQ(f.book.find(3)->quantity, 3);
  CHECK(!f.book.contains(1));
  f.check();
}

TEST(order_sweeps_multiple_price_levels) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 101, 5);
  f.sell(3, 102, 5);
  const auto r = f.buy(4, 101, 20);  // limit 101: must not touch 102
  CHECK_EQ(r.status, OrderStatus::Resting);
  CHECK_EQ(r.filled, 10);
  CHECK_EQ(r.remaining, 10);
  CHECK_EQ(f.trades.size(), 2);
  CHECK_EQ(f.trades[0].price, 100);
  CHECK_EQ(f.trades[1].price, 101);
  CHECK_EQ(f.book.best_bid(), 101);
  CHECK_EQ(f.book.best_ask(), 102);
  f.check();
}

TEST(trade_sequence_numbers_increase_by_one) {
  Fixture f;
  f.sell(1, 100, 1);
  f.sell(2, 100, 1);
  f.sell(3, 100, 1);
  f.buy(4, 100, 3);
  CHECK_EQ(f.trades.size(), 3);
  CHECK_EQ(f.trades[0].sequence, 1);
  CHECK_EQ(f.trades[1].sequence, 2);
  CHECK_EQ(f.trades[2].sequence, 3);
  CHECK_EQ(f.book.trade_count(), 3);
}

// ---------------------------------------------------------------------------
// Time in force
// ---------------------------------------------------------------------------

TEST(ioc_fills_what_it_can_and_drops_the_rest) {
  Fixture f;
  f.sell(1, 100, 4);
  const auto r = f.buy(2, 100, 10, TimeInForce::IOC);
  CHECK_EQ(r.status, OrderStatus::Cancelled);
  CHECK_EQ(r.filled, 4);
  CHECK_EQ(r.remaining, 6);
  CHECK_EQ(f.book.order_count(), 0);  // nothing rests
  CHECK_EQ(f.book.best_bid(), std::nullopt);
  f.check();
}

TEST(ioc_with_no_liquidity_is_cancelled) {
  Fixture f;
  const auto r = f.buy(1, 100, 10, TimeInForce::IOC);
  CHECK_EQ(r.status, OrderStatus::Cancelled);
  CHECK_EQ(r.filled, 0);
  CHECK_EQ(r.remaining, 10);
  CHECK_EQ(f.book.order_count(), 0);
}

TEST(ioc_respects_its_limit_price) {
  Fixture f;
  f.sell(1, 105, 5);
  const auto r = f.buy(2, 100, 5, TimeInForce::IOC);
  CHECK_EQ(r.status, OrderStatus::Cancelled);
  CHECK_EQ(r.filled, 0);
  CHECK(f.trades.empty());
  CHECK_EQ(f.book.volume_at(Side::Sell, 105), 5);
}

TEST(fok_fills_completely_across_levels_when_possible) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 101, 5);
  const auto r = f.buy(3, 101, 10, TimeInForce::FOK);
  CHECK_EQ(r.status, OrderStatus::Filled);
  CHECK_EQ(r.filled, 10);
  CHECK_EQ(f.trades.size(), 2);
  CHECK_EQ(f.book.order_count(), 0);
  f.check();
}

TEST(fok_with_insufficient_liquidity_changes_nothing) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 101, 4);
  const auto r = f.buy(3, 101, 10, TimeInForce::FOK);
  CHECK_EQ(r.status, OrderStatus::Cancelled);
  CHECK_EQ(r.filled, 0);
  CHECK_EQ(r.remaining, 10);
  CHECK(f.trades.empty());
  CHECK_EQ(f.book.trade_count(), 0);
  CHECK_EQ(f.book.volume_at(Side::Sell, 100), 5);
  CHECK_EQ(f.book.volume_at(Side::Sell, 101), 4);
  f.check();
}

TEST(fok_does_not_count_liquidity_beyond_its_limit) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 105, 100);
  const auto r = f.buy(3, 101, 10, TimeInForce::FOK);
  CHECK_EQ(r.status, OrderStatus::Cancelled);
  CHECK(f.trades.empty());
}

TEST(fok_sell_side_works_too) {
  Fixture f;
  f.buy(1, 100, 6);
  CHECK_EQ(f.sell(2, 100, 10, TimeInForce::FOK).status, OrderStatus::Cancelled);
  f.buy(3, 100, 4);
  CHECK_EQ(f.sell(4, 100, 10, TimeInForce::FOK).status, OrderStatus::Filled);
  f.check();
}

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

TEST(market_order_sweeps_the_book_at_any_price) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 200, 5);
  const auto r = f.book.submit(Order::market(3, Side::Buy, 7));
  CHECK_EQ(r.status, OrderStatus::Filled);
  CHECK_EQ(r.filled, 7);
  CHECK_EQ(f.trades.size(), 2);
  CHECK_EQ(f.trades[0].price, 100);
  CHECK_EQ(f.trades[1].price, 200);
  CHECK_EQ(f.trades[1].quantity, 2);
  CHECK_EQ(f.book.volume_at(Side::Sell, 200), 3);
  f.check();
}

TEST(market_order_never_rests) {
  Fixture f;
  f.sell(1, 100, 5);
  const auto r = f.book.submit(Order::market(2, Side::Buy, 10));
  CHECK_EQ(r.status, OrderStatus::Cancelled);
  CHECK_EQ(r.filled, 5);
  CHECK_EQ(r.remaining, 5);
  CHECK_EQ(f.book.order_count(), 0);
}

TEST(market_order_on_empty_book_is_cancelled) {
  Fixture f;
  const auto r = f.book.submit(Order::market(1, Side::Sell, 10));
  CHECK_EQ(r.status, OrderStatus::Cancelled);
  CHECK_EQ(r.filled, 0);
  CHECK_EQ(f.book.order_count(), 0);
}

TEST(market_order_is_ioc_even_if_marked_gtc) {
  Fixture f;
  Order o = Order::market(1, Side::Buy, 10);
  o.tif = TimeInForce::GTC;
  CHECK_EQ(f.book.submit(o).status, OrderStatus::Cancelled);
  CHECK_EQ(f.book.order_count(), 0);
}

TEST(market_sell_hits_best_bids_first) {
  Fixture f;
  f.buy(1, 90, 5);
  f.buy(2, 95, 5);
  f.book.submit(Order::market(3, Side::Sell, 6));
  CHECK_EQ(f.trades.size(), 2);
  CHECK_EQ(f.trades[0].maker_id, 2);
  CHECK_EQ(f.trades[1].maker_id, 1);
  CHECK_EQ(f.trades[1].quantity, 1);
  f.check();
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST(cancel_removes_order_and_empty_level) {
  Fixture f;
  f.buy(1, 100, 5);
  CHECK(f.book.cancel(1));
  CHECK_EQ(f.book.order_count(), 0);
  CHECK_EQ(f.book.level_count(Side::Buy), 0);
  CHECK_EQ(f.book.best_bid(), std::nullopt);
  CHECK(!f.book.cancel(1));  // second cancel fails
  f.check();
}

TEST(cancel_keeps_the_level_and_the_queue_order_of_others) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 100, 5);
  f.sell(3, 100, 5);
  CHECK(f.book.cancel(2));
  CHECK_EQ(f.book.volume_at(Side::Sell, 100), 10);
  f.buy(4, 100, 10);
  CHECK_EQ(f.trades.size(), 2);
  CHECK_EQ(f.trades[0].maker_id, 1);
  CHECK_EQ(f.trades[1].maker_id, 3);
  f.check();
}

TEST(cancel_of_unknown_or_filled_order_returns_false) {
  Fixture f;
  CHECK(!f.book.cancel(42));
  f.sell(1, 100, 5);
  f.buy(2, 100, 5);
  CHECK(!f.book.cancel(1));  // already filled
  CHECK(!f.book.cancel(2));  // never rested
}

// ---------------------------------------------------------------------------
// Modify
// ---------------------------------------------------------------------------

TEST(reducing_quantity_keeps_time_priority) {
  Fixture f;
  f.sell(1, 100, 10);
  f.sell(2, 100, 10);
  const auto r = f.book.modify(1, 100, 4);
  CHECK_EQ(r.status, OrderStatus::Resting);
  CHECK_EQ(r.remaining, 4);
  CHECK_EQ(f.book.volume_at(Side::Sell, 100), 14);
  f.buy(3, 100, 5);
  CHECK_EQ(f.trades.size(), 2);
  CHECK_EQ(f.trades[0].maker_id, 1);
  CHECK_EQ(f.trades[0].quantity, 4);
  CHECK_EQ(f.trades[1].maker_id, 2);
  CHECK_EQ(f.trades[1].quantity, 1);
  f.check();
}

TEST(modify_to_same_quantity_is_a_noop_that_keeps_priority) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 100, 5);
  CHECK_EQ(f.book.modify(1, 100, 5).status, OrderStatus::Resting);
  f.buy(3, 100, 1);
  CHECK_EQ(f.trades[0].maker_id, 1);
  f.check();
}

TEST(increasing_quantity_loses_time_priority) {
  Fixture f;
  f.sell(1, 100, 5);
  f.sell(2, 100, 5);
  f.book.modify(1, 100, 8);
  CHECK_EQ(f.book.volume_at(Side::Sell, 100), 13);
  f.buy(3, 100, 5);
  CHECK_EQ(f.trades.size(), 1);
  CHECK_EQ(f.trades[0].maker_id, 2);  // order 2 is now first in line
  f.check();
}

TEST(changing_price_moves_the_order_to_the_new_level) {
  Fixture f;
  f.buy(1, 100, 5);
  const auto r = f.book.modify(1, 101, 5);
  CHECK_EQ(r.status, OrderStatus::Resting);
  CHECK_EQ(f.book.volume_at(Side::Buy, 100), 0);
  CHECK_EQ(f.book.volume_at(Side::Buy, 101), 5);
  CHECK_EQ(f.book.level_count(Side::Buy), 1);
  CHECK_EQ(f.book.order_count(), 1);
  f.check();
}

TEST(modify_that_crosses_the_spread_executes) {
  Fixture f;
  f.buy(1, 99, 5);
  f.sell(2, 100, 5);
  const auto r = f.book.modify(1, 100, 5);
  CHECK_EQ(r.status, OrderStatus::Filled);
  CHECK_EQ(r.filled, 5);
  CHECK_EQ(f.trades.size(), 1);
  CHECK_EQ(f.trades[0].taker_id, 1);
  CHECK_EQ(f.trades[0].maker_id, 2);
  CHECK_EQ(f.trades[0].price, 100);
  CHECK_EQ(f.book.order_count(), 0);
  f.check();
}

TEST(modify_rejections_leave_the_original_order_untouched) {
  Fixture f;
  f.buy(1, 100, 5);

  auto r = f.book.modify(99, 100, 5);
  CHECK_EQ(r.status, OrderStatus::Rejected);
  CHECK(r.reason == RejectReason::UnknownOrder);

  r = f.book.modify(1, 100, 0);
  CHECK_EQ(r.status, OrderStatus::Rejected);
  CHECK(r.reason == RejectReason::ZeroQuantity);

  r = f.book.modify(1, 0, 5);
  CHECK_EQ(r.status, OrderStatus::Rejected);
  CHECK(r.reason == RejectReason::InvalidPrice);

  CHECK(f.book.contains(1));
  CHECK_EQ(f.book.find(1)->quantity, 5);
  CHECK_EQ(f.book.find(1)->price, 100);
  f.check();
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST(duplicate_active_order_id_is_rejected) {
  Fixture f;
  f.buy(1, 100, 5);
  const auto r = f.buy(1, 90, 5);
  CHECK_EQ(r.status, OrderStatus::Rejected);
  CHECK(r.reason == RejectReason::DuplicateOrderId);
  CHECK_EQ(f.book.order_count(), 1);
  CHECK_EQ(f.book.volume_at(Side::Buy, 90), 0);
  f.check();
}

TEST(zero_quantity_is_rejected) {
  Fixture f;
  const auto r = f.buy(1, 100, 0);
  CHECK_EQ(r.status, OrderStatus::Rejected);
  CHECK(r.reason == RejectReason::ZeroQuantity);
  CHECK_EQ(f.book.order_count(), 0);
}

TEST(non_positive_limit_price_is_rejected) {
  Fixture f;
  CHECK(f.buy(1, 0, 5).reason == RejectReason::InvalidPrice);
  CHECK(f.sell(2, -5, 5).reason == RejectReason::InvalidPrice);
  CHECK_EQ(f.book.order_count(), 0);
}

TEST(order_id_can_be_reused_once_the_order_is_gone) {
  Fixture f;
  f.sell(1, 100, 5);
  f.buy(2, 100, 5);                                   // order 1 fully filled
  CHECK_EQ(f.sell(1, 100, 5).status, OrderStatus::Resting);  // id 1 reusable
  CHECK(f.book.cancel(1));
  CHECK_EQ(f.buy(1, 90, 5).status, OrderStatus::Resting);    // and again
  f.check();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TEST(depth_lists_best_levels_first_with_aggregates) {
  Fixture f;
  f.buy(1, 100, 5);
  f.buy(2, 102, 3);
  f.buy(3, 102, 4);
  f.buy(4, 101, 1);
  f.sell(5, 110, 2);
  f.sell(6, 108, 6);

  const auto bids = f.book.depth(Side::Buy, 2);
  CHECK_EQ(bids.size(), 2);
  CHECK_EQ(bids[0].price, 102);
  CHECK_EQ(bids[0].quantity, 7);
  CHECK_EQ(bids[0].orders, 2);
  CHECK_EQ(bids[1].price, 101);

  CHECK_EQ(f.book.depth(Side::Buy, 10).size(), 3);

  const auto asks = f.book.depth(Side::Sell, 10);
  CHECK_EQ(asks.size(), 2);
  CHECK_EQ(asks[0].price, 108);  // lowest ask first
  CHECK_EQ(asks[1].price, 110);
  CHECK(f.book.depth(Side::Sell, 0).empty());
}

TEST(find_reports_remaining_quantity_and_side) {
  Fixture f;
  f.sell(1, 100, 10);
  f.buy(2, 100, 3);
  const auto o = f.book.find(1);
  CHECK(o.has_value());
  CHECK(o->side == Side::Sell);
  CHECK_EQ(o->price, 100);
  CHECK_EQ(o->quantity, 7);
  CHECK(!f.book.find(2).has_value());
}

TEST(very_large_quantities_do_not_overflow_matching) {
  Fixture f;
  const Quantity big = Quantity{1} << 60;
  f.sell(1, 100, big);
  const auto r = f.buy(2, 100, big - 1);
  CHECK_EQ(r.status, OrderStatus::Filled);
  CHECK_EQ(f.book.volume_at(Side::Sell, 100), 1);
  f.check();
}
