cmake_minimum_required(VERSION 3.22)
project(trade_bot
    VERSION 0.0.1
    DESCRIPTION "Trade Bot — hardcore C++23 implementation"
    LANGUAGES CXX)

# ═══════════════════════════════════════════════════════════════
# Compiler version guard
# ═══════════════════════════════════════════════════════════════
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13")
    message(FATAL_ERROR
        "GCC >= 13 required (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). "
        "Install gcc-13 or newer.")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17")
    message(FATAL_ERROR
        "Clang >= 17 required (found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}). "
        "Install clang-17 or newer.")
endif()

# ═══════════════════════════════════════════════════════════════
# Base settings
# ═══════════════════════════════════════════════════════════════
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ═══════════════════════════════════════════════════════════════
# Options
# ═══════════════════════════════════════════════════════════════
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks (micro-benchmarks only)" OFF)
option(ENABLE_AVX2 "Enable AVX2 optimizations" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# ═══════════════════════════════════════════════════════════════
# Compiler flags
# ═══════════════════════════════════════════════════════════════
if(MSVC)
    add_compile_options(/W4 /WX /permissive- /utf-8)
    if(ENABLE_AVX2)
        add_compile_options(/arch:AVX2)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    if(ENABLE_AVX2)
        add_compile_options(-mavx2)
    endif()
    if(ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
    # spdlog 1.13.0 conan package has no bundled fmt — use external fmt instead
    add_compile_options(-DSPDLOG_FMT_EXTERNAL)
    # OpenSSL 3.x + LTO triggers false-positive -Wfree-nonheap-object (GCC only)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-free-nonheap-object)
    endif()
endif()

# Release-specific optimisations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-march=native)
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ═══════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════
find_package(spdlog REQUIRED CONFIG)
find_package(hdr_histogram REQUIRED CONFIG)
# Make spdlog headers available globally for object libraries
include_directories(${spdlog_INCLUDE_DIRS})
find_package(xsimd REQUIRED CONFIG)
find_package(concurrentqueue REQUIRED CONFIG)
find_package(Boost REQUIRED CONFIG)
find_package(tomlplusplus REQUIRED CONFIG)
find_package(CURL REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(nlohmann_json_schema_validator REQUIRED CONFIG)
find_package(OpenSSL REQUIRED CONFIG)
find_package(absl REQUIRED CONFIG)
find_package(Python3 REQUIRED)
# Add include directories for all Conan packages globally to support object libraries
include_directories(
    ${spdlog_INCLUDE_DIRS}
    ${nlohmann_json_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${tomlplusplus_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${OpenSSL_INCLUDE_DIRS}
    ${absl_INCLUDE_DIRS}
)
# Add spdlog include path explicitly to resolve bundled fmt headers
include_directories(/home/yevge/.conan2/p/b/spdlo4958797c55550/p/include)
# Add fmt include directory for bundled fmt headers used by spdlog
include_directories(/home/yevge/.conan2/p/b/fmt73b066b56df8e/p/include)

# Workaround: Conan 2 openssl/3.6.2 recipe forgets to populate INTERFACE_INCLUDE_DIRECTORIES.
if(DEFINED openssl_PACKAGE_FOLDER_RELEASE)
    set_property(TARGET OpenSSL::SSL APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${openssl_PACKAGE_FOLDER_RELEASE}/include")
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${openssl_PACKAGE_FOLDER_RELEASE}/include")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ═══════════════════════════════════════════════════════════════
# Dashboard HTML pipeline
#   1. inline_dashboard.py  : modular sources → single bundled HTML
#      (Phase 0 of epic #236: passthrough copy; later phases will
#       inline HTML fragments / CSS chunks / JS modules.)
#   2. embed_html.py        : bundled HTML → C header (xxd replacement)
# ═══════════════════════════════════════════════════════════════
set(DASHBOARD_HTML_SRC      "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/index.html")
set(DASHBOARD_HTML_BUNDLED  "${CMAKE_CURRENT_BINARY_DIR}/dashboard.bundled.html")
set(DASHBOARD_HTML_H        "${CMAKE_CURRENT_BINARY_DIR}/dashboard_html.h")
set(INLINE_DASHBOARD_PY     "${CMAKE_CURRENT_SOURCE_DIR}/cmake/inline_dashboard.py")
set(EMBED_HTML_PY           "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_html.py")
file(GLOB DASHBOARD_STYLE_DEPS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/styles/*.css"
)
file(GLOB DASHBOARD_JS_DEPS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/js/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/js/core/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/js/panels/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/js/charts/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/js/transport/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/dashboard/js/app/*.js"
)

add_custom_command(
    OUTPUT ${DASHBOARD_HTML_BUNDLED}
    COMMAND ${Python3_EXECUTABLE} ${INLINE_DASHBOARD_PY} ${DASHBOARD_HTML_SRC} ${DASHBOARD_HTML_BUNDLED}
    DEPENDS ${DASHBOARD_HTML_SRC} ${INLINE_DASHBOARD_PY} ${DASHBOARD_STYLE_DEPS} ${DASHBOARD_JS_DEPS}
    COMMENT "Bundling dashboard → dashboard.bundled.html"
)

add_custom_command(
    OUTPUT ${DASHBOARD_HTML_H}
    COMMAND ${Python3_EXECUTABLE} ${EMBED_HTML_PY} ${DASHBOARD_HTML_BUNDLED} ${DASHBOARD_HTML_H}
    DEPENDS ${DASHBOARD_HTML_BUNDLED} ${EMBED_HTML_PY}
    COMMENT "Embedding dashboard.bundled.html → dashboard_html.h"
)

# ═══════════════════════════════════════════════════════════════
# Object libraries — compile once, reuse across 30+ targets
# ═══════════════════════════════════════════════════════════════
add_library(obj_logger OBJECT src/logger/Logger.cpp)
target_include_directories(obj_logger PUBLIC src)
target_link_libraries(obj_logger PUBLIC spdlog::spdlog)
# Ensure spdlog headers are available for the object library compilation
target_include_directories(obj_logger PRIVATE ${spdlog_INCLUDE_DIRS})

add_library(obj_simd_ops OBJECT src/perf/SimdOps.cpp)
target_include_directories(obj_simd_ops PUBLIC src)
target_link_libraries(obj_simd_ops PUBLIC xsimd)
# Provide spdlog include for any code that includes Logger.hpp
target_link_libraries(obj_simd_ops PUBLIC spdlog::spdlog)

add_library(obj_codec OBJECT src/transport/MetaScalpCodec.cpp)
target_include_directories(obj_codec PUBLIC src)
target_link_libraries(obj_codec PUBLIC nlohmann_json::nlohmann_json)
# Ensure spdlog headers are available for this object library
target_link_libraries(obj_codec PUBLIC spdlog::spdlog)

add_library(obj_signal_bus OBJECT src/signals/SignalBus.cpp)
target_include_directories(obj_signal_bus PUBLIC src)
target_link_libraries(obj_signal_bus PUBLIC nlohmann_json::nlohmann_json)
# Ensure spdlog headers are available for this object library
target_link_libraries(obj_signal_bus PUBLIC spdlog::spdlog)

add_library(obj_orderbook OBJECT src/marketdata/OrderBook.cpp)
target_include_directories(obj_orderbook PUBLIC src)
target_link_libraries(obj_orderbook PUBLIC absl::btree xsimd)

add_library(obj_trade_stream OBJECT src/marketdata/TradeStream.cpp)
target_include_directories(obj_trade_stream PUBLIC src)
target_link_libraries(obj_trade_stream PUBLIC xsimd)

add_library(obj_kalman_lag OBJECT src/marketdata/KalmanLagEstimator.cpp)
target_include_directories(obj_kalman_lag PUBLIC src)
target_link_libraries(obj_kalman_lag PUBLIC boost::boost)

add_library(obj_leader_tracker OBJECT src/marketdata/LeaderTracker.cpp)
target_include_directories(obj_leader_tracker PUBLIC src)
target_link_libraries(obj_leader_tracker PUBLIC boost::boost)

add_library(obj_ticker_universe OBJECT src/universe/TickerUniverse.cpp)
target_include_directories(obj_ticker_universe PUBLIC src)

add_library(obj_universe_filters OBJECT src/universe/UniverseFilters.cpp)
target_include_directories(obj_universe_filters PUBLIC src)
target_link_libraries(obj_universe_filters PUBLIC spdlog::spdlog)

add_library(obj_metrics_registry OBJECT src/metrics/MetricsRegistry.cpp)
target_include_directories(obj_metrics_registry PUBLIC src)

add_library(obj_config OBJECT src/config/Config.cpp)
target_include_directories(obj_config PUBLIC src)
target_link_libraries(obj_config PUBLIC tomlplusplus::tomlplusplus spdlog::spdlog)

# ═══════════════════════════════════════════════════════════════
# Main executable
# ═══════════════════════════════════════════════════════════════
add_executable(trade_bot
    src/main.cpp
    src/perf/CpuPinning.cpp
    src/perf/TimestampMonitor.cpp
    src/transport/CurlHttpClient.cpp
    src/transport/MetaScalpDiscovery.cpp
    src/transport/BeastWsClient.cpp
    src/transport/MarketDataFeed.cpp
    src/transport/DumpRecorder.cpp
    src/transport/OrderGateway.cpp
    src/transport/ReplayFeed.cpp
    src/transport/FinresHandler.cpp
    src/transport/NotificationFeed.cpp
    src/transport/ClusterSnapshotClient.cpp
    src/trading/OrderReconciliator.cpp
    src/control/KillSwitch.cpp
    src/control/ClockDriftMonitor.cpp
    src/control/SntpClient.cpp
    src/control/DashboardServer.cpp
    src/risk/NewsCalendar.cpp
    src/risk/RiskManager.cpp
    src/risk/AccountStatePersister.cpp
    src/features/FeatureExtractor.cpp
    src/universe/ConnectionSelector.cpp
    src/universe/OrderbookSettingsLoader.cpp
    src/marketdata/ClusterSnapshot.cpp
    src/strategy/StrategyEngine.cpp
    src/strategy/BounceFromDensity.cpp
    src/strategy/BreakoutEatThrough.cpp
    src/strategy/LeaderLag.cpp
    src/executor/PaperExecutor.cpp
    src/executor/LiveExecutor.cpp
    src/executor/StartupRecovery.cpp
    src/logger/TradeJournal.cpp
    src/signals/DensityDetector.cpp
    src/signals/IcebergDetector.cpp
    src/signals/TapeAnalyzer.cpp
    src/signals/LevelDetector.cpp
    src/signals/ApproachAnalyzer.cpp
    src/signals/LeaderSignal.cpp
    src/metrics/MetricsExporter.cpp
    src/metrics/AlertWebhook.cpp
)

# DashboardServer.cpp depends on generated dashboard_html.h
set_source_files_properties(src/control/DashboardServer.cpp PROPERTIES
    OBJECT_DEPENDS ${DASHBOARD_HTML_H})

target_include_directories(trade_bot PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(trade_bot PRIVATE
    obj_logger obj_simd_ops obj_codec obj_signal_bus obj_config
    obj_orderbook obj_trade_stream obj_kalman_lag obj_leader_tracker
    obj_ticker_universe obj_universe_filters obj_metrics_registry
    spdlog::spdlog
    xsimd
    hdr_histogram::hdr_histogram_static
    boost::boost
    concurrentqueue::concurrentqueue
    tomlplusplus::tomlplusplus
    CURL::libcurl
    nlohmann_json::nlohmann_json
    nlohmann_json_schema_validator
    OpenSSL::SSL
    OpenSSL::Crypto
    absl::btree
    absl::flat_hash_map
    absl::hash
)

# ═══════════════════════════════════════════════════════════════
# Labeler tool
# ═══════════════════════════════════════════════════════════════
add_executable(labeler
    tool/labeler/main.cpp
    src/transport/CurlHttpClient.cpp
    src/transport/ClusterSnapshotClient.cpp
    src/marketdata/ClusterSnapshot.cpp
    src/features/FeatureExtractor.cpp
    src/signals/DensityDetector.cpp
    src/signals/IcebergDetector.cpp
    src/signals/TapeAnalyzer.cpp
    src/signals/LevelDetector.cpp
    src/signals/ApproachAnalyzer.cpp
    src/universe/ConnectionSelector.cpp
    src/universe/OrderbookSettingsLoader.cpp
    src/perf/TimestampMonitor.cpp
    src/perf/CpuPinning.cpp) 
target_link_libraries(labeler PRIVATE
    obj_logger obj_simd_ops obj_codec obj_signal_bus
    obj_orderbook obj_trade_stream obj_kalman_lag obj_leader_tracker
    obj_ticker_universe obj_universe_filters obj_metrics_registry obj_config
    spdlog::spdlog
    xsimd
    hdr_histogram::hdr_histogram_static
    boost::boost
    concurrentqueue::concurrentqueue
    tomlplusplus::tomlplusplus
    CURL::libcurl
    nlohmann_json::nlohmann_json
    nlohmann_json_schema_validator
    OpenSSL::SSL
    OpenSSL::Crypto
    absl::btree
    absl::flat_hash_map
    absl::hash
)

# ═══════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED CONFIG)

    # ── smoke ──────────────────────────────────────────────
    add_executable(smoke_test test/unit/smoke_test.cpp)
    target_link_libraries(smoke_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME smoke COMMAND smoke_test)

    # ── logger ─────────────────────────────────────────────
    add_executable(logger_test test/unit/logger_test.cpp)
    target_link_libraries(logger_test PRIVATE obj_logger GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME logger COMMAND logger_test)

    # ── numeric ────────────────────────────────────────────
    add_executable(numeric_test test/unit/numeric_test.cpp)
    target_include_directories(numeric_test PRIVATE src)
    target_link_libraries(numeric_test PRIVATE GTest::gtest GTest::gtest_main hdr_histogram::hdr_histogram_static)
    add_test(NAME numeric COMMAND numeric_test)

    # ── perf ───────────────────────────────────────────────
    add_executable(perf_test
        test/unit/perf_test.cpp
        src/perf/CpuPinning.cpp
    )
    target_link_libraries(perf_test PRIVATE
        obj_logger obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog xsimd boost::boost concurrentqueue::concurrentqueue)
    add_test(NAME perf COMMAND perf_test)

    # ── config ─────────────────────────────────────────────
    add_executable(config_test test/unit/config_test.cpp)
    target_link_libraries(config_test PRIVATE
        obj_logger obj_config
        GTest::gtest GTest::gtest_main spdlog::spdlog tomlplusplus::tomlplusplus)
    add_test(NAME config COMMAND config_test)

    # ── http_client ────────────────────────────────────────
    add_executable(http_client_test
        test/unit/http_client_test.cpp
        src/transport/CurlHttpClient.cpp
    )
    target_link_libraries(http_client_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl boost::boost)
    add_test(NAME http_client COMMAND http_client_test)

    # ── discover ───────────────────────────────────────────
    add_executable(discover_test
        test/unit/discover_test.cpp
        src/transport/MetaScalpDiscovery.cpp
        src/transport/CurlHttpClient.cpp
    )
    target_link_libraries(discover_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME discover COMMAND discover_test)

    # ── domain ─────────────────────────────────────────────
    add_executable(domain_test test/unit/domain_test.cpp)
    target_include_directories(domain_test PRIVATE src)
    target_link_libraries(domain_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME domain COMMAND domain_test)

    # ── ws ─────────────────────────────────────────────────
    add_executable(ws_test
        test/unit/ws_test.cpp
        src/transport/BeastWsClient.cpp
    )
    target_link_libraries(ws_test PRIVATE
        obj_logger obj_metrics_registry
        GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME ws COMMAND ws_test)

    # ── ws_smoke ───────────────────────────────────────────
    add_executable(ws_smoke_test
        test/integration/ws_smoke_test.cpp
        src/transport/BeastWsClient.cpp
    )
    target_link_libraries(ws_smoke_test PRIVATE
        obj_logger obj_metrics_registry
        GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME ws_smoke COMMAND ws_smoke_test)

    # ── codec ──────────────────────────────────────────────
    add_executable(codec_test test/unit/codec_test.cpp)
    target_link_libraries(codec_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json)
    add_test(NAME codec COMMAND codec_test)

    # ── feed ───────────────────────────────────────────────
    add_executable(feed_test
        test/unit/feed_test.cpp
        src/transport/MarketDataFeed.cpp
    )
    target_link_libraries(feed_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME feed COMMAND feed_test)

    # ── feed_smoke ─────────────────────────────────────────
    add_executable(feed_smoke_test
        test/integration/feed_smoke_test.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/BeastWsClient.cpp
    )
    target_link_libraries(feed_smoke_test PRIVATE
        obj_logger obj_codec obj_metrics_registry
        GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME feed_smoke COMMAND feed_smoke_test)

    # ── order_gateway ──────────────────────────────────────
    add_executable(order_gateway_test
        test/unit/order_gateway_test.cpp
        src/transport/OrderGateway.cpp
    )
    target_link_libraries(order_gateway_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME order_gateway COMMAND order_gateway_test)

    # ── timestamp_monitor ──────────────────────────────────
    add_executable(timestamp_monitor_test
        test/unit/timestamp_monitor_test.cpp
        src/perf/TimestampMonitor.cpp
        src/transport/MarketDataFeed.cpp
    )
    target_link_libraries(timestamp_monitor_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME timestamp_monitor COMMAND timestamp_monitor_test)

    # ── timestamp_jitter ───────────────────────────────────
    add_executable(timestamp_jitter_test
        test/integration/timestamp_jitter_test.cpp
        src/perf/TimestampMonitor.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/BeastWsClient.cpp
        src/transport/CurlHttpClient.cpp
        src/transport/MetaScalpDiscovery.cpp
    )
    target_link_libraries(timestamp_jitter_test PRIVATE
        obj_logger obj_codec obj_metrics_registry
        GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static nlohmann_json::nlohmann_json boost::boost CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME timestamp_jitter COMMAND timestamp_jitter_test)

    # ── order_reconciliator ────────────────────────────────
    add_executable(order_reconciliator_test
        test/unit/order_reconciliator_test.cpp
        src/trading/OrderReconciliator.cpp
    )
    target_link_libraries(order_reconciliator_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME order_reconciliator COMMAND order_reconciliator_test)

    # ── replay ─────────────────────────────────────────────
    add_executable(replay_test
        test/unit/replay_test.cpp
        src/transport/ReplayFeed.cpp
        src/transport/MarketDataFeed.cpp
    )
    target_link_libraries(replay_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME replay COMMAND replay_test)

    # ── killswitch ─────────────────────────────────────────
    add_executable(killswitch_test
        test/unit/killswitch_test.cpp
        src/control/KillSwitch.cpp
    )
    target_link_libraries(killswitch_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME killswitch COMMAND killswitch_test)

    # ── clock_drift_monitor ────────────────────────────────
    add_executable(clock_drift_monitor_test
        test/unit/clock_drift_monitor_test.cpp
        src/control/ClockDriftMonitor.cpp
    )
    target_link_libraries(clock_drift_monitor_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME clock_drift_monitor COMMAND clock_drift_monitor_test)

    # ── news_calendar ──────────────────────────────────────
    add_executable(news_calendar_test
        test/unit/news_calendar_test.cpp
        src/risk/NewsCalendar.cpp
    )
    target_link_libraries(news_calendar_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator)
    add_test(NAME news_calendar COMMAND news_calendar_test)

    # ── orderbook ──────────────────────────────────────────
    add_executable(orderbook_test test/unit/orderbook_test.cpp)
    target_link_libraries(orderbook_test PRIVATE
        obj_logger obj_orderbook obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree xsimd)
    add_test(NAME orderbook COMMAND orderbook_test)

    # ── tradestream ────────────────────────────────────────
    add_executable(tradestream_test test/unit/tradestream_test.cpp)
    target_link_libraries(tradestream_test PRIVATE
        obj_logger obj_trade_stream obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog xsimd)
    add_test(NAME tradestream COMMAND tradestream_test)

    # ── cluster_snapshot_codec ─────────────────────────────
    add_executable(cluster_snapshot_codec_test test/unit/cluster_snapshot_codec_test.cpp)
    target_link_libraries(cluster_snapshot_codec_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME cluster_snapshot_codec COMMAND cluster_snapshot_codec_test)

    # ── cluster_snapshot_manager ───────────────────────────
    add_executable(cluster_snapshot_manager_test
        test/unit/cluster_snapshot_manager_test.cpp
        src/marketdata/ClusterSnapshot.cpp
        src/transport/ClusterSnapshotClient.cpp
    )
    target_link_libraries(cluster_snapshot_manager_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME cluster_snapshot_manager COMMAND cluster_snapshot_manager_test)

    # ── notification_routing ───────────────────────────────
    add_executable(notification_routing_test
        test/unit/notification_routing_test.cpp
        src/transport/NotificationFeed.cpp
    )
    target_link_libraries(notification_routing_test PRIVATE
        obj_logger obj_codec obj_ticker_universe obj_universe_filters obj_metrics_registry
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json)
    add_test(NAME notification_routing COMMAND notification_routing_test)

    # ── orderbook_settings_loader ──────────────────────────
    add_executable(orderbook_settings_loader_test
        test/unit/orderbook_settings_loader_test.cpp
        src/universe/OrderbookSettingsLoader.cpp
    )
    target_link_libraries(orderbook_settings_loader_test PRIVATE
        obj_logger obj_codec obj_ticker_universe obj_universe_filters
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json)
    add_test(NAME orderbook_settings_loader COMMAND orderbook_settings_loader_test)

    # ── signal_bus ─────────────────────────────────────────
    add_executable(signal_bus_test test/unit/signal_bus_test.cpp)
    target_link_libraries(signal_bus_test PRIVATE
        obj_signal_bus
        GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    add_test(NAME signal_bus COMMAND signal_bus_test)

    # ── density_detector ───────────────────────────────────
    add_executable(density_detector_test
        test/unit/density_detector_test.cpp
        src/signals/DensityDetector.cpp
    )
    target_link_libraries(density_detector_test PRIVATE
        obj_logger obj_signal_bus obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
    add_test(NAME density_detector COMMAND density_detector_test)

    # ── iceberg_bayesian ───────────────────────────────────
    add_executable(iceberg_bayesian_test
        test/unit/iceberg_bayesian_test.cpp
        src/signals/IcebergDetector.cpp
    )
    target_link_libraries(iceberg_bayesian_test PRIVATE
        obj_logger obj_signal_bus obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
    add_test(NAME iceberg_bayesian COMMAND iceberg_bayesian_test)

    # ── iceberg_detector ───────────────────────────────────
    add_executable(iceberg_detector_test
        test/unit/iceberg_detector_test.cpp
        src/signals/IcebergDetector.cpp
    )
    target_link_libraries(iceberg_detector_test PRIVATE
        obj_logger obj_signal_bus obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
    add_test(NAME iceberg_detector COMMAND iceberg_detector_test)

    # ── tape_analyzer ──────────────────────────────────────
    add_executable(tape_analyzer_test
        test/unit/tape_analyzer_test.cpp
        src/signals/TapeAnalyzer.cpp
    )
    target_link_libraries(tape_analyzer_test PRIVATE
        obj_logger obj_signal_bus obj_trade_stream obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
    add_test(NAME tape_analyzer COMMAND tape_analyzer_test)

    # ── level_dbscan ───────────────────────────────────────
    add_executable(level_dbscan_test test/unit/level_dbscan_test.cpp)
    target_include_directories(level_dbscan_test PRIVATE src)
    target_link_libraries(level_dbscan_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME level_dbscan COMMAND level_dbscan_test)

    # ── level_kde ──────────────────────────────────────────
    add_executable(level_kde_test test/unit/level_kde_test.cpp)
    target_include_directories(level_kde_test PRIVATE src)
    target_link_libraries(level_kde_test PRIVATE GTest::gtest GTest::gtest_main xsimd)
    add_test(NAME level_kde COMMAND level_kde_test)

    # ── level_detector ─────────────────────────────────────
    add_executable(level_detector_test
        test/unit/level_detector_test.cpp
        src/signals/LevelDetector.cpp
        src/signals/ApproachAnalyzer.cpp
        src/marketdata/ClusterSnapshot.cpp
        src/transport/ClusterSnapshotClient.cpp
    )
    target_link_libraries(level_detector_test PRIVATE
        obj_logger obj_signal_bus obj_orderbook obj_codec obj_ticker_universe obj_universe_filters obj_simd_ops
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME level_detector COMMAND level_detector_test)

    # ── approach_zigzag ────────────────────────────────────
    add_executable(approach_zigzag_test test/unit/approach_zigzag_test.cpp)
    target_include_directories(approach_zigzag_test PRIVATE src)
    target_link_libraries(approach_zigzag_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME approach_zigzag COMMAND approach_zigzag_test)

    # ── approach_hmm_forward ───────────────────────────────
    add_executable(approach_hmm_forward_test test/unit/approach_hmm_forward_test.cpp)
    target_include_directories(approach_hmm_forward_test PRIVATE src)
    target_link_libraries(approach_hmm_forward_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME approach_hmm_forward COMMAND approach_hmm_forward_test)

    # ── approach_analyzer ──────────────────────────────────
    add_executable(approach_analyzer_test
        test/unit/approach_analyzer_test.cpp
        src/signals/ApproachAnalyzer.cpp
        src/signals/LevelDetector.cpp
        src/marketdata/ClusterSnapshot.cpp
        src/transport/ClusterSnapshotClient.cpp
    )
    target_link_libraries(approach_analyzer_test PRIVATE
        obj_logger obj_signal_bus obj_orderbook obj_codec obj_ticker_universe obj_universe_filters obj_simd_ops
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME approach_analyzer COMMAND approach_analyzer_test)

    # ── leader_signal ──────────────────────────────────────
    add_executable(leader_signal_test
        test/unit/leader_signal_test.cpp
        src/signals/LeaderSignal.cpp
    )
    target_link_libraries(leader_signal_test PRIVATE
        obj_logger obj_signal_bus obj_leader_tracker obj_kalman_lag
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME leader_signal COMMAND leader_signal_test)

    # ── strategy_engine ────────────────────────────────────
    add_executable(strategy_engine_test
        test/unit/strategy_engine_test.cpp
        src/strategy/StrategyEngine.cpp
    )
    target_link_libraries(strategy_engine_test PRIVATE
        obj_logger obj_signal_bus
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME strategy_engine COMMAND strategy_engine_test)

    # ── bounce_strategy ────────────────────────────────────
    add_executable(bounce_strategy_test
        test/unit/bounce_strategy_test.cpp
        src/strategy/BounceFromDensity.cpp
    )
    target_link_libraries(bounce_strategy_test PRIVATE
        obj_logger obj_signal_bus
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME bounce_strategy COMMAND bounce_strategy_test)

    # ── breakout_strategy ──────────────────────────────────
    add_executable(breakout_strategy_test
        test/unit/breakout_strategy_test.cpp
        src/strategy/BreakoutEatThrough.cpp
    )
    target_link_libraries(breakout_strategy_test PRIVATE
        obj_logger obj_signal_bus
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME breakout_strategy COMMAND breakout_strategy_test)

    # ── leaderlag_strategy ─────────────────────────────────
    add_executable(leaderlag_strategy_test
        test/unit/leaderlag_strategy_test.cpp
        src/strategy/LeaderLag.cpp
    )
    target_link_libraries(leaderlag_strategy_test PRIVATE
        obj_logger obj_signal_bus
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME leaderlag_strategy COMMAND leaderlag_strategy_test)

    # ── signal_level_bridge ────────────────────────────────
    add_executable(signal_level_bridge_test
        test/unit/signal_level_bridge_test.cpp
        src/signals/SignalLevelBridge.cpp
        src/transport/SignalLevelGateway.cpp
    )
    target_link_libraries(signal_level_bridge_test PRIVATE
        obj_logger obj_codec obj_signal_bus
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog absl::btree xsimd nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME signal_level_bridge COMMAND signal_level_bridge_test)

    # ── risk_manager ───────────────────────────────────────
    add_executable(risk_manager_test
        test/unit/risk_manager_test.cpp
        src/risk/RiskManager.cpp
        src/risk/NewsCalendar.cpp
    )
    target_link_libraries(risk_manager_test PRIVATE
        obj_logger obj_ticker_universe obj_universe_filters
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator boost::boost)
    add_test(NAME risk_manager COMMAND risk_manager_test)

    # ── r13_funding ────────────────────────────────────────
    add_executable(r13_funding_test
        test/unit/r13_funding_test.cpp
        src/risk/RiskManager.cpp
        src/risk/NewsCalendar.cpp
    )
    target_link_libraries(r13_funding_test PRIVATE
        obj_logger obj_ticker_universe obj_universe_filters
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator boost::boost)
    add_test(NAME r13_funding COMMAND r13_funding_test)

    # ── trading_day_reset ──────────────────────────────────
    add_executable(trading_day_reset_test
        test/unit/trading_day_reset_idempotent_test.cpp
        src/risk/AccountStatePersister.cpp
    )
    target_link_libraries(trading_day_reset_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME trading_day_reset COMMAND trading_day_reset_test)

    # ── risk_flow ──────────────────────────────────────────
    add_executable(risk_flow_test
        test/integration/risk_flow_test.cpp
        src/risk/RiskManager.cpp
        src/risk/NewsCalendar.cpp
    )
    target_link_libraries(risk_flow_test PRIVATE
        obj_logger obj_ticker_universe obj_universe_filters
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator)
    add_test(NAME risk_flow COMMAND risk_flow_test)

    # ── state_persistence ──────────────────────────────────
    add_executable(state_persistence_test
        test/integration/state_persistence_test.cpp
        src/risk/AccountStatePersister.cpp
    )
    target_link_libraries(state_persistence_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json)
    add_test(NAME state_persistence COMMAND state_persistence_test)

    # ── metrics ────────────────────────────────────────────
    add_executable(metrics_test test/unit/metrics_test.cpp)
    target_link_libraries(metrics_test PRIVATE
        obj_metrics_registry
        GTest::gtest GTest::gtest_main)
    add_test(NAME metrics COMMAND metrics_test)

    # ── metrics_exporter ───────────────────────────────────
    add_executable(metrics_exporter_test
        test/unit/metrics_exporter_test.cpp
        src/metrics/MetricsExporter.cpp
        src/transport/CurlHttpClient.cpp
    )
    target_link_libraries(metrics_exporter_test PRIVATE
        obj_logger obj_metrics_registry
        GTest::gtest GTest::gtest_main CURL::libcurl spdlog::spdlog boost::boost)
    add_test(NAME metrics_exporter COMMAND metrics_exporter_test)

    # ── startup_recovery ───────────────────────────────────
    add_executable(startup_recovery_test
        test/unit/startup_recovery_test.cpp
        src/executor/StartupRecovery.cpp
        src/risk/AccountStatePersister.cpp
        src/transport/OrderGateway.cpp
    )
    target_link_libraries(startup_recovery_test PRIVATE
        obj_logger obj_codec
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json CURL::libcurl)
    add_test(NAME startup_recovery COMMAND startup_recovery_test)

    # ── executor_state_machine ─────────────────────────────
    add_executable(executor_state_machine_test
        test/unit/executor_state_machine_test.cpp
        src/executor/LiveExecutor.cpp
        src/trading/OrderReconciliator.cpp
        src/transport/OrderGateway.cpp
        src/transport/MarketDataFeed.cpp
    )
    target_link_libraries(executor_state_machine_test PRIVATE
        obj_logger obj_codec obj_metrics_registry
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json CURL::libcurl)
    add_test(NAME executor_state_machine COMMAND executor_state_machine_test)

    # ── stop_contract ──────────────────────────────────────
    add_executable(stop_contract_test
        test/contract/stop_contract_test.cpp
        src/transport/OrderGateway.cpp
        src/transport/MetaScalpDiscovery.cpp
        src/transport/CurlHttpClient.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/BeastWsClient.cpp
    )
    target_link_libraries(stop_contract_test PRIVATE
        obj_logger obj_codec obj_metrics_registry
        GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json CURL::libcurl boost::boost OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME stop_contract COMMAND stop_contract_test)

    # ── leader_correlation_welford ─────────────────────────
    add_executable(leader_correlation_welford_test test/unit/leader_correlation_welford_test.cpp)
    target_link_libraries(leader_correlation_welford_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME leader_correlation_welford COMMAND leader_correlation_welford_test)

    # ── kalman_lag_estimator ───────────────────────────────
    add_executable(kalman_lag_estimator_test test/unit/kalman_lag_estimator_test.cpp)
    target_link_libraries(kalman_lag_estimator_test PRIVATE
        obj_logger obj_kalman_lag
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME kalman_lag_estimator COMMAND kalman_lag_estimator_test)

    # ── leader ─────────────────────────────────────────────
    add_executable(leader_test test/unit/leader_test.cpp)
    target_link_libraries(leader_test PRIVATE
        obj_logger obj_leader_tracker obj_kalman_lag
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME leader COMMAND leader_test)

    # ── leader_cusum_decorrelation ─────────────────────────
    add_executable(leader_cusum_decorrelation_test test/unit/leader_cusum_decorrelation_test.cpp)
    target_link_libraries(leader_cusum_decorrelation_test PRIVATE
        obj_logger obj_leader_tracker obj_kalman_lag
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME leader_cusum_decorrelation COMMAND leader_cusum_decorrelation_test)

    # ── feature_extractor ──────────────────────────────────
    add_executable(feature_extractor_test
        test/unit/feature_extractor_test.cpp
        src/features/FeatureExtractor.cpp
    )
    target_link_libraries(feature_extractor_test PRIVATE
        obj_logger obj_orderbook obj_trade_stream obj_leader_tracker obj_kalman_lag obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static absl::btree xsimd)
    add_test(NAME feature_extractor COMMAND feature_extractor_test)

    # ── feature_welford ────────────────────────────────────
    add_executable(feature_welford_test
        test/unit/feature_welford_test.cpp
        src/features/FeatureExtractor.cpp
    )
    target_link_libraries(feature_welford_test PRIVATE
        obj_logger obj_orderbook obj_trade_stream obj_leader_tracker obj_kalman_lag obj_simd_ops
        GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static absl::btree xsimd)
    add_test(NAME feature_welford COMMAND feature_welford_test)

    # ── universe_filters ───────────────────────────────────
    add_executable(universe_filters_test test/unit/universe_filters_test.cpp)
    target_link_libraries(universe_filters_test PRIVATE
        obj_logger obj_universe_filters
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME universe_filters COMMAND universe_filters_test)

    # ── connection_selector ────────────────────────────────
    add_executable(connection_selector_test
        test/unit/connection_selector_test.cpp
        src/universe/ConnectionSelector.cpp
    )
    target_link_libraries(connection_selector_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME connection_selector COMMAND connection_selector_test)

    # ── ticker_universe_pool ───────────────────────────────
    add_executable(ticker_universe_pool_test test/unit/ticker_universe_pool_test.cpp)
    target_link_libraries(ticker_universe_pool_test PRIVATE
        obj_logger obj_ticker_universe obj_universe_filters
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME ticker_universe_pool COMMAND ticker_universe_pool_test)

    # ── ticker_universe_affinity ───────────────────────────
    add_executable(ticker_universe_affinity_test test/unit/ticker_universe_affinity_test.cpp)
    target_link_libraries(ticker_universe_affinity_test PRIVATE
        obj_logger obj_ticker_universe obj_universe_filters
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME ticker_universe_affinity COMMAND ticker_universe_affinity_test)

    # ── dashboard_server ───────────────────────────────────
    add_executable(dashboard_server_test
        test/unit/dashboard_server_test.cpp
        src/control/DashboardServer.cpp
        src/transport/DumpRecorder.cpp
    )
    # DS-11.5: DashboardServer.cpp includes generated dashboard_html.h
    set_source_files_properties(src/control/DashboardServer.cpp PROPERTIES
        OBJECT_DEPENDS ${DASHBOARD_HTML_H})
    target_include_directories(dashboard_server_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(dashboard_server_test PRIVATE
        obj_logger
        GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME dashboard_server COMMAND dashboard_server_test)
endif()

# ═══════════════════════════════════════════════════════════════
# Benchmarks (separate from tests, controlled by BUILD_BENCHMARKS)
# ═══════════════════════════════════════════════════════════════
if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED CONFIG)

    add_executable(bench_perf
        bench/bench_perf.cpp
    )
    target_link_libraries(bench_perf PRIVATE obj_simd_ops benchmark::benchmark xsimd)

    add_executable(bench_orderbook_apply
        bench/bench_orderbook_apply.cpp
    )
    target_link_libraries(bench_orderbook_apply PRIVATE
        obj_logger obj_orderbook obj_simd_ops
        benchmark::benchmark xsimd absl::btree spdlog::spdlog)

    add_executable(bench_orderbook_depth
        bench/bench_orderbook_depth.cpp
    )
    target_link_libraries(bench_orderbook_depth PRIVATE
        obj_logger obj_orderbook obj_simd_ops
        benchmark::benchmark xsimd absl::btree spdlog::spdlog)

    add_executable(bench_tradestream_on_trade
        bench/bench_tradestream_on_trade.cpp
    )
    target_link_libraries(bench_tradestream_on_trade PRIVATE
        obj_logger obj_trade_stream obj_simd_ops
        benchmark::benchmark xsimd spdlog::spdlog)
endif()

# ═══════════════════════════════════════════════════════════════
# Status
# ═══════════════════════════════════════════════════════════════
message(STATUS "Project:    ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ Std:    ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:   ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "AVX2:       ${ENABLE_AVX2}")
message(STATUS "Sanitizers: ${ENABLE_SANITIZERS}")
message(STATUS "LTO:        ${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
message(STATUS "Tests:      ${BUILD_TESTS}")
message(STATUS "Benchmarks: ${BUILD_BENCHMARKS}")
