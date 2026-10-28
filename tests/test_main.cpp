#include <cstdio>
#include <cstring>

#include "minitest.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  int failed = 0;
  for (const auto& test : minitest::registry()) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr) continue;
    minitest::failures_in_current_test() = 0;
    test.fn();
    ++ran;
    if (minitest::failures_in_current_test() == 0) {
      std::printf("[ OK ] %s\n", test.name);
    } else {
      std::printf("[FAIL] %s\n", test.name);
      ++failed;
    }
  }
  std::printf("\n%d test(s) run, %d failed\n", ran, failed);
  return failed == 0 ? 0 : 1;
}
