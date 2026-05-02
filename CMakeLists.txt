cmake_minimum_required(VERSION 3.22)
project(trade_bot
    VERSION 0.0.1
    DESCRIPTION "Trade Bot — hardcore C++23 implementation"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_AVX2 "Enable AVX2 optimizations" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer in Debug" ON)

# Strict warnings
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
    if(ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# Dependencies
find_package(spdlog REQUIRED CONFIG)
find_package(hdr_histogram REQUIRED CONFIG)
find_package(xsimd REQUIRED CONFIG)
find_package(concurrentqueue REQUIRED CONFIG)
find_package(benchmark REQUIRED CONFIG)
find_package(Boost REQUIRED CONFIG)
find_package(tomlplusplus REQUIRED CONFIG)
find_package(CURL REQUIRED CONFIG)
find_package(nlohmann_json REQUIRED CONFIG)
find_package(nlohmann_json_schema_validator REQUIRED CONFIG)
find_package(OpenSSL REQUIRED CONFIG)
find_package(absl REQUIRED CONFIG)
# Workaround: Conan 2 openssl/3.6.2 recipe forgets to populate INTERFACE_INCLUDE_DIRECTORIES.
# Force-attach include path from the package folder.
if(DEFINED openssl_PACKAGE_FOLDER_RELEASE)
    set_property(TARGET OpenSSL::SSL APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${openssl_PACKAGE_FOLDER_RELEASE}/include")
    set_property(TARGET OpenSSL::Crypto APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${openssl_PACKAGE_FOLDER_RELEASE}/include")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Main executable (T0-SETUP: just prints version and exits)
add_executable(trade_bot
    src/main.cpp
    src/logger/Logger.cpp
    src/perf/CpuPinning.cpp
    src/perf/SimdOps.cpp
    src/perf/TimestampMonitor.cpp
    src/config/Config.cpp
    src/transport/CurlHttpClient.cpp
    src/transport/MetaScalpDiscovery.cpp
    src/transport/BeastWsClient.cpp
    src/transport/MetaScalpCodec.cpp
    src/transport/MarketDataFeed.cpp
    src/transport/OrderGateway.cpp
    src/transport/ReplayFeed.cpp
    src/trading/OrderReconciliator.cpp
    src/control/KillSwitch.cpp
    src/control/ClockDriftMonitor.cpp
    src/control/SntpClient.cpp
    src/risk/NewsCalendar.cpp
    src/marketdata/OrderBook.cpp
    src/marketdata/KalmanLagEstimator.cpp
    src/marketdata/LeaderTracker.cpp
    src/marketdata/TradeStream.cpp
    src/features/FeatureExtractor.cpp
    src/universe/UniverseFilters.cpp
    src/universe/ConnectionSelector.cpp
    src/universe/TickerUniverse.cpp
)
target_include_directories(trade_bot PRIVATE src)
target_link_libraries(trade_bot PRIVATE 
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
)

# Tests
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED CONFIG)
    
    add_executable(smoke_test test/unit/smoke_test.cpp)
    target_link_libraries(smoke_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME smoke COMMAND smoke_test)

    add_executable(logger_test 
        test/unit/logger_test.cpp 
        src/logger/Logger.cpp
    )
    target_include_directories(logger_test PRIVATE src)
    target_link_libraries(logger_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME logger COMMAND logger_test)

    add_executable(numeric_test 
        test/unit/numeric_test.cpp
    )
    target_include_directories(numeric_test PRIVATE src)
    target_link_libraries(numeric_test PRIVATE GTest::gtest GTest::gtest_main hdr_histogram::hdr_histogram_static)
    add_test(NAME numeric COMMAND numeric_test)

    add_executable(perf_test 
        test/unit/perf_test.cpp
        src/perf/CpuPinning.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(perf_test PRIVATE src)
    target_link_libraries(perf_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog xsimd boost::boost concurrentqueue::concurrentqueue)
    add_test(NAME perf COMMAND perf_test)

    add_executable(config_test 
        test/unit/config_test.cpp
        src/config/Config.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(config_test PRIVATE src)
    target_link_libraries(config_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog tomlplusplus::tomlplusplus)
    add_test(NAME config COMMAND config_test)

    add_executable(http_client_test 
        test/unit/http_client_test.cpp
        src/transport/CurlHttpClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(http_client_test PRIVATE src)
    target_link_libraries(http_client_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl boost::boost)
    add_test(NAME http_client COMMAND http_client_test)

    add_executable(discover_test 
        test/unit/discover_test.cpp
        src/transport/MetaScalpDiscovery.cpp
        src/transport/CurlHttpClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(discover_test PRIVATE src)
    target_link_libraries(discover_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME discover COMMAND discover_test)

    add_executable(domain_test 
        test/unit/domain_test.cpp
    )
    target_include_directories(domain_test PRIVATE src)
    target_link_libraries(domain_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME domain COMMAND domain_test)

    add_executable(ws_test 
        test/unit/ws_test.cpp
        src/transport/BeastWsClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(ws_test PRIVATE src)
    target_link_libraries(ws_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME ws COMMAND ws_test)

    add_executable(ws_smoke_test 
        test/integration/ws_smoke_test.cpp
        src/transport/BeastWsClient.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(ws_smoke_test PRIVATE src)
    target_link_libraries(ws_smoke_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME ws_smoke COMMAND ws_smoke_test)

    add_executable(codec_test 
        test/unit/codec_test.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(codec_test PRIVATE src)
    target_link_libraries(codec_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json)
    add_test(NAME codec COMMAND codec_test)

    add_executable(feed_test 
        test/unit/feed_test.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(feed_test PRIVATE src)
    target_link_libraries(feed_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME feed COMMAND feed_test)

    add_executable(feed_smoke_test 
        test/integration/feed_smoke_test.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/BeastWsClient.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(feed_smoke_test PRIVATE src)
    target_link_libraries(feed_smoke_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME feed_smoke COMMAND feed_smoke_test)

    add_executable(order_gateway_test
        test/unit/order_gateway_test.cpp
        src/transport/OrderGateway.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(order_gateway_test PRIVATE src)
    target_link_libraries(order_gateway_test PRIVATE GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
    add_test(NAME order_gateway COMMAND order_gateway_test)

    # T0-MONITOR-TIMESTAMPS
    add_executable(timestamp_monitor_test
        test/unit/timestamp_monitor_test.cpp
        src/perf/TimestampMonitor.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(timestamp_monitor_test PRIVATE src)
    target_link_libraries(timestamp_monitor_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        hdr_histogram::hdr_histogram_static
        nlohmann_json::nlohmann_json
        boost::boost)
    add_test(NAME timestamp_monitor COMMAND timestamp_monitor_test)

    add_executable(timestamp_jitter_test
        test/integration/timestamp_jitter_test.cpp
        src/perf/TimestampMonitor.cpp
        src/transport/MarketDataFeed.cpp
        src/transport/MetaScalpCodec.cpp
        src/transport/BeastWsClient.cpp
        src/transport/CurlHttpClient.cpp
        src/transport/MetaScalpDiscovery.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(timestamp_jitter_test PRIVATE src)
    target_link_libraries(timestamp_jitter_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        hdr_histogram::hdr_histogram_static
        nlohmann_json::nlohmann_json
        boost::boost
        CURL::libcurl
        OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME timestamp_jitter COMMAND timestamp_jitter_test)

    # T0-ORDER-RECONCILIATION
    add_executable(order_reconciliator_test
        test/unit/order_reconciliator_test.cpp
        src/trading/OrderReconciliator.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(order_reconciliator_test PRIVATE src)
    target_link_libraries(order_reconciliator_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog)
    add_test(NAME order_reconciliator COMMAND order_reconciliator_test)

    # T0-REPLAY
    add_executable(replay_test
        test/unit/replay_test.cpp
        src/transport/ReplayFeed.cpp
        src/transport/MetaScalpCodec.cpp
        src/transport/MarketDataFeed.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(replay_test PRIVATE src)
    target_link_libraries(replay_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        boost::boost)
    add_test(NAME replay COMMAND replay_test)

    # T0-KILLSWITCH
    add_executable(killswitch_test
        test/unit/killswitch_test.cpp
        src/control/KillSwitch.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(killswitch_test PRIVATE src)
    target_link_libraries(killswitch_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog)
    add_test(NAME killswitch COMMAND killswitch_test)

    # T0-CLOCK (uses INtpClient mock — SntpClient real impl is wired in trade_bot main only)
    add_executable(clock_drift_monitor_test
        test/unit/clock_drift_monitor_test.cpp
        src/control/ClockDriftMonitor.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(clock_drift_monitor_test PRIVATE src)
    target_link_libraries(clock_drift_monitor_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog)
    add_test(NAME clock_drift_monitor COMMAND clock_drift_monitor_test)

    # T0-NEWS
    add_executable(news_calendar_test
        test/unit/news_calendar_test.cpp
        src/risk/NewsCalendar.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(news_calendar_test PRIVATE src)
    target_link_libraries(news_calendar_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator)
    add_test(NAME news_calendar COMMAND news_calendar_test)

    # T1-ORDERBOOK
    add_executable(orderbook_test
        test/unit/orderbook_test.cpp
        src/marketdata/OrderBook.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(orderbook_test PRIVATE src)
    target_link_libraries(orderbook_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        absl::btree
        xsimd)
    add_test(NAME orderbook COMMAND orderbook_test)

    add_executable(tradestream_test
        test/unit/tradestream_test.cpp
        src/marketdata/TradeStream.cpp
        src/logger/Logger.cpp
        src/perf/SimdOps.cpp
    )
    target_include_directories(tradestream_test PRIVATE src)
    target_link_libraries(tradestream_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        xsimd)
    add_test(NAME tradestream COMMAND tradestream_test)

    add_executable(cluster_snapshot_codec_test
        test/unit/cluster_snapshot_codec_test.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(cluster_snapshot_codec_test PRIVATE src)
    target_link_libraries(cluster_snapshot_codec_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME cluster_snapshot_codec COMMAND cluster_snapshot_codec_test)

    add_executable(cluster_snapshot_manager_test
        test/unit/cluster_snapshot_manager_test.cpp
        src/marketdata/ClusterSnapshot.cpp
        src/transport/ClusterSnapshotClient.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(cluster_snapshot_manager_test PRIVATE src)
    target_link_libraries(cluster_snapshot_manager_test PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME cluster_snapshot_manager COMMAND cluster_snapshot_manager_test)

    add_executable(notification_routing_test
        test/unit/notification_routing_test.cpp
        src/transport/NotificationFeed.cpp
        src/transport/MetaScalpCodec.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(notification_routing_test PRIVATE src)
    target_link_libraries(notification_routing_test PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME notification_routing COMMAND notification_routing_test)

    add_executable(orderbook_settings_loader_test
        test/unit/orderbook_settings_loader_test.cpp
        src/universe/OrderbookSettingsLoader.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/transport/MetaScalpCodec.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(orderbook_settings_loader_test PRIVATE src)
    target_link_libraries(orderbook_settings_loader_test PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME orderbook_settings_loader COMMAND orderbook_settings_loader_test)

    add_executable(signal_bus_test
        test/unit/signal_bus_test.cpp
        src/signals/SignalBus.cpp
    )
    target_include_directories(signal_bus_test PRIVATE src)
    target_link_libraries(signal_bus_test PRIVATE
        GTest::gtest GTest::gtest_main
        nlohmann_json::nlohmann_json)
    add_test(NAME signal_bus COMMAND signal_bus_test)

    add_executable(density_detector_test
        test/unit/density_detector_test.cpp
        src/signals/DensityDetector.cpp
        src/signals/SignalBus.cpp
        src/marketdata/OrderBook.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(density_detector_test PRIVATE src)
    target_link_libraries(density_detector_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        absl::btree
        absl::flat_hash_map
        absl::hash
        xsimd
        nlohmann_json::nlohmann_json)
    add_test(NAME density_detector COMMAND density_detector_test)

    add_executable(iceberg_bayesian_test
        test/unit/iceberg_bayesian_test.cpp
        src/signals/IcebergDetector.cpp
        src/signals/SignalBus.cpp
        src/marketdata/OrderBook.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(iceberg_bayesian_test PRIVATE src)
    target_link_libraries(iceberg_bayesian_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        absl::btree
        absl::flat_hash_map
        absl::hash
        xsimd
        nlohmann_json::nlohmann_json)
    add_test(NAME iceberg_bayesian COMMAND iceberg_bayesian_test)

    add_executable(iceberg_detector_test
        test/unit/iceberg_detector_test.cpp
        src/signals/IcebergDetector.cpp
        src/signals/SignalBus.cpp
        src/marketdata/OrderBook.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(iceberg_detector_test PRIVATE src)
    target_link_libraries(iceberg_detector_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        absl::btree
        absl::flat_hash_map
        absl::hash
        xsimd
        nlohmann_json::nlohmann_json)
    add_test(NAME iceberg_detector COMMAND iceberg_detector_test)

    add_executable(tape_analyzer_test
        test/unit/tape_analyzer_test.cpp
        src/signals/TapeAnalyzer.cpp
        src/signals/SignalBus.cpp
        src/marketdata/TradeStream.cpp
        src/marketdata/OrderBook.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(tape_analyzer_test PRIVATE src)
    target_link_libraries(tape_analyzer_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        absl::btree
        absl::flat_hash_map
        absl::hash
        xsimd
        nlohmann_json::nlohmann_json)
    add_test(NAME tape_analyzer COMMAND tape_analyzer_test)

    add_executable(level_dbscan_test
        test/unit/level_dbscan_test.cpp
    )
    target_include_directories(level_dbscan_test PRIVATE src)
    target_link_libraries(level_dbscan_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME level_dbscan COMMAND level_dbscan_test)

    add_executable(level_kde_test
        test/unit/level_kde_test.cpp
    )
    target_include_directories(level_kde_test PRIVATE src)
    target_link_libraries(level_kde_test PRIVATE GTest::gtest GTest::gtest_main xsimd)
    add_test(NAME level_kde COMMAND level_kde_test)

    add_executable(level_detector_test
        test/unit/level_detector_test.cpp
        src/signals/LevelDetector.cpp
        src/signals/SignalBus.cpp
        src/marketdata/OrderBook.cpp
        src/marketdata/ClusterSnapshot.cpp
        src/transport/ClusterSnapshotClient.cpp
        src/transport/MetaScalpCodec.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(level_detector_test PRIVATE src)
    target_link_libraries(level_detector_test PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        spdlog::spdlog
        absl::btree
        absl::flat_hash_map
        absl::hash
        xsimd
        nlohmann_json::nlohmann_json)
    add_test(NAME level_detector COMMAND level_detector_test)

    add_executable(approach_zigzag_test
        test/unit/approach_zigzag_test.cpp
    )
    target_include_directories(approach_zigzag_test PRIVATE src)
    target_link_libraries(approach_zigzag_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME approach_zigzag COMMAND approach_zigzag_test)

    add_executable(approach_hmm_forward_test
        test/unit/approach_hmm_forward_test.cpp
    )
    target_include_directories(approach_hmm_forward_test PRIVATE src)
    target_link_libraries(approach_hmm_forward_test PRIVATE GTest::gtest GTest::gtest_main)
    add_test(NAME approach_hmm_forward COMMAND approach_hmm_forward_test)

    add_executable(approach_analyzer_test
        test/unit/approach_analyzer_test.cpp
        src/signals/ApproachAnalyzer.cpp
        src/signals/LevelDetector.cpp
        src/signals/SignalBus.cpp
        src/marketdata/OrderBook.cpp
        src/marketdata/ClusterSnapshot.cpp
        src/transport/ClusterSnapshotClient.cpp
        src/transport/MetaScalpCodec.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(approach_analyzer_test PRIVATE src)
    target_link_libraries(approach_analyzer_test PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        spdlog::spdlog
        absl::btree
        absl::flat_hash_map
        absl::hash
        xsimd
        nlohmann_json::nlohmann_json)
    add_test(NAME approach_analyzer COMMAND approach_analyzer_test)

    add_executable(leader_signal_test
        test/unit/leader_signal_test.cpp
        src/signals/LeaderSignal.cpp
        src/signals/SignalBus.cpp
        src/marketdata/LeaderTracker.cpp
        src/marketdata/KalmanLagEstimator.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(leader_signal_test PRIVATE src)
    target_link_libraries(leader_signal_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME leader_signal COMMAND leader_signal_test)

    add_executable(strategy_engine_test
        test/unit/strategy_engine_test.cpp
        src/strategy/StrategyEngine.cpp
        src/signals/SignalBus.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(strategy_engine_test PRIVATE src)
    target_link_libraries(strategy_engine_test PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME strategy_engine COMMAND strategy_engine_test)

    add_executable(bounce_strategy_test
        test/unit/bounce_strategy_test.cpp
        src/strategy/BounceFromDensity.cpp
        src/signals/SignalBus.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(bounce_strategy_test PRIVATE src)
    target_link_libraries(bounce_strategy_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME bounce_strategy COMMAND bounce_strategy_test)

    add_executable(breakout_strategy_test
        test/unit/breakout_strategy_test.cpp
        src/strategy/BreakoutEatThrough.cpp
        src/signals/SignalBus.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(breakout_strategy_test PRIVATE src)
    target_link_libraries(breakout_strategy_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME breakout_strategy COMMAND breakout_strategy_test)

    add_executable(leaderlag_strategy_test
        test/unit/leaderlag_strategy_test.cpp
        src/strategy/LeaderLag.cpp
        src/signals/SignalBus.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(leaderlag_strategy_test PRIVATE src)
    target_link_libraries(leaderlag_strategy_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog
        nlohmann_json::nlohmann_json)
    add_test(NAME leaderlag_strategy COMMAND leaderlag_strategy_test)

    # Benchmarks
    add_executable(leader_correlation_welford_test
        test/unit/leader_correlation_welford_test.cpp
        src/logger/Logger.cpp)
    target_include_directories(leader_correlation_welford_test PRIVATE src)
    target_link_libraries(leader_correlation_welford_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME leader_correlation_welford COMMAND leader_correlation_welford_test)

    add_executable(kalman_lag_estimator_test
        test/unit/kalman_lag_estimator_test.cpp
        src/marketdata/KalmanLagEstimator.cpp
        src/logger/Logger.cpp)
    target_include_directories(kalman_lag_estimator_test PRIVATE src)
    target_link_libraries(kalman_lag_estimator_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME kalman_lag_estimator COMMAND kalman_lag_estimator_test)

    add_executable(leader_test
        test/unit/leader_test.cpp
        src/marketdata/LeaderTracker.cpp
        src/marketdata/KalmanLagEstimator.cpp
        src/logger/Logger.cpp)
    target_include_directories(leader_test PRIVATE src)
    target_link_libraries(leader_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME leader COMMAND leader_test)

    add_executable(leader_cusum_decorrelation_test
        test/unit/leader_cusum_decorrelation_test.cpp
        src/marketdata/LeaderTracker.cpp
        src/marketdata/KalmanLagEstimator.cpp
        src/logger/Logger.cpp)
    target_include_directories(leader_cusum_decorrelation_test PRIVATE src)
    target_link_libraries(leader_cusum_decorrelation_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME leader_cusum_decorrelation COMMAND leader_cusum_decorrelation_test)

    # T1-FRAME
    add_executable(feature_extractor_test
        test/unit/feature_extractor_test.cpp
        src/features/FeatureExtractor.cpp
        src/marketdata/OrderBook.cpp
        src/marketdata/TradeStream.cpp
        src/marketdata/LeaderTracker.cpp
        src/marketdata/KalmanLagEstimator.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp)
    target_include_directories(feature_extractor_test PRIVATE src)
    target_link_libraries(feature_extractor_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog hdr_histogram::hdr_histogram_static
        absl::btree xsimd)
    add_test(NAME feature_extractor COMMAND feature_extractor_test)

    add_executable(feature_welford_test
        test/unit/feature_welford_test.cpp
        src/features/FeatureExtractor.cpp
        src/marketdata/OrderBook.cpp
        src/marketdata/TradeStream.cpp
        src/marketdata/LeaderTracker.cpp
        src/marketdata/KalmanLagEstimator.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp)
    target_include_directories(feature_welford_test PRIVATE src)
    target_link_libraries(feature_welford_test PRIVATE
        GTest::gtest GTest::gtest_main
        spdlog::spdlog hdr_histogram::hdr_histogram_static
        absl::btree xsimd)
    add_test(NAME feature_welford COMMAND feature_welford_test)

    # T1-UNIVERSE
    add_executable(universe_filters_test
        test/unit/universe_filters_test.cpp
        src/universe/UniverseFilters.cpp
        src/logger/Logger.cpp)
    target_include_directories(universe_filters_test PRIVATE src)
    target_link_libraries(universe_filters_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME universe_filters COMMAND universe_filters_test)

    add_executable(connection_selector_test
        test/unit/connection_selector_test.cpp
        src/universe/ConnectionSelector.cpp
        src/logger/Logger.cpp)
    target_include_directories(connection_selector_test PRIVATE src)
    target_link_libraries(connection_selector_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME connection_selector COMMAND connection_selector_test)

    add_executable(ticker_universe_pool_test
        test/unit/ticker_universe_pool_test.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/logger/Logger.cpp)
    target_include_directories(ticker_universe_pool_test PRIVATE src)
    target_link_libraries(ticker_universe_pool_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME ticker_universe_pool COMMAND ticker_universe_pool_test)

    add_executable(ticker_universe_affinity_test
        test/unit/ticker_universe_affinity_test.cpp
        src/universe/TickerUniverse.cpp
        src/universe/UniverseFilters.cpp
        src/logger/Logger.cpp)
    target_include_directories(ticker_universe_affinity_test PRIVATE src)
    target_link_libraries(ticker_universe_affinity_test PRIVATE
        GTest::gtest GTest::gtest_main spdlog::spdlog)
    add_test(NAME ticker_universe_affinity COMMAND ticker_universe_affinity_test)

    # Benchmarks
    add_executable(bench_perf 
        bench/bench_perf.cpp
        src/perf/SimdOps.cpp
    )
    target_include_directories(bench_perf PRIVATE src)
    target_link_libraries(bench_perf PRIVATE benchmark::benchmark xsimd)

    add_executable(bench_orderbook_apply
        bench/bench_orderbook_apply.cpp
        src/marketdata/OrderBook.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(bench_orderbook_apply PRIVATE src)
    target_link_libraries(bench_orderbook_apply PRIVATE benchmark::benchmark xsimd absl::btree spdlog::spdlog)

    add_executable(bench_orderbook_depth
        bench/bench_orderbook_depth.cpp
        src/marketdata/OrderBook.cpp
        src/perf/SimdOps.cpp
        src/logger/Logger.cpp
    )
    target_include_directories(bench_orderbook_depth PRIVATE src)
    target_link_libraries(bench_orderbook_depth PRIVATE benchmark::benchmark xsimd absl::btree spdlog::spdlog)

    add_executable(bench_tradestream_on_trade
        bench/bench_tradestream_on_trade.cpp
        src/marketdata/TradeStream.cpp
        src/logger/Logger.cpp
        src/perf/SimdOps.cpp
    )
    target_include_directories(bench_tradestream_on_trade PRIVATE src)
    target_link_libraries(bench_tradestream_on_trade PRIVATE benchmark::benchmark xsimd spdlog::spdlog)
endif()

message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "AVX2:         ${ENABLE_AVX2}")
message(STATUS "Sanitizers:   ${ENABLE_SANITIZERS}")
