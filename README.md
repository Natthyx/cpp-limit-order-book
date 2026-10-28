# orderbook

A price-time-priority **limit order book and matching engine** in modern C++17, with a
multi-symbol engine, a command-line front end, a randomized differential test suite, and a
reproducible benchmark. No third-party dependencies.

```
LIMIT SELL AAPL 10100 50          ACK id=1 status=RESTING filled=0 remaining=50
LIMIT BUY  AAPL 10500 20          ACK id=2 status=FILLED  filled=20 remaining=0
                                  TRADE seq=1 symbol=AAPL taker=2 maker=1 side=BUY price=10100 qty=20
```

## Features

- Limit and market orders; time-in-force **GTC**, **IOC**, **FOK**
- **Price-time priority**: best price first, FIFO within a price level
- Trades execute at the **resting (maker) order's price**, so aggressive orders get price improvement
- **Cancel** in O(1) average, and **modify** with exchange-style semantics (reducing quantity at the
  same price keeps queue position; any other change is cancel/replace and loses it)
- Input validation with explicit reject reasons (duplicate id, zero quantity, bad price, unknown order)
- Multi-symbol `MatchingEngine` with globally unique order ids
- Integer prices (ticks) everywhere: no floating-point in matching logic

## Build and run

Requires a C++17 compiler (tested with GCC 13 on Linux and Apple clang 17 on macOS; MSVC is untested). CMake 3.16+ is recommended; a plain
`Makefile` is provided as a fallback.

```bash
# CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/ob_bench
./build/ob_cli examples/demo.txt

# Or, without CMake
make test      # unit + randomized tests
make asan      # same tests under AddressSanitizer + UBSan
make bench     # benchmark
make cli       # run the demo script
```

Sanitizer build with CMake: `cmake -S . -B build-asan -DOB_SANITIZE=ON && cmake --build build-asan -j`.

### CLI

```
LIMIT  <BUY|SELL> <symbol> <price> <qty> [GTC|IOC|FOK]
MARKET <BUY|SELL> <symbol> <qty>
CANCEL <symbol> <order_id>
MODIFY <symbol> <order_id> <new_price> <new_qty>
BOOK   <symbol> [depth]
SYMBOLS | HELP | QUIT
```

Run it interactively (`./build/ob_cli`) or feed it a script (`./build/ob_cli examples/demo.txt`).

### Library

```cpp
#include "ob/order_book.hpp"
using namespace ob;

OrderBook book("AAPL", [](const Trade& t) { /* called once per fill */ });
book.submit(Order::limit(1, Side::Sell, 10100, 50));
auto r = book.submit(Order::limit(2, Side::Buy, 10100, 20));   // r.status == Filled
book.cancel(1);
```

## Design

| Concern | Choice | Why |
|---|---|---|
| Each side of the book | `std::map<Price, PriceLevel>` (bids `greater<>`, asks `less<>`) | Best price is `begin()` in O(1); inserting a new level is O(log L). Ordered iteration makes sweeping several levels natural. |
| Orders within a price | `std::list<RestingOrder>` in arrival order | FIFO = time priority. Append and erase-from-middle are O(1) and never invalidate other orders' iterators. |
| Cancel / modify lookup | `unordered_map<OrderId, Locator>` holding side, price, level pointer, list iterator | Cancel is O(1) average instead of scanning the book. |
| Level aggregate | `PriceLevel::total` maintained incrementally | O(1) volume queries; makes the FOK liquidity check cheap. |
| Prices / quantities | `int64_t` ticks / `uint64_t` | No floating-point comparison bugs. |
| Output | Trade callback | The caller decides whether to collect, log, or ignore trades, with no allocation in the book. |

Complexity, with n resting orders, L price levels, and k levels consumed by an incoming order:

| Operation | Cost |
|---|---|
| submit (match) | O(k log L) for erasing emptied levels, plus O(1) per fill |
| submit (rest) | O(log L) |
| cancel | O(1) average |
| modify in place | O(1) average |
| modify cancel/replace | cancel + submit |
| best bid / ask | O(1) |

### Matching rules

1. An incoming order matches the opposite side while the best level's price satisfies its limit
   (buy: `ask <= limit`, sell: `bid >= limit`). Market orders have no limit.
2. Within a level, resting orders fill oldest first.
3. The trade price is the resting order's price.
4. After matching, a **GTC** limit remainder rests; an **IOC** or market remainder is dropped.
5. **FOK** first computes the executable quantity without changing anything. If it is less than the
   order size, the order is cancelled and the book is untouched (no partial trades).
