#pragma once
// A tiny, dependency-free test harness (so the project builds offline with
// nothing but a C++17 compiler). Provides TEST, CHECK, CHECK_EQ, CHECK_MSG.

#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace minitest {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failures_in_current_test() {
  static int n = 0;
  return n;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const std::string& what) {
  ++failures_in_current_test();
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, what.c_str());
}

template <class T>
struct is_optional : std::false_type {};
template <class T>
struct is_optional<std::optional<T>> : std::true_type {};

template <class T>
std::string str(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else if constexpr (std::is_same_v<T, bool>) {
    return v ? "true" : "false";
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(v);
  } else if constexpr (std::is_same_v<T, std::nullopt_t>) {
    return "nullopt";
  } else if constexpr (is_optional<T>::value) {
    return v ? "Some(" + str(*v) + ")" : std::string("nullopt");
  } else {
    return "<value>";
  }
}

// Equality that is safe for mixed signed/unsigned integers (no sign-compare
// warnings, and -1 never equals 2^64-1).
template <class A, class B>
bool equal(const A& a, const B& b) {
  if constexpr (std::is_integral_v<A> && std::is_integral_v<B> &&
                std::is_signed_v<A> != std::is_signed_v<B>) {
    if constexpr (std::is_signed_v<A>) {
      return a >= 0 && static_cast<std::make_unsigned_t<A>>(a) == b;
    } else {
      return b >= 0 && a == static_cast<std::make_unsigned_t<B>>(b);
    }
  } else {
    return a == b;
  }
}

}  // namespace minitest

#define TEST(name)                                                  \
  static void name();                                               \
  static ::minitest::Registrar minitest_registrar_##name(#name, &name); \
  static void name()

#define CHECK(expr)                                                       \
  do {                                                                    \
    if (!(expr)) ::minitest::report_failure(__FILE__, __LINE__, #expr);   \
  } while (0)

#define CHECK_MSG(expr, msg)                                              \
  do {                                                                    \
    if (!(expr))                                                          \
      ::minitest::report_failure(__FILE__, __LINE__,                      \
                                 std::string(#expr " -- ") + (msg));      \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    const auto minitest_a = (a);                                         \
    const auto minitest_b = (b);                                         \
    if (!::minitest::equal(minitest_a, minitest_b)) {                     \
      ::minitest::report_failure(                                         \
          __FILE__, __LINE__,                                             \
          std::string(#a " == " #b "  (got ") + ::minitest::str(minitest_a) + \
              " vs " + ::minitest::str(minitest_b) + ")");                \
    }                                                                     \
  } while (0)
