#include "minitest.hpp"
#include "ob/matching_engine.hpp"

using namespace ob;

TEST(engine_keeps_symbols_isolated) {
  MatchingEngine e;
  e.submit_limit("AAPL", Side::Sell, 100, 10);
  const auto r = e.submit_limit("MSFT", Side::Buy, 100, 10);  // different book
  CHECK_EQ(r.result.status, OrderStatus::Resting);
  CHECK(r.trades.empty());
  CHECK_EQ(e.find_book("AAPL")->best_ask(), 100);
  CHECK_EQ(e.find_book("MSFT")->best_bid(), 100);
  CHECK(e.find_book("GOOG") == nullptr);
}

TEST(engine_assigns_unique_increasing_ids_across_symbols) {
  MatchingEngine e;
  const auto a = e.submit_limit("AAPL", Side::Buy, 100, 1);
  const auto b = e.submit_limit("MSFT", Side::Buy, 100, 1);
  const auto c = e.submit_market("AAPL", Side::Sell, 1);
  CHECK_EQ(a.id, 1);
  CHECK_EQ(b.id, 2);
  CHECK_EQ(c.id, 3);
}

TEST(engine_reports_only_the_trades_of_the_current_call) {
  MatchingEngine e;
  const auto m1 = e.submit_limit("X", Side::Sell, 100, 5);
  const auto m2 = e.submit_limit("X", Side::Sell, 101, 5);
  const auto t1 = e.submit_limit("X", Side::Buy, 100, 5);
  CHECK_EQ(t1.trades.size(), 1);
  CHECK_EQ(t1.trades[0].maker_id, m1.id);
  const auto t2 = e.submit_limit("X", Side::Buy, 101, 5);
  CHECK_EQ(t2.trades.size(), 1);  // not 2: earlier trades are not repeated
  CHECK_EQ(t2.trades[0].maker_id, m2.id);
}

TEST(engine_supports_ioc_fok_and_market) {
  MatchingEngine e;
  e.submit_limit("X", Side::Sell, 100, 5);
  CHECK_EQ(e.submit_limit("X", Side::Buy, 100, 9, TimeInForce::FOK).result.status,
           OrderStatus::Cancelled);
  const auto ioc = e.submit_limit("X", Side::Buy, 100, 9, TimeInForce::IOC);
  CHECK_EQ(ioc.result.filled, 5);
  CHECK_EQ(e.submit_market("X", Side::Buy, 1).result.status, OrderStatus::Cancelled);
}

TEST(engine_cancel_requires_the_right_symbol) {
  MatchingEngine e;
  const auto r = e.submit_limit("AAPL", Side::Buy, 100, 5);
  CHECK(!e.cancel("MSFT", r.id));
  CHECK(!e.cancel("NOPE", r.id));
  CHECK(e.cancel("AAPL", r.id));
  CHECK(!e.cancel("AAPL", r.id));
}

TEST(engine_modify_works_and_rejects_unknown_symbol) {
  MatchingEngine e;
  const auto resting = e.submit_limit("AAPL", Side::Sell, 101, 5);
  const auto buy = e.submit_limit("AAPL", Side::Buy, 100, 5);
  const auto r = e.modify("AAPL", buy.id, 101, 5);  // now crosses
  CHECK_EQ(r.result.status, OrderStatus::Filled);
  CHECK_EQ(r.trades.size(), 1);
  CHECK_EQ(r.trades[0].maker_id, resting.id);

  const auto bad = e.modify("NOPE", 1, 100, 5);
  CHECK_EQ(bad.result.status, OrderStatus::Rejected);
  CHECK(bad.result.reason == RejectReason::UnknownOrder);
}

TEST(engine_lists_symbols_sorted) {
  MatchingEngine e;
  e.submit_limit("MSFT", Side::Buy, 1, 1);
  e.submit_limit("AAPL", Side::Buy, 1, 1);
  const auto s = e.symbols();
  CHECK_EQ(s.size(), 2);
  CHECK(s[0] == "AAPL");
  CHECK(s[1] == "MSFT");
}