6. An order id must be unique among *live* orders; ids may be reused after an order is filled or cancelled.

## Testing

`make test` / `ctest` runs 50 tests with a tiny built-in harness (no GoogleTest, so it works offline):

- **Unit tests**: price priority, FIFO priority, price improvement, partial fills, sweeps across
  levels, IOC/FOK/market behavior, cancel, modify semantics, validation, queries.
- **Differential fuzz tests**: random streams of limit/market/IOC/FOK/cancel/modify operations
  (including invalid inputs and duplicate ids) are applied to the real book *and* to a deliberately
  naive O(n) reference implementation. Every result, every trade, and the full book depth must
  match. Failures print the seed and operation index so they can be reproduced exactly.
- **Invariant checker**: `OrderBook::check_invariants()` verifies after every fuzz operation that
  the id index, price levels, and level totals agree, that no empty level or zero-quantity order
  exists, and that the book is never crossed.
- **Sanitizers**: the suite is run under AddressSanitizer and UndefinedBehaviorSanitizer (`make asan`).
- **Mutation spot-checks**: deliberately breaking the engine (LIFO instead of FIFO, ignoring the
  limit price in the FOK check, off-by-one in the crossing test) makes the suite fail.

## Benchmark

```bash
./build/ob_bench                       # defaults: 2,000,000 ops, 5 runs
./build/ob_bench --ops 5000000 --seed 7 --runs 10
```

The operation stream is pre-generated from a seed (deterministic, RNG cost excluded). It models a
quiet market: orders cluster around a slowly drifting mid price; roughly 50% passive limits,
10% aggressive limits, 3% market, 2% IOC, 30% cancels, 5% modifies. Two passes are run:

1. **Throughput** over the whole stream with no per-operation timing (best / median / worst of N runs).
2. **Latency**: each operation timed with `steady_clock`; p50/p90/p99/p99.9/max are reported along
   with the measured timer overhead so you can judge how much of the number is the clock itself.

Always benchmark a Release build (the CMake default) and close other heavy programs.

### Results

Default settings (`./build/ob_bench`): 2,000,000 operations, seed 12345, 5 runs, single thread,
Release build (`-O2`).

| Machine | Compiler / flags | Median throughput | Best / worst | p99 | p99.9 |
|---|---|---|---|---|---|
| Apple M2, 8 GB RAM, macOS 26.3 | Apple clang 17.0.0 (clang-1700.0.13.5), `-O2 -DNDEBUG` | **15.35 M ops/s** | 15.41 / 12.59 M ops/s | 333 ns | 625 ns |

Workload outcome for that run: 861,587 trades (21.9M units traded), 42.5% of cancels hit a live
order, and 165,615 orders were resting at the end across 89 bid and 115 ask levels.

Notes on reading these numbers:

- **Throughput** is measured over the whole stream with no per-operation timing, so it is not
  affected by the clock.
- **Latency** is measured per operation with `steady_clock`. On this machine the clock ticks in
  steps of about 42 ns (the reported values are multiples of that), so the p50 of 42 ns means
  "about one tick" and is not a precise figure. The p99 and p99.9 values are still meaningful as
  upper bounds. The maximum (about 126 us) is operating-system scheduling noise, not the book.
- Results are for one machine; treat them as relative, not absolute. Re-run on your hardware and
  compare before and after any optimization.

## Known limitations and ideas for next steps

- **Single-threaded.** One book per thread; a real deployment shards symbols across threads.
- **Allocation per resting order** (`std::list` node + hash-map node). The natural next optimization
  is a pooled/arena allocator (`std::pmr`) or an intrusive list over a preallocated order array, and a
  flat open-addressing id map. Measure with `ob_bench` before and after.
- **Price levels in a `std::map`** are pointer-chasing; a dense array indexed by tick (when the price
  range is bounded) or a sorted vector for the hot levels would improve cache behavior.
- No self-trade prevention, stop orders, iceberg orders, or auction/opening-cross logic.
- Order ids are caller-supplied at the `OrderBook` level and engine-assigned at the `MatchingEngine` level.
- Total quantity at one price level is a `uint64_t`; summing beyond 2^64 would overflow (unrealistic here).
- Persistence and replay: logging every accepted command would give deterministic recovery, since
  the matching logic is deterministic.

## Layout

```
include/ob/       types.hpp, order_book.hpp, matching_engine.hpp
src/              order_book.cpp, matching_engine.cpp, cli_main.cpp
tests/            minitest.hpp (harness), unit tests, differential fuzz test
bench/            bench_main.cpp
examples/         demo.txt
```