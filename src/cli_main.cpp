// Interactive / scripted front end for the matching engine.
//
//   ./ob_cli                    read commands from stdin
//   ./ob_cli examples/demo.txt  run a script (commands are echoed)
//
// Type HELP for the command list.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ob/matching_engine.hpp"

using namespace ob;

namespace {

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

bool parse_unsigned(const std::string& s, std::uint64_t& out) {
  if (s.empty() || !std::all_of(s.begin(), s.end(),
                                [](unsigned char c) { return std::isdigit(c) != 0; })) {
    return false;
  }
  errno = 0;
  out = std::strtoull(s.c_str(), nullptr, 10);
  return errno == 0;
}

bool parse_signed(const std::string& s, std::int64_t& out) {
  if (s.empty()) return false;
  const bool negative = s[0] == '-';
  std::uint64_t magnitude = 0;
  if (!parse_unsigned(negative ? s.substr(1) : s, magnitude)) return false;
  if (magnitude > static_cast<std::uint64_t>(INT64_MAX)) return false;
  out = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
  return true;
}

bool parse_side(const std::string& s, Side& out) {
  const std::string u = upper(s);
  if (u == "BUY" || u == "B") { out = Side::Buy; return true; }
  if (u == "SELL" || u == "S") { out = Side::Sell; return true; }
  return false;
}

bool parse_tif(const std::string& s, TimeInForce& out) {
  const std::string u = upper(s);
  if (u == "GTC") { out = TimeInForce::GTC; return true; }
  if (u == "IOC") { out = TimeInForce::IOC; return true; }
  if (u == "FOK") { out = TimeInForce::FOK; return true; }
  return false;
}

const char* kHelp =
    "Commands (prices are integer ticks, e.g. 10050 = 100.50):\n"
    "  LIMIT  <BUY|SELL> <symbol> <price> <qty> [GTC|IOC|FOK]\n"
    "  MARKET <BUY|SELL> <symbol> <qty>\n"
    "  CANCEL <symbol> <order_id>\n"
    "  MODIFY <symbol> <order_id> <new_price> <new_qty>\n"
    "  BOOK   <symbol> [depth]\n"
    "  SYMBOLS | HELP | QUIT\n"
    "Lines starting with # are comments.\n";

class Cli {
 public:
  explicit Cli(std::ostream& out) : out_(out) {}

  // Returns false when the session should end.
  bool execute(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> t;
    for (std::string tok; in >> tok;) t.push_back(tok);
    if (t.empty() || t[0][0] == '#') return true;

    const std::string cmd = upper(t[0]);
    if (cmd == "QUIT" || cmd == "EXIT") return false;
    if (cmd == "HELP") { out_ << kHelp; return true; }
    if (cmd == "SYMBOLS") { list_symbols(); return true; }
    if (cmd == "LIMIT") { limit(t); return true; }
    if (cmd == "MARKET") { market(t); return true; }
    if (cmd == "CANCEL") { cancel(t); return true; }
    if (cmd == "MODIFY") { modify(t); return true; }
    if (cmd == "BOOK") { book(t); return true; }
    out_ << "ERROR unknown command '" << t[0] << "' (try HELP)\n";
    return true;
  }

 private:
  void usage(const char* text) { out_ << "ERROR usage: " << text << "\n"; }

  void limit(const std::vector<std::string>& t) {
    Side side;
    Price price;
    std::uint64_t qty;
    TimeInForce tif = TimeInForce::GTC;
    if ((t.size() != 5 && t.size() != 6) || !parse_side(t[1], side) ||
        !parse_signed(t[3], price) || !parse_unsigned(t[4], qty) ||
        (t.size() == 6 && !parse_tif(t[5], tif))) {
      return usage("LIMIT <BUY|SELL> <symbol> <price> <qty> [GTC|IOC|FOK]");
    }
    print(t[2], engine_.submit_limit(upper(t[2]), side, price, qty, tif));
  }

