cmake_minimum_required(VERSION 3.16)
project(matching_engine CXX)

# Silences the CMP0135 dev warning for FetchContent URL downloads (extract
# timestamps = extraction time, so content changes are always picked up).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Warnings-as-signal, not warnings-as-errors yet -- Phase 1 is about correctness,
# not about being pedantic. We'll tighten this in later phases.
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

# --- Core library ---------------------------------------------------------
# Everything that isn't "how do I run this" lives in a static lib so both
# the CLI executable and the test binary link against the exact same code.
add_library(engine_core
    src/order_book.cpp
    src/fast_order_book.cpp
    src/concurrent_book.cpp
    src/order_stream.cpp
    src/fix_like.cpp
)
target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core PUBLIC Threads::Threads)

# --- CLI / demo executable -------------------------------------------------
add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE engine_core)

# --- Tests ------------------------------------------------------------------
option(ENGINE_BUILD_TESTS "Build unit tests" ON)
if(ENGINE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  )
  # Windows-specific gtest quirk, harmless elsewhere; keep for portability.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()

  add_executable(engine_tests
    tests/test_order_book.cpp
    tests/test_invariants.cpp
    tests/test_ioc.cpp
    tests/test_cancel_replace.cpp
    tests/test_fast_book.cpp
    tests/test_differential.cpp
    tests/test_spsc_ring.cpp
    tests/test_concurrent_book.cpp
    tests/test_flat_hash_map.cpp
    tests/test_fix_like.cpp
    tests/test_market_data.cpp
  )
  target_link_libraries(engine_tests PRIVATE engine_core GTest::gtest_main)
  target_include_directories(engine_tests PRIVATE include)

  include(GoogleTest)
  gtest_discover_tests(engine_tests)
endif()

# --- Benchmarks --------------------------------------------------------------
option(ENGINE_BUILD_BENCHMARKS "Build Google Benchmark harness" ON)
if(ENGINE_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.zip
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)

  add_executable(engine_bench bench/bench_order_book.cpp)
  target_link_libraries(engine_bench PRIVATE engine_core benchmark::benchmark)
  target_include_directories(engine_bench PRIVATE include)

  add_executable(engine_bench_concurrent bench/bench_concurrent.cpp)
  target_link_libraries(engine_bench_concurrent PRIVATE engine_core benchmark::benchmark)
  target_include_directories(engine_bench_concurrent PRIVATE include)
endif()

# --- Phase 5: latency measurement tool + HdrHistogram --------------------------
option(ENGINE_BUILD_LATENCY_TOOL "Build the Phase 5 latency tool" ON)
if(ENGINE_BUILD_LATENCY_TOOL)
  include(FetchContent)
  FetchContent_Declare(
    hdrhistogram
    URL https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.11.8.zip
  )
  # DISABLED drops the optional (zlib-backed) log component so the library
  # builds with no system dependencies beyond the C compiler.
  set(HDR_LOG_REQUIRED "DISABLED" CACHE STRING "HDR logging component" FORCE)
  set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(HDR_HISTOGRAM_BUILD_STATIC ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(hdrhistogram)

  add_executable(engine_latency tools/latency_bench.cpp)
  target_link_libraries(engine_latency PRIVATE engine_core hdr_histogram_static)
  target_include_directories(engine_latency PRIVATE include ${hdrhistogram_SOURCE_DIR}/include)

  # Phase 6: market-data feed server + client (sockets).
  add_executable(engine_feed tools/feed_server.cpp src/net.cpp)
  target_link_libraries(engine_feed PRIVATE engine_core)
  target_include_directories(engine_feed PRIVATE include)
  if(WIN32)
    target_link_libraries(engine_feed PRIVATE ws2_32)
  endif()

  add_executable(latency_tests
    tests/test_latency_histogram.cpp
    tests/test_order_stream.cpp
  )
  target_link_libraries(latency_tests PRIVATE engine_core GTest::gtest_main hdr_histogram_static)
  target_include_directories(latency_tests PRIVATE include ${hdrhistogram_SOURCE_DIR}/include)
  include(GoogleTest)
  gtest_discover_tests(latency_tests)
endif()

# --- Phase 6: PGO build plumbing (GCC/Clang, two phases) -----------------------
# Configure with -DENGINE_PGO=GENERATE, train with a benchmark run, then
# reconfigure with -DENGINE_PGO=USE and rebuild:
#   cmake -S . -B build-pgo -DENGINE_PGO=GENERATE
#   ./build-pgo/engine_bench --benchmark_min_time=0.2s   (trains profiles)
#   cmake -S . -B build-pgo -DENGINE_PGO=USE
#   cmake --build build-pgo
set(ENGINE_PGO "" CACHE STRING "PGO mode for engine_bench: GENERATE or USE")
if(ENGINE_PGO STREQUAL "GENERATE")
  target_compile_options(engine_bench PRIVATE -fprofile-generate)
  target_link_options(engine_bench PRIVATE -fprofile-generate)
elseif(ENGINE_PGO STREQUAL "USE")
  target_compile_options(engine_bench PRIVATE -fprofile-use -fprofile-correction)
  target_link_options(engine_bench PRIVATE -fprofile-use)
endif()
