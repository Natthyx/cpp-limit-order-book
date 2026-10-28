cmake_minimum_required(VERSION 3.16)
project(orderbook VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimized build; benchmark numbers from a Debug build are meaningless.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_BUILD_TESTS "Build the unit tests" ON)
option(OB_BUILD_BENCH "Build the benchmark" ON)
option(OB_BUILD_CLI "Build the command-line front end" ON)
option(OB_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(OB_WERROR "Treat compiler warnings as errors" OFF)

if(MSVC)
  add_compile_options(/W4)
  if(OB_WERROR)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(OB_WERROR)
    add_compile_options(-Werror)
  endif()
  if(OB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# ---- Core library ----------------------------------------------------------
add_library(ob_core STATIC
  src/order_book.cpp
  src/matching_engine.cpp
)
target_include_directories(ob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- Command-line front end ------------------------------------------------
if(OB_BUILD_CLI)
  add_executable(ob_cli src/cli_main.cpp)
  target_link_libraries(ob_cli PRIVATE ob_core)
endif()

# ---- Benchmark -------------------------------------------------------------
if(OB_BUILD_BENCH)
  add_executable(ob_bench bench/bench_main.cpp)
  target_link_libraries(ob_bench PRIVATE ob_core)
endif()

# ---- Tests -----------------------------------------------------------------
if(OB_BUILD_TESTS)
  enable_testing()
  add_executable(ob_tests
    tests/test_main.cpp
    tests/test_order_book.cpp
    tests/test_engine.cpp
    tests/test_fuzz.cpp
  )
  target_include_directories(ob_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(ob_tests PRIVATE ob_core)
  add_test(NAME ob_tests COMMAND ob_tests)
endif()
