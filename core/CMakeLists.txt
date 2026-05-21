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
    add_compile_options(-Wall -Wextra -Werror -Wno-error=deprecated-literal-operator)
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
find_package(fmt REQUIRED CONFIG)
find_package(flatbuffers REQUIRED CONFIG)
find_package(simdjson REQUIRED CONFIG)
find_package(Python3 REQUIRED)
# Add include directories for all Conan packages globally to support object libraries
include_directories(
    ${spdlog_INCLUDE_DIRS}
    ${fmt_INCLUDE_DIRS}
    ${nlohmann_json_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${tomlplusplus_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${OpenSSL_INCLUDE_DIRS}
    ${absl_INCLUDE_DIRS}
)

# Workaround: Conan 2 openssl/3.6.2 recipe forgets to populate INTERFACE_INCLUDE_DIRECTORIES.
if(DEFINED openssl_PACKAGE_FOLDER_RELEASE)
    set_property(TARGET OpenSSL::SSL APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${openssl_PACKAGE_FOLDER_RELEASE}/include")
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${openssl_PACKAGE_FOLDER_RELEASE}/include")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# ═══════════════════════════════════════════════════════════════
# Object libraries — compile once, reuse across 30+ targets
# ═══════════════════════════════════════════════════════════════
add_library(obj_logger OBJECT src/logger/Logger.cpp)
target_include_directories(obj_logger PUBLIC src)
target_link_libraries(obj_logger PUBLIC spdlog::spdlog fmt::fmt hdr_histogram::hdr_histogram_static)
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
# Signal.hpp → LatencyTracer.hpp → HdrHistogramWrapper.hpp needs hdr_histogram include path
target_link_libraries(obj_signal_bus PUBLIC hdr_histogram::hdr_histogram_static)

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

add_library(obj_perf_registry OBJECT src/perf/PerfRegistry.cpp src/perf/TraceId.cpp)
target_include_directories(obj_perf_registry PUBLIC src)
target_link_libraries(obj_perf_registry PUBLIC
    hdr_histogram::hdr_histogram_static
    absl::flat_hash_map
    spdlog::spdlog)

# ═══════════════════════════════════════════════════════════════
# Main executable
# ═══════════════════════════════════════════════════════════════
add_executable(trade_bot
    src/main.cpp
    src/app/BotAppInit.cpp
    src/app/BotAppRun.cpp
    src/app/PipelineProcessor.cpp
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
    src/control/DashboardServerBinary.cpp
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
    src/strategy/FlushReversal.cpp
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
    src/signals/SignalLevelBridge.cpp
    src/transport/SignalLevelGateway.cpp
    src/metrics/MetricsExporter.cpp
    src/metrics/AlertWebhook.cpp
)

target_include_directories(trade_bot PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(trade_bot PRIVATE
    obj_logger obj_simd_ops obj_codec obj_signal_bus obj_config
    obj_orderbook obj_trade_stream obj_kalman_lag obj_leader_tracker
    obj_ticker_universe obj_universe_filters obj_metrics_registry obj_perf_registry
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
    flatbuffers::flatbuffers
    simdjson::simdjson
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
    obj_logger obj_simd_ops obj_codec obj_signal_bus obj_perf_registry
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
# perf_replay tool — offline pipeline latency profiler
# ═══════════════════════════════════════════════════════════════
add_executable(perf_replay
    tool/perf_replay/main.cpp
    src/transport/CurlHttpClient.cpp
    src/transport/ClusterSnapshotClient.cpp
    src/transport/ReplayFeed.cpp
    src/marketdata/ClusterSnapshot.cpp
    src/features/FeatureExtractor.cpp
    src/signals/DensityDetector.cpp
    src/signals/IcebergDetector.cpp
    src/signals/TapeAnalyzer.cpp
    src/signals/LevelDetector.cpp
    src/signals/ApproachAnalyzer.cpp
    src/signals/LeaderSignal.cpp
    src/strategy/StrategyEngine.cpp
    src/strategy/BounceFromDensity.cpp
    src/strategy/BreakoutEatThrough.cpp
    src/strategy/LeaderLag.cpp
    src/strategy/FlushReversal.cpp
    src/risk/RiskManager.cpp
    src/risk/NewsCalendar.cpp
    src/executor/PaperExecutor.cpp
    src/logger/TradeJournal.cpp
    src/universe/ConnectionSelector.cpp
    src/universe/OrderbookSettingsLoader.cpp
    src/perf/TimestampMonitor.cpp
    src/perf/CpuPinning.cpp)
target_link_libraries(perf_replay PRIVATE
    obj_logger obj_simd_ops obj_codec obj_signal_bus obj_perf_registry
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
# core_probe tool — AI-friendly diagnostic CLI for pipeline tracing
# ═══════════════════════════════════════════════════════════════
# Resolve git SHA for --version output (best-effort, falls back to "unknown").
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE CORE_PROBE_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CORE_PROBE_GIT_SHA)
    set(CORE_PROBE_GIT_SHA "unknown")
endif()

add_executable(core_probe
    tool/core_probe/main.cpp
    tool/core_probe/CliOptions.cpp
    tool/core_probe/TraceLogger.cpp
    tool/core_probe/TraceFormatter.cpp
    tool/core_probe/SummaryCollector.cpp
    tool/core_probe/InvariantChecker.cpp
    tool/core_probe/ProbePipeline.cpp
    tool/core_probe/ReplayRunner.cpp
    tool/core_probe/DiffRunner.cpp
    tool/core_probe/AssertRunner.cpp
    tool/core_probe/SchemaRunner.cpp
    tool/core_probe/AiRecipeRunner.cpp
    tool/core_probe/SyntheticFeed.cpp
    tool/core_probe/SynthRunner.cpp
    tool/core_probe/LiveRunner.cpp
    src/transport/CurlHttpClient.cpp
    src/transport/ClusterSnapshotClient.cpp
    src/transport/ReplayFeed.cpp
    src/transport/BeastWsClient.cpp
    src/transport/MarketDataFeed.cpp
    src/marketdata/ClusterSnapshot.cpp
    src/features/FeatureExtractor.cpp
    src/signals/DensityDetector.cpp
    src/signals/IcebergDetector.cpp
    src/signals/TapeAnalyzer.cpp
    src/signals/LevelDetector.cpp
    src/signals/ApproachAnalyzer.cpp
    src/signals/LeaderSignal.cpp
    src/strategy/StrategyEngine.cpp
    src/strategy/BounceFromDensity.cpp
    src/strategy/BreakoutEatThrough.cpp
    src/strategy/LeaderLag.cpp
    src/strategy/FlushReversal.cpp
    src/risk/RiskManager.cpp
    src/risk/NewsCalendar.cpp
    src/executor/PaperExecutor.cpp
    src/logger/TradeJournal.cpp
    src/universe/ConnectionSelector.cpp
    src/universe/OrderbookSettingsLoader.cpp
    src/perf/TimestampMonitor.cpp
    src/perf/CpuPinning.cpp)
target_compile_definitions(core_probe PRIVATE GIT_SHA="${CORE_PROBE_GIT_SHA}")
target_link_libraries(core_probe PRIVATE
    obj_logger obj_simd_ops obj_codec obj_signal_bus obj_perf_registry
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
    add_subdirectory(test)
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

    add_executable(bench_feature_extractor
        bench/bench_feature_extractor.cpp
        src/features/FeatureExtractor.cpp)
    target_link_libraries(bench_feature_extractor PRIVATE
        obj_logger obj_simd_ops obj_perf_registry
        obj_orderbook obj_trade_stream obj_kalman_lag obj_leader_tracker
        benchmark::benchmark xsimd absl::btree absl::flat_hash_map
        hdr_histogram::hdr_histogram_static spdlog::spdlog)

    add_executable(bench_density_detector
        bench/bench_density_detector.cpp
        src/features/FeatureExtractor.cpp
        src/signals/DensityDetector.cpp)
    target_link_libraries(bench_density_detector PRIVATE
        obj_logger obj_simd_ops obj_signal_bus obj_perf_registry
        obj_orderbook obj_trade_stream obj_kalman_lag obj_leader_tracker
        obj_ticker_universe obj_universe_filters
        benchmark::benchmark xsimd absl::btree absl::flat_hash_map
        hdr_histogram::hdr_histogram_static spdlog::spdlog)

    add_executable(bench_strategy_engine_tick
        bench/bench_strategy_engine_tick.cpp
        src/strategy/StrategyEngine.cpp
        src/features/FeatureExtractor.cpp)
    target_link_libraries(bench_strategy_engine_tick PRIVATE
        obj_logger obj_simd_ops obj_signal_bus obj_perf_registry
        obj_orderbook obj_trade_stream obj_kalman_lag obj_leader_tracker
        benchmark::benchmark xsimd absl::btree absl::flat_hash_map
        hdr_histogram::hdr_histogram_static spdlog::spdlog)

    add_executable(bench_end_to_end_pipeline
        bench/bench_end_to_end_pipeline.cpp
        src/features/FeatureExtractor.cpp
        src/signals/DensityDetector.cpp
        src/strategy/StrategyEngine.cpp
        src/executor/PaperExecutor.cpp
        src/logger/TradeJournal.cpp)
    target_link_libraries(bench_end_to_end_pipeline PRIVATE
        obj_logger obj_simd_ops obj_signal_bus obj_perf_registry obj_config
        obj_orderbook obj_trade_stream obj_kalman_lag obj_leader_tracker
        obj_ticker_universe obj_universe_filters obj_metrics_registry
        benchmark::benchmark xsimd absl::btree absl::flat_hash_map
        hdr_histogram::hdr_histogram_static
        concurrentqueue::concurrentqueue
        nlohmann_json::nlohmann_json spdlog::spdlog)
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