  void market(const std::vector<std::string>& t) {
    Side side;
    std::uint64_t qty;
    if (t.size() != 4 || !parse_side(t[1], side) || !parse_unsigned(t[3], qty)) {
      return usage("MARKET <BUY|SELL> <symbol> <qty>");
    }
    print(t[2], engine_.submit_market(upper(t[2]), side, qty));
  }

  void cancel(const std::vector<std::string>& t) {
    std::uint64_t id;
    if (t.size() != 3 || !parse_unsigned(t[2], id)) return usage("CANCEL <symbol> <order_id>");
    if (engine_.cancel(upper(t[1]), id)) {
      out_ << "CANCELLED id=" << id << "\n";
    } else {
      out_ << "REJECT id=" << id << " reason=unknown_order\n";
    }
  }

  void modify(const std::vector<std::string>& t) {
    std::uint64_t id;
    std::uint64_t qty;
    Price price;
    if (t.size() != 5 || !parse_unsigned(t[2], id) || !parse_signed(t[3], price) ||
        !parse_unsigned(t[4], qty)) {
      return usage("MODIFY <symbol> <order_id> <new_price> <new_qty>");
    }
    print(t[1], engine_.modify(upper(t[1]), id, price, qty));
  }

  void book(const std::vector<std::string>& t) {
    std::uint64_t depth = 10;
    if (t.size() < 2 || t.size() > 3 || (t.size() == 3 && !parse_unsigned(t[2], depth))) {
      return usage("BOOK <symbol> [depth]");
    }
    const OrderBook* b = engine_.find_book(upper(t[1]));
    if (b == nullptr) {
      out_ << "ERROR no book for symbol " << upper(t[1]) << "\n";
      return;
    }
    const auto asks = b->depth(Side::Sell, depth);
    const auto bids = b->depth(Side::Buy, depth);
    out_ << "BOOK " << b->symbol() << "\n";
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) row("ASK", *it);
    if (!asks.empty() && !bids.empty()) {
      out_ << "  ------ spread " << (asks.front().price - bids.front().price) << "\n";
    } else {
      out_ << "  ------\n";
    }
    for (const auto& level : bids) row("BID", level);
  }

  void row(const char* side, const Level& l) {
    out_ << "  " << side << " " << l.price << " x " << l.quantity << " (" << l.orders
         << (l.orders == 1 ? " order)" : " orders)") << "\n";
  }

  void list_symbols() {
    const auto s = engine_.symbols();
    if (s.empty()) out_ << "(no symbols yet)\n";
    for (const auto& name : s) out_ << name << "\n";
  }

  void print(const std::string& symbol, const MatchingEngine::Report& r) {
    const std::string sym = upper(symbol);
    if (r.result.status == OrderStatus::Rejected) {
      out_ << "REJECT id=" << r.id << " reason=" << to_string(r.result.reason) << "\n";
      return;
    }
    out_ << "ACK id=" << r.id << " status=" << to_string(r.result.status)
         << " filled=" << r.result.filled << " remaining=" << r.result.remaining << "\n";
    for (const Trade& tr : r.trades) {
      out_ << "TRADE seq=" << tr.sequence << " symbol=" << sym << " taker=" << tr.taker_id
           << " maker=" << tr.maker_id << " side=" << to_string(tr.taker_side)
           << " price=" << tr.price << " qty=" << tr.quantity << "\n";
    }
  }

  std::ostream& out_;
  MatchingEngine engine_;
};

}  // namespace

int main(int argc, char** argv) {
  Cli cli(std::cout);
  if (argc > 2) {
    std::cerr << "usage: " << argv[0] << " [script_file]\n";
    return 2;
  }
  if (argc == 2) {
    std::ifstream file(argv[1]);
    if (!file) {
      std::cerr << "cannot open " << argv[1] << "\n";
      return 1;
    }
    for (std::string line; std::getline(file, line);) {
      if (!line.empty() && line[0] != '#') std::cout << "> " << line << "\n";
      if (!cli.execute(line)) break;
    }
    return 0;
  }
  for (std::string line; std::getline(std::cin, line);) {
    if (!cli.execute(line)) break;
  }
  return 0;
}
