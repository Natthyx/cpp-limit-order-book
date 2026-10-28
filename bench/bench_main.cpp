// Throughput and latency benchmark for the order book.
//
//   ./ob_bench [--ops N] [--seed S] [--runs R]
//
// The operation stream is generated up front (so RNG cost is not measured)
// and is deterministic for a given seed. Two passes are run:
//   1. Throughput: the whole stream with no per-operation timing, so the
//      clock calls do not distort the result.
//   2. Latency: every operation is individually timed with steady_clock; the
//      cost of the timer itself is measured and reported so you can judge it.
//
// The mix imitates a quiet market: orders cluster around a slowly drifting
// mid price, most are passive (they rest), some cross the spread, and a large
// share are cancelled shortly after being placed.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "ob/order_book.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

enum class Kind : std::uint8_t { Limit, Market, Cancel, Modify };

struct Op {
  Kind kind;
  Side side;
  TimeInForce tif;
  Price price;
  Quantity qty;
  OrderId id;
};

struct Options {
  std::size_t ops = 2'000'000;
  std::uint64_t seed = 12345;
  int runs = 5;
};

std::vector<Op> generate(const Options& opt) {
  std::mt19937_64 rng(opt.seed);
  const auto uniform = [&rng](std::uint64_t a, std::uint64_t b) {
    return std::uniform_int_distribution<std::uint64_t>(a, b)(rng);
  };
  std::geometric_distribution<int> offset(0.12);  // mean ~7 ticks from the touch

  std::vector<Op> ops;
  ops.reserve(opt.ops);
  Price mid = 100'000;
  OrderId next_id = 1;
  constexpr std::uint64_t kRecentWindow = 20'000;

  for (std::size_t i = 0; i < opt.ops; ++i) {
    if (i % 200 == 0) mid += static_cast<Price>(uniform(0, 2)) - 1;  // slow drift
    const Side side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
    const Price away = 1 + offset(rng);
    const std::uint64_t roll = uniform(0, 99);

    Op op{};
    op.side = side;
    op.tif = TimeInForce::GTC;
    op.qty = uniform(1, 100);

    if (roll < 50) {  // passive limit order: rests on its own side
      op.kind = Kind::Limit;
      op.id = next_id++;
      op.price = side == Side::Buy ? mid - away : mid + away;
    } else if (roll < 60) {  // aggressive limit: crosses into the other side
      op.kind = Kind::Limit;
      op.id = next_id++;
      op.price = side == Side::Buy ? mid + away : mid - away;
    } else if (roll < 63) {  // market order
      op.kind = Kind::Market;
      op.id = next_id++;
      op.price = 0;
    } else if (roll < 65) {  // IOC limit
      op.kind = Kind::Limit;
      op.tif = TimeInForce::IOC;
      op.id = next_id++;
      op.price = side == Side::Buy ? mid + away : mid - away;
    } else if (roll < 95) {  // cancel a recently placed order
      op.kind = Kind::Cancel;
      const std::uint64_t hi = next_id > 1 ? next_id - 1 : 1;
      const std::uint64_t lo = hi > kRecentWindow ? hi - kRecentWindow : 1;
      op.id = uniform(lo, hi);
    } else {  // modify a recently placed order
      op.kind = Kind::Modify;
      const std::uint64_t hi = next_id > 1 ? next_id - 1 : 1;
      const std::uint64_t lo = hi > kRecentWindow ? hi - kRecentWindow : 1;
      op.id = uniform(lo, hi);
      op.price = mid + static_cast<Price>(uniform(0, 20)) - 10;
    }
    ops.push_back(op);
  }
  return ops;
}

struct Counters {
  std::uint64_t trades = 0;
  std::uint64_t traded_qty = 0;
  std::uint64_t cancels_hit = 0;
  std::uint64_t cancels = 0;
  std::uint64_t checksum = 0;  // keeps the optimizer honest
};

// Applies one operation; kept inline so both passes execute identical code.
inline void apply(OrderBook& book, const Op& op, Counters& c) {
  switch (op.kind) {
    case Kind::Limit: {
      const auto r = book.submit(Order::limit(op.id, op.side, op.price, op.qty, op.tif));
      c.checksum += r.filled + static_cast<std::uint64_t>(r.status);
      break;
    }
    case Kind::Market: {
      const auto r = book.submit(Order::market(op.id, op.side, op.qty));
      c.checksum += r.filled;
      break;
    }
    case Kind::Cancel: {
      ++c.cancels;
      if (book.cancel(op.id)) ++c.cancels_hit;
      break;
    }
    case Kind::Modify: {
      const auto r = book.modify(op.id, op.price, op.qty);
      c.checksum += r.filled + static_cast<std::uint64_t>(r.status);
      break;
    }
  }
}

