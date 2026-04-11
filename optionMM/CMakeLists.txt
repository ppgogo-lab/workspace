cmake_minimum_required(VERSION 3.20)
project(optionMM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Compiler flags ───────────────────────────────────────────────────────────
# Note: target CPU is Intel i7-9700 (Coffeelake) — supports AVX2 but NOT AVX-512.
# On the production Xeon Gold 6544Y, replace -mavx2 with -mavx512f -mavx512dq.
set(OMM_COMMON_FLAGS "-Wall -Wextra -Wpedantic -fno-rtti")

# Release: optimise aggressively, enable AVX2 SIMD
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mavx2 -DNDEBUG ${OMM_COMMON_FLAGS}")

# Debug: full debug info, sanitizers
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g3 -fsanitize=address,undefined ${OMM_COMMON_FLAGS}")

# RelWithDebInfo: for profiling
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer -march=native -mavx2 ${OMM_COMMON_FLAGS}")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ─── Global include directory ─────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR})          # for proto/gen/ includes

# ─── Dependencies ─────────────────────────────────────────────────────────────
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(Ceres REQUIRED)

# gRPC via pkg-config (Ubuntu packages install it this way)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++)
pkg_check_modules(GRPCPP REQUIRED grpc++)

# ─── common_lib ───────────────────────────────────────────────────────────────
add_library(common_lib STATIC
    src/common/config.cpp
    src/common/thread_utils.cpp
)
target_link_libraries(common_lib PUBLIC
    yaml-cpp
    Threads::Threads
)

# ─── pricing_lib ──────────────────────────────────────────────────────────────
add_library(pricing_lib STATIC
    src/pricing/black76.cpp
    src/pricing/svi.cpp
    src/pricing/sabr.cpp
    src/pricing/cubic_spline.cpp
    src/pricing/wing.cpp
    src/pricing/orc_wing.cpp
)
target_compile_options(pricing_lib PRIVATE -mavx2 -mfma)
target_link_libraries(pricing_lib PUBLIC Ceres::ceres)

# ─── risk_lib ─────────────────────────────────────────────────────────────────
add_library(risk_lib STATIC
    src/risk/pre_trade_risk.cpp
    src/risk/post_trade_risk.cpp
)

# ─── strategy_lib ─────────────────────────────────────────────────────────────
add_library(strategy_lib STATIC
    src/strategy/mm_framework.cpp
    src/strategy/simple_mm.cpp
)
target_link_libraries(strategy_lib PUBLIC risk_lib common_lib)

# ─── feed_lib ─────────────────────────────────────────────────────────────────
add_library(feed_lib STATIC
    src/feed/multicast_feed.cpp
    src/feed/fpga_feed.cpp
)
target_link_libraries(feed_lib PUBLIC common_lib)

# ─── Exchange SDK paths ───────────────────────────────────────────────────────
set(CTP_SDK_DIR   "${CMAKE_SOURCE_DIR}/third_party/ctp")
set(FEMAS_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/femas")

# CTP .so has no "lib" prefix so -lthosttraderapi_se won't find it.
# Use IMPORTED targets to link by absolute path.
add_library(ctp_trader SHARED IMPORTED)
set_target_properties(ctp_trader PROPERTIES
    IMPORTED_LOCATION "${CTP_SDK_DIR}/thosttraderapi_se.so")

add_library(femas_trader SHARED IMPORTED)
set_target_properties(femas_trader PROPERTIES
    IMPORTED_LOCATION "${FEMAS_SDK_DIR}/libUSTPtraderapiAF.so"
    # FEMAS SDK was built against OpenSSL 1.0 (CentOS 7); suppress unresolved
    # SSL symbols at link time — they are only needed at runtime on the target host.
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    INTERFACE_LINK_OPTIONS "-Wl,--allow-shlib-undefined")

# ─── sim_gateway_lib (SimGateway only — no exchange SDK, used by tests) ──────
add_library(sim_gateway_lib STATIC
    src/gateway/sim_gateway.cpp
)
target_include_directories(sim_gateway_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sim_gateway_lib PUBLIC common_lib logger_lib)

# ─── gateway_lib (full: Sim + CTP + FEMAS — used by main binary) ─────────────
add_library(gateway_lib STATIC
    src/gateway/sim_gateway.cpp
    src/gateway/ctp_gateway.cpp
    src/gateway/femas_gateway.cpp
)
target_include_directories(gateway_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CTP_SDK_DIR}
    ${FEMAS_SDK_DIR}
)
target_link_libraries(gateway_lib PUBLIC
    common_lib logger_lib ctp_trader femas_trader
)

# ─── logger_lib ───────────────────────────────────────────────────────────────
add_library(logger_lib STATIC
    src/logger/logger.cpp
)
target_link_libraries(logger_lib PUBLIC spdlog::spdlog Threads::Threads)

# ─── engine_lib ───────────────────────────────────────────────────────────────
# Full engine: links gateway_lib which includes CTP+FEMAS SDK .so files.
add_library(engine_lib STATIC
    src/engine/trading_engine.cpp
)
target_compile_options(engine_lib PRIVATE -mavx2 -mfma)
target_link_libraries(engine_lib PUBLIC
    common_lib pricing_lib risk_lib strategy_lib feed_lib gateway_lib logger_lib)

# Test-only engine: links sim_gateway_lib (no CTP/FEMAS SDK dependencies).
add_library(engine_lib_test STATIC
    src/engine/trading_engine.cpp
)
target_compile_options(engine_lib_test PRIVATE -mavx2 -mfma)
target_link_libraries(engine_lib_test PUBLIC
    common_lib pricing_lib risk_lib strategy_lib feed_lib sim_gateway_lib logger_lib)

# ─── monitoring_lib (gRPC server) ─────────────────────────────────────────────
add_library(monitoring_lib STATIC
    proto/gen/trading.pb.cc
    proto/gen/trading.grpc.pb.cc
    src/monitoring/grpc_server.cpp
)
target_include_directories(monitoring_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
    ${Protobuf_INCLUDE_DIRS}
    ${GRPC_INCLUDE_DIRS}
)
target_link_libraries(monitoring_lib PUBLIC
    engine_lib
    logger_lib
    ${Protobuf_LIBRARIES}
    ${GRPC_LIBRARIES}
    Threads::Threads
)

# ─── Main executable ──────────────────────────────────────────────────────────
add_executable(optionmm src/main.cpp)
target_link_libraries(optionmm PRIVATE monitoring_lib engine_lib)

# ─── Tests ────────────────────────────────────────────────────────────────────
enable_testing()
find_package(GTest REQUIRED)

# test_ring_buffer
add_executable(test_ring_buffer tests/test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer PRIVATE GTest::gtest_main Threads::Threads)
add_test(NAME test_ring_buffer COMMAND test_ring_buffer)

# test_black76
add_executable(test_black76 tests/test_black76.cpp)
target_link_libraries(test_black76 PRIVATE pricing_lib GTest::gtest_main)
target_compile_options(test_black76 PRIVATE -mavx2 -mfma)
add_test(NAME test_black76 COMMAND test_black76)

# test_vol_surface
add_executable(test_vol_surface tests/test_vol_surface.cpp)
target_link_libraries(test_vol_surface PRIVATE pricing_lib GTest::gtest_main Threads::Threads)
add_test(NAME test_vol_surface COMMAND test_vol_surface)

# test_pre_trade_risk
add_executable(test_pre_trade_risk tests/test_pre_trade_risk.cpp)
target_link_libraries(test_pre_trade_risk PRIVATE risk_lib GTest::gtest_main)
add_test(NAME test_pre_trade_risk COMMAND test_pre_trade_risk)

# test_simple_mm (strategy + gateway + engine integration)
# Uses engine_lib_test which does not pull in CTP/FEMAS SDK shared libraries.
add_executable(test_simple_mm tests/test_simple_mm.cpp)
target_link_libraries(test_simple_mm PRIVATE engine_lib_test GTest::gtest_main Threads::Threads)
add_test(NAME test_simple_mm COMMAND test_simple_mm)

# test_latency — NOT added to ctest (run explicitly for benchmarking)
# Build with RelWithDebInfo for realistic latency numbers.
add_executable(test_latency tests/test_latency.cpp)
target_link_libraries(test_latency PRIVATE engine_lib_test GTest::gtest_main Threads::Threads)
target_compile_options(test_latency PRIVATE -mavx2 -mfma)
# Intentionally no add_test() — latency benchmarks must be run explicitly:
#   ./build/tests/test_latency --gtest_filter="LatencyTest.*"

# test_vol_calibration — reads MarketTick.csv, calibrates all 4 vol models, writes CSV results
# Run manually: ./build/test_vol_calibration
add_executable(test_vol_calibration tests/test_vol_calibration.cpp)
target_link_libraries(test_vol_calibration PRIVATE pricing_lib GTest::gtest_main)
target_compile_options(test_vol_calibration PRIVATE -mavx2 -mfma)
# Intentionally no add_test() — depends on external CSV file path
