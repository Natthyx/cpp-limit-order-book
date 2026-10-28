# Fallback build for machines without CMake:  make test | make bench | make cli
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
INC      := -Iinclude -Itests
BUILD    := build-make

CORE  := src/order_book.cpp src/matching_engine.cpp
TESTS := tests/test_main.cpp tests/test_order_book.cpp tests/test_engine.cpp tests/test_fuzz.cpp

.PHONY: all test asan bench cli clean
all: $(BUILD)/ob_tests $(BUILD)/ob_bench $(BUILD)/ob_cli

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ob_tests: $(CORE) $(TESTS) $(wildcard include/ob/*.hpp tests/*.hpp) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $(CORE) $(TESTS) -o $@

$(BUILD)/ob_tests_asan: $(CORE) $(TESTS) $(wildcard include/ob/*.hpp tests/*.hpp) | $(BUILD)
	$(CXX) $(CXXFLAGS) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) $(CORE) $(TESTS) -o $@

$(BUILD)/ob_bench: $(CORE) bench/bench_main.cpp $(wildcard include/ob/*.hpp) | $(BUILD)
	$(CXX) $(CXXFLAGS) -DNDEBUG $(INC) $(CORE) bench/bench_main.cpp -o $@

$(BUILD)/ob_cli: $(CORE) src/cli_main.cpp $(wildcard include/ob/*.hpp) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $(CORE) src/cli_main.cpp -o $@

test: $(BUILD)/ob_tests
	./$(BUILD)/ob_tests

asan: $(BUILD)/ob_tests_asan
	./$(BUILD)/ob_tests_asan

bench: $(BUILD)/ob_bench
	./$(BUILD)/ob_bench

cli: $(BUILD)/ob_cli
	./$(BUILD)/ob_cli examples/demo.txt

clean:
	rm -rf $(BUILD) build