std::unique_ptr<OrderBook> make_book(Counters& c) {
  auto book = std::make_unique<OrderBook>("BENCH", [&c](const Trade& t) {
    ++c.trades;
    c.traded_qty += t.quantity;
  });
  book->reserve_orders(1 << 18);
  return book;
}

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

std::uint64_t percentile(const std::vector<std::uint32_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const std::size_t idx =
      std::min(sorted.size() - 1, static_cast<std::size_t>(p / 100.0 * static_cast<double>(sorted.size())));
  return sorted[idx];
}

Options parse(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (std::strcmp(argv[i], "--ops") == 0) {
      opt.ops = static_cast<std::size_t>(std::strtoull(next(), nullptr, 10));
    } else if (std::strcmp(argv[i], "--seed") == 0) {
      opt.seed = std::strtoull(next(), nullptr, 10);
    } else if (std::strcmp(argv[i], "--runs") == 0) {
      opt.runs = std::max(1, std::atoi(next()));
    } else {
      std::fprintf(stderr, "usage: %s [--ops N] [--seed S] [--runs R]\n", argv[0]);
      std::exit(2);
    }
  }
  return opt;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  std::printf("Generating %zu operations (seed %llu)...\n", opt.ops,
              static_cast<unsigned long long>(opt.seed));
  const std::vector<Op> ops = generate(opt);

  // ---- Pass 1: throughput (best and median of several runs) --------------
  std::vector<double> rates;
  Counters last;
  std::size_t resting_at_end = 0;
  std::size_t bid_levels = 0;
  std::size_t ask_levels = 0;
  for (int run = 0; run < opt.runs; ++run) {
    Counters c;
    auto book = make_book(c);
    const auto start = Clock::now();
    for (const Op& op : ops) apply(*book, op, c);
    const double elapsed = seconds(Clock::now() - start);
    rates.push_back(static_cast<double>(ops.size()) / elapsed);
    last = c;
    resting_at_end = book->order_count();
    bid_levels = book->level_count(Side::Buy);
    ask_levels = book->level_count(Side::Sell);
  }
  std::sort(rates.begin(), rates.end());
  const double median_rate = rates[rates.size() / 2];

  // ---- Pass 2: per-operation latency -------------------------------------
  std::vector<std::uint32_t> lat;
  lat.reserve(ops.size());
  {
    Counters c;
    auto book = make_book(c);
    for (const Op& op : ops) {
      const auto t0 = Clock::now();
      apply(*book, op, c);
      const auto t1 = Clock::now();
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      lat.push_back(static_cast<std::uint32_t>(std::min<std::int64_t>(ns, UINT32_MAX)));
    }
  }
  std::sort(lat.begin(), lat.end());

  // Cost of the timer itself (two back-to-back clock reads).
  std::vector<std::uint32_t> overhead;
  for (int i = 0; i < 100000; ++i) {
    const auto t0 = Clock::now();
    const auto t1 = Clock::now();
    overhead.push_back(static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  }
  std::sort(overhead.begin(), overhead.end());

  std::printf("\n=== Order book benchmark ===\n");
  std::printf("operations            : %zu\n", ops.size());
  std::printf("trades executed       : %llu (%llu units)\n",
              static_cast<unsigned long long>(last.trades),
              static_cast<unsigned long long>(last.traded_qty));
  std::printf("cancel hit rate       : %.1f%% (%llu of %llu)\n",
              last.cancels ? 100.0 * static_cast<double>(last.cancels_hit) /
                                 static_cast<double>(last.cancels)
                           : 0.0,
              static_cast<unsigned long long>(last.cancels_hit),
              static_cast<unsigned long long>(last.cancels));
  std::printf("resting orders at end : %zu (%zu bid levels, %zu ask levels)\n", resting_at_end,
              bid_levels, ask_levels);
  std::printf("\nThroughput (%d runs, no per-op timing)\n", opt.runs);
  std::printf("  best                : %.2f M ops/s\n", rates.back() / 1e6);
  std::printf("  median              : %.2f M ops/s\n", median_rate / 1e6);
  std::printf("  worst               : %.2f M ops/s\n", rates.front() / 1e6);
  std::printf("\nLatency per operation (ns, includes ~%llu ns timer overhead)\n",
              static_cast<unsigned long long>(percentile(overhead, 50)));
  std::printf("  p50                 : %llu\n", static_cast<unsigned long long>(percentile(lat, 50)));
  std::printf("  p90                 : %llu\n", static_cast<unsigned long long>(percentile(lat, 90)));
  std::printf("  p99                 : %llu\n", static_cast<unsigned long long>(percentile(lat, 99)));
  std::printf("  p99.9               : %llu\n", static_cast<unsigned long long>(percentile(lat, 99.9)));
  std::printf("  max                 : %llu\n", static_cast<unsigned long long>(lat.back()));
  std::printf("\n(checksum %llu)\n", static_cast<unsigned long long>(last.checksum));
  return 0;
}
