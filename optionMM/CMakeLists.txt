cmake_minimum_required(VERSION 3.20)
project(optionMM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(OMM_BUILD_TRADER_GUI "Build the optional Qt trader viewer" OFF)
option(OMM_ENABLE_BLACK76_AVX512 "Build the AVX-512 Black76 backend" ON)
set(OMM_PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/trading.proto")
set(OMM_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto/gen")
file(MAKE_DIRECTORY "${OMM_PROTO_GEN_DIR}")

if(WIN32)
  if(NOT OMM_BUILD_TRADER_GUI)
    message(FATAL_ERROR "Windows builds currently support only the trader GUI. Configure with -DOMM_BUILD_TRADER_GUI=ON.")
  endif()

  find_package(Protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

  if(TARGET protobuf::protoc)
    set(OMM_PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
  else()
    find_program(OMM_PROTOC_EXECUTABLE protoc REQUIRED)
  endif()
  if(TARGET gRPC::grpc_cpp_plugin)
    set(OMM_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  else()
    find_program(OMM_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
  endif()

  add_custom_command(
      OUTPUT
          "${OMM_PROTO_GEN_DIR}/trading.pb.cc"
          "${OMM_PROTO_GEN_DIR}/trading.pb.h"
          "${OMM_PROTO_GEN_DIR}/trading.grpc.pb.cc"
          "${OMM_PROTO_GEN_DIR}/trading.grpc.pb.h"
      COMMAND ${OMM_PROTOC_EXECUTABLE}
      ARGS
          --proto_path=${CMAKE_SOURCE_DIR}/proto
          --cpp_out=${OMM_PROTO_GEN_DIR}
          --grpc_out=${OMM_PROTO_GEN_DIR}
          --plugin=protoc-gen-grpc=${OMM_GRPC_CPP_PLUGIN_EXECUTABLE}
          ${OMM_PROTO_FILE}
      DEPENDS ${OMM_PROTO_FILE}
      VERBATIM)

  add_executable(optionmm_trader_gui
      "${OMM_PROTO_GEN_DIR}/trading.pb.cc"
      "${OMM_PROTO_GEN_DIR}/trading.grpc.pb.cc"
      src/gui/main.cpp
      src/gui/trader_main_window.cpp
  )
  if(MSVC)
    target_compile_options(optionmm_trader_gui PRIVATE /EHsc)
  endif()
  target_include_directories(optionmm_trader_gui PRIVATE
      ${OMM_PROTO_GEN_DIR}
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}
  )
  target_link_libraries(optionmm_trader_gui PRIVATE
      Qt6::Core
      Qt6::Gui
      Qt6::Widgets
      protobuf::libprotobuf
      gRPC::grpc++
      gRPC::grpc
      ws2_32
      crypt32
  )
  return()
endif()

# ─── Compiler flags ───────────────────────────────────────────────────────────
# Keep the baseline binary portable across build and run hosts.
# Black76 ISA-specific code is compiled per-source and selected at runtime.
set(OMM_COMMON_FLAGS "-Wall -Wextra -Wpedantic -fno-rtti")

# Release: optimise aggressively
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG ${OMM_COMMON_FLAGS}")

# Debug: full debug info, sanitizers
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g3 -fsanitize=address,undefined ${OMM_COMMON_FLAGS}")

# RelWithDebInfo: for profiling
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer ${OMM_COMMON_FLAGS}")

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
find_package(SQLite3 REQUIRED)

# gRPC via pkg-config (Ubuntu packages install it this way)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GRPC REQUIRED grpc++)
pkg_check_modules(GRPCPP REQUIRED grpc++)
find_program(OMM_PROTOC_EXECUTABLE protoc REQUIRED)
find_program(OMM_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)

add_custom_command(
    OUTPUT
        "${OMM_PROTO_GEN_DIR}/trading.pb.cc"
        "${OMM_PROTO_GEN_DIR}/trading.pb.h"
        "${OMM_PROTO_GEN_DIR}/trading.grpc.pb.cc"
        "${OMM_PROTO_GEN_DIR}/trading.grpc.pb.h"
    COMMAND ${OMM_PROTOC_EXECUTABLE}
    ARGS
        --proto_path=${CMAKE_SOURCE_DIR}/proto
        --cpp_out=${OMM_PROTO_GEN_DIR}
        --grpc_out=${OMM_PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${OMM_GRPC_CPP_PLUGIN_EXECUTABLE}
        ${OMM_PROTO_FILE}
    DEPENDS ${OMM_PROTO_FILE}
    VERBATIM)

# Exchange SDK paths
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
    # SSL symbols at link time. They are only needed at runtime on the target host.
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    INTERFACE_LINK_OPTIONS "-Wl,--allow-shlib-undefined")

add_library(femas_md SHARED IMPORTED)
set_target_properties(femas_md PROPERTIES
    IMPORTED_LOCATION "${FEMAS_SDK_DIR}/libUSTPmduserapiAF.so"
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    INTERFACE_LINK_OPTIONS "-Wl,--allow-shlib-undefined")

add_library(femas_simulator_lib STATIC
    src/sim/femas_simulator.cpp
)
target_include_directories(femas_simulator_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${FEMAS_SDK_DIR}
)
target_link_libraries(femas_simulator_lib PUBLIC
    logger_lib
    Threads::Threads
)

add_library(femas_api_lib STATIC
    src/femas/femas_api.cpp
)
target_include_directories(femas_api_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${FEMAS_SDK_DIR}
)
target_link_libraries(femas_api_lib PUBLIC
    femas_simulator_lib
    femas_md
    femas_trader
)

# ─── common_lib ───────────────────────────────────────────────────────────────
add_library(common_lib STATIC
    src/common/config.cpp
    src/common/thread_utils.cpp
    src/sim/sim_instruments.cpp
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
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  target_sources(pricing_lib PRIVATE src/pricing/black76_avx2.cpp)
  set_source_files_properties(src/pricing/black76_avx2.cpp PROPERTIES
      COMPILE_OPTIONS "-mavx2;-mfma")
  target_compile_definitions(pricing_lib PUBLIC OMM_BLACK76_AVX2_BACKEND=1)

  if(OMM_ENABLE_BLACK76_AVX512)
    target_sources(pricing_lib PRIVATE src/pricing/black76_avx512.cpp)
    set_source_files_properties(src/pricing/black76_avx512.cpp PROPERTIES
        COMPILE_OPTIONS "-mavx512f;-mfma")
    target_compile_definitions(pricing_lib PUBLIC OMM_BLACK76_AVX512_BACKEND=1)
  endif()
endif()
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
    src/strategy/option_mm_core.cpp
    src/strategy/pcp_arbitrage.cpp
)
target_link_libraries(strategy_lib PUBLIC risk_lib common_lib)

# ─── feed_lib ─────────────────────────────────────────────────────────────────
add_library(feed_lib STATIC
    src/feed/multicast_feed.cpp
    src/feed/fpga_feed.cpp
    src/feed/femas_feed.cpp
    src/feed/sim_feed.cpp
)
target_include_directories(feed_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${FEMAS_SDK_DIR}
)
target_link_libraries(feed_lib PUBLIC
    common_lib
    femas_api_lib
)

if(0)

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

add_library(femas_md SHARED IMPORTED)
set_target_properties(femas_md PROPERTIES
    IMPORTED_LOCATION "${FEMAS_SDK_DIR}/libUSTPmduserapiAF.so"
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    INTERFACE_LINK_OPTIONS "-Wl,--allow-shlib-undefined")

# ─── sim_gateway_lib (SimGateway only — no exchange SDK, used by tests) ──────
endif()

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
    common_lib logger_lib ctp_trader femas_api_lib
)

# ─── logger_lib ───────────────────────────────────────────────────────────────
add_library(logger_lib STATIC
    src/logger/logger.cpp
)
target_link_libraries(logger_lib PUBLIC spdlog::spdlog Threads::Threads)

add_library(persistence_lib STATIC
    src/persistence/data_repository.cpp
)
target_link_libraries(persistence_lib PUBLIC
    common_lib
    logger_lib
    SQLite::SQLite3
)

# ─── engine_lib ───────────────────────────────────────────────────────────────
# Full engine: links gateway_lib which includes CTP+FEMAS SDK .so files.
add_library(engine_lib STATIC
    src/engine/trading_engine.cpp
)
target_link_libraries(engine_lib PUBLIC
    common_lib pricing_lib risk_lib strategy_lib feed_lib gateway_lib logger_lib persistence_lib)

# Test-only engine: links sim_gateway_lib (no CTP/FEMAS SDK dependencies).
add_library(engine_lib_test STATIC
    src/engine/trading_engine.cpp
)
target_link_libraries(engine_lib_test PUBLIC
    common_lib pricing_lib risk_lib strategy_lib sim_gateway_lib logger_lib persistence_lib)

# ─── monitoring_lib (gRPC server) ─────────────────────────────────────────────
add_library(monitoring_lib STATIC
    "${OMM_PROTO_GEN_DIR}/trading.pb.cc"
    "${OMM_PROTO_GEN_DIR}/trading.grpc.pb.cc"
    src/monitoring/grpc_server.cpp
)
target_include_directories(monitoring_lib PUBLIC
    ${OMM_PROTO_GEN_DIR}
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

if(OMM_BUILD_TRADER_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
  add_executable(optionmm_trader_gui
      src/gui/main.cpp
      src/gui/trader_main_window.cpp
  )
  target_include_directories(optionmm_trader_gui PRIVATE
      ${OMM_PROTO_GEN_DIR}
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}
      ${Protobuf_INCLUDE_DIRS}
      ${GRPC_INCLUDE_DIRS}
  )
  target_link_libraries(optionmm_trader_gui PRIVATE
      monitoring_lib
      Qt6::Core
      Qt6::Gui
      Qt6::Widgets
      ${Protobuf_LIBRARIES}
      ${GRPC_LIBRARIES}
      Threads::Threads
  )
endif()

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
add_test(NAME test_black76 COMMAND test_black76)

# test_vol_surface
add_executable(test_vol_surface tests/test_vol_surface.cpp)
target_link_libraries(test_vol_surface PRIVATE pricing_lib GTest::gtest_main Threads::Threads)
add_test(NAME test_vol_surface COMMAND test_vol_surface)

# test_pre_trade_risk
add_executable(test_pre_trade_risk tests/test_pre_trade_risk.cpp)
target_link_libraries(test_pre_trade_risk PRIVATE risk_lib GTest::gtest_main)
add_test(NAME test_pre_trade_risk COMMAND test_pre_trade_risk)

# test_monitoring_topic
add_executable(test_monitoring_topic tests/test_monitoring_topic.cpp)
target_link_libraries(test_monitoring_topic PRIVATE GTest::gtest_main Threads::Threads)
add_test(NAME test_monitoring_topic COMMAND test_monitoring_topic)

# test_instrument_lookup
add_executable(test_instrument_lookup tests/test_instrument_lookup.cpp)
target_link_libraries(test_instrument_lookup PRIVATE GTest::gtest_main Threads::Threads)
add_test(NAME test_instrument_lookup COMMAND test_instrument_lookup)

# test_simple_mm (strategy + gateway + engine integration)
# Uses engine_lib_test which does not pull in CTP/FEMAS SDK shared libraries.
add_executable(test_simple_mm tests/test_simple_mm.cpp)
target_link_libraries(test_simple_mm PRIVATE engine_lib_test GTest::gtest_main Threads::Threads)
add_test(NAME test_simple_mm COMMAND test_simple_mm)

add_executable(test_option_mm_core tests/test_option_mm_core.cpp)
target_link_libraries(test_option_mm_core PRIVATE strategy_lib risk_lib common_lib GTest::gtest_main Threads::Threads)
add_test(NAME test_option_mm_core COMMAND test_option_mm_core)

add_executable(test_femas_simulator tests/test_femas_simulator.cpp)
target_link_libraries(test_femas_simulator PRIVATE femas_simulator_lib GTest::gtest_main Threads::Threads)
add_test(NAME test_femas_simulator COMMAND test_femas_simulator)

add_executable(test_pcp_arbitrage tests/test_pcp_arbitrage.cpp)
target_link_libraries(test_pcp_arbitrage PRIVATE strategy_lib risk_lib common_lib GTest::gtest_main Threads::Threads)
add_test(NAME test_pcp_arbitrage COMMAND test_pcp_arbitrage)

add_executable(test_data_repository tests/test_data_repository.cpp)
target_link_libraries(test_data_repository PRIVATE persistence_lib SQLite::SQLite3 GTest::gtest_main Threads::Threads)
add_test(NAME test_data_repository COMMAND test_data_repository)

# test_latency — NOT added to ctest (run explicitly for benchmarking)
# Build with RelWithDebInfo for realistic latency numbers.
add_executable(test_latency tests/test_latency.cpp)
target_link_libraries(test_latency PRIVATE engine_lib_test GTest::gtest_main Threads::Threads)
# Intentionally no add_test() — latency benchmarks must be run explicitly:
#   ./build/tests/test_latency --gtest_filter="LatencyTest.*"

# test_vol_calibration — reads MarketTick.csv, calibrates all 4 vol models, writes CSV results
# Run manually: ./build/test_vol_calibration
add_executable(test_vol_calibration tests/test_vol_calibration.cpp)
target_link_libraries(test_vol_calibration PRIVATE pricing_lib GTest::gtest_main)
# Intentionally no add_test() — depends on external CSV file path
