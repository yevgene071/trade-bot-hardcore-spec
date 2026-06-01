# ═══════════════════════════════════════════════════════════════
# test/CMakeLists.txt
# All 46 test targets extracted from the root CMakeLists.txt.
# Included via add_subdirectory(test) from the root.
# ═══════════════════════════════════════════════════════════════

enable_testing()
find_package(GTest REQUIRED CONFIG)

# ── smoke ──────────────────────────────────────────────
add_executable(smoke_test unit/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME smoke COMMAND smoke_test)

# ── logger ─────────────────────────────────────────────
add_executable(logger_test unit/logger_test.cpp)
target_link_libraries(logger_test PRIVATE obj_logger GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME logger COMMAND logger_test)

# ── numeric ────────────────────────────────────────────
add_executable(numeric_test unit/numeric_test.cpp)
target_include_directories(numeric_test PRIVATE ../src)
target_link_libraries(numeric_test PRIVATE GTest::gtest GTest::gtest_main hdr_histogram::hdr_histogram_static)
add_test(NAME numeric COMMAND numeric_test)

# ── perf ───────────────────────────────────────────────
add_executable(perf_test
    unit/perf_test.cpp
    ../src/perf/CpuPinning.cpp
)
target_link_libraries(perf_test PRIVATE
    obj_logger obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog xsimd boost::boost concurrentqueue::concurrentqueue)
add_test(NAME perf COMMAND perf_test)

# ── config ─────────────────────────────────────────────
add_executable(config_test unit/config_test.cpp)
target_link_libraries(config_test PRIVATE
    obj_logger obj_config
    GTest::gtest GTest::gtest_main spdlog::spdlog tomlplusplus::tomlplusplus)
add_test(NAME config COMMAND config_test)

# ── http_client ────────────────────────────────────────
add_executable(http_client_test
    unit/http_client_test.cpp
    ../src/transport/CurlHttpClient.cpp
)
target_link_libraries(http_client_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl boost::boost)
add_test(NAME http_client COMMAND http_client_test)

# ── discover ───────────────────────────────────────────
add_executable(discover_test
    unit/discover_test.cpp
    ../src/transport/MetaScalpDiscovery.cpp
    ../src/transport/CurlHttpClient.cpp
)
target_link_libraries(discover_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog CURL::libcurl nlohmann_json::nlohmann_json boost::boost)
add_test(NAME discover COMMAND discover_test)

# ── domain ─────────────────────────────────────────────
add_executable(domain_test unit/domain_test.cpp)
target_include_directories(domain_test PRIVATE ../src)
target_link_libraries(domain_test PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME domain COMMAND domain_test)

# ── ws ─────────────────────────────────────────────────
add_executable(ws_test
    unit/ws_test.cpp
    ../src/transport/BeastWsClient.cpp
)
target_link_libraries(ws_test PRIVATE
    obj_logger obj_metrics_registry obj_perf_registry
    GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME ws COMMAND ws_test)

# ── ws_smoke ───────────────────────────────────────────
add_executable(ws_smoke_test
    integration/ws_smoke_test.cpp
    ../src/transport/BeastWsClient.cpp
)
target_link_libraries(ws_smoke_test PRIVATE
    obj_logger obj_metrics_registry obj_perf_registry
    GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME ws_smoke COMMAND ws_smoke_test)

# ── codec ──────────────────────────────────────────────
add_executable(codec_test unit/codec_test.cpp)
target_link_libraries(codec_test PRIVATE
    obj_logger obj_codec
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json)
add_test(NAME codec COMMAND codec_test)

# ── feed ───────────────────────────────────────────────
add_executable(feed_test
    unit/feed_test.cpp
    ../src/transport/MarketDataFeed.cpp
)
target_link_libraries(feed_test PRIVATE
    obj_logger obj_codec obj_config obj_perf_registry
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME feed COMMAND feed_test)

# ── feed_smoke ─────────────────────────────────────────
add_executable(feed_smoke_test
    integration/feed_smoke_test.cpp
    ../src/transport/MarketDataFeed.cpp
    ../src/transport/BeastWsClient.cpp
)
target_link_libraries(feed_smoke_test PRIVATE
    obj_logger obj_codec obj_metrics_registry obj_config obj_perf_registry
    GTest::gtest GTest::gtest_main spdlog::spdlog boost::boost nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto absl::btree absl::flat_hash_map absl::hash)
add_test(NAME feed_smoke COMMAND feed_smoke_test)

# ── order_gateway ──────────────────────────────────────
add_executable(order_gateway_test
    unit/order_gateway_test.cpp
    ../src/transport/OrderGateway.cpp
)
target_link_libraries(order_gateway_test PRIVATE
    obj_logger obj_codec
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
add_test(NAME order_gateway COMMAND order_gateway_test)

# ── timestamp_monitor ──────────────────────────────────
add_executable(timestamp_monitor_test
    unit/timestamp_monitor_test.cpp
    ../src/perf/TimestampMonitor.cpp
    ../src/transport/MarketDataFeed.cpp
)
target_link_libraries(timestamp_monitor_test PRIVATE
    obj_logger obj_codec obj_config obj_perf_registry
    GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static nlohmann_json::nlohmann_json boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME timestamp_monitor COMMAND timestamp_monitor_test)

# ── timestamp_jitter ───────────────────────────────────
add_executable(timestamp_jitter_test
    integration/timestamp_jitter_test.cpp
    ../src/perf/TimestampMonitor.cpp
    ../src/transport/MarketDataFeed.cpp
    ../src/transport/BeastWsClient.cpp
    ../src/transport/CurlHttpClient.cpp
    ../src/transport/MetaScalpDiscovery.cpp
)
target_link_libraries(timestamp_jitter_test PRIVATE
    obj_logger obj_codec obj_metrics_registry obj_config obj_perf_registry
    GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static nlohmann_json::nlohmann_json boost::boost CURL::libcurl OpenSSL::SSL OpenSSL::Crypto absl::btree absl::flat_hash_map absl::hash)
add_test(NAME timestamp_jitter COMMAND timestamp_jitter_test)

# ── order_reconciliator ────────────────────────────────
add_executable(order_reconciliator_test
    unit/order_reconciliator_test.cpp
    ../src/trading/OrderReconciliator.cpp
)
target_link_libraries(order_reconciliator_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree)
add_test(NAME order_reconciliator COMMAND order_reconciliator_test)

# ── replay ─────────────────────────────────────────────
add_executable(replay_test
    unit/replay_test.cpp
    ../src/transport/ReplayFeed.cpp
    ../src/transport/MarketDataFeed.cpp
)
target_link_libraries(replay_test PRIVATE
    obj_logger obj_codec obj_config obj_perf_registry
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME replay COMMAND replay_test)

# ── killswitch ─────────────────────────────────────────
add_executable(killswitch_test
    unit/killswitch_test.cpp
    ../src/control/KillSwitch.cpp
)
target_link_libraries(killswitch_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME killswitch COMMAND killswitch_test)

# ── clock_drift_monitor ────────────────────────────────
add_executable(clock_drift_monitor_test
    unit/clock_drift_monitor_test.cpp
    ../src/control/ClockDriftMonitor.cpp
)
target_link_libraries(clock_drift_monitor_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME clock_drift_monitor COMMAND clock_drift_monitor_test)

# ── news_calendar ──────────────────────────────────────
add_executable(news_calendar_test
    unit/news_calendar_test.cpp
    ../src/risk/NewsCalendar.cpp
)
target_link_libraries(news_calendar_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator)
add_test(NAME news_calendar COMMAND news_calendar_test)

# ── orderbook ──────────────────────────────────────────
add_executable(orderbook_test unit/orderbook_test.cpp)
target_link_libraries(orderbook_test PRIVATE
    obj_logger obj_orderbook obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree xsimd)
add_test(NAME orderbook COMMAND orderbook_test)

# ── tradestream ────────────────────────────────────────
add_executable(tradestream_test unit/tradestream_test.cpp)
target_link_libraries(tradestream_test PRIVATE
    obj_logger obj_trade_stream obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog xsimd)
add_test(NAME tradestream COMMAND tradestream_test)

# ── cluster_snapshot_codec ─────────────────────────────
add_executable(cluster_snapshot_codec_test unit/cluster_snapshot_codec_test.cpp)
target_link_libraries(cluster_snapshot_codec_test PRIVATE
    obj_logger obj_codec
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
add_test(NAME cluster_snapshot_codec COMMAND cluster_snapshot_codec_test)

# ── cluster_snapshot_manager ───────────────────────────
add_executable(cluster_snapshot_manager_test
    unit/cluster_snapshot_manager_test.cpp
    ../src/marketdata/ClusterSnapshot.cpp
    ../src/transport/ClusterSnapshotClient.cpp
)
target_link_libraries(cluster_snapshot_manager_test PRIVATE
    obj_logger obj_codec
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
add_test(NAME cluster_snapshot_manager COMMAND cluster_snapshot_manager_test)

# ── notification_routing ───────────────────────────────
add_executable(notification_routing_test
    unit/notification_routing_test.cpp
    ../src/transport/NotificationFeed.cpp
    ../src/signals/SignalLevelBridge.cpp
    ../src/transport/SignalLevelGateway.cpp
)
target_link_libraries(notification_routing_test PRIVATE
    obj_logger obj_codec obj_ticker_universe obj_universe_filters obj_metrics_registry obj_signal_bus
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json xsimd absl::btree absl::flat_hash_map absl::hash boost::boost)
add_test(NAME notification_routing COMMAND notification_routing_test)

# ── orderbook_settings_loader ──────────────────────────
add_executable(orderbook_settings_loader_test
    unit/orderbook_settings_loader_test.cpp
    ../src/universe/OrderbookSettingsLoader.cpp
)
target_link_libraries(orderbook_settings_loader_test PRIVATE
    obj_logger obj_codec obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json absl::btree absl::flat_hash_map absl::hash)
add_test(NAME orderbook_settings_loader COMMAND orderbook_settings_loader_test)

# ── signal_bus ─────────────────────────────────────────
add_executable(signal_bus_test unit/signal_bus_test.cpp)
target_link_libraries(signal_bus_test PRIVATE
    obj_signal_bus
    GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
add_test(NAME signal_bus COMMAND signal_bus_test)

# ── density_detector ───────────────────────────────────
add_executable(density_detector_test
    unit/density_detector_test.cpp
    ../src/signals/DensityDetector.cpp
)
target_link_libraries(density_detector_test PRIVATE
    obj_logger obj_signal_bus obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
add_test(NAME density_detector COMMAND density_detector_test)

# ── iceberg_bayesian ───────────────────────────────────
add_executable(iceberg_bayesian_test
    unit/iceberg_bayesian_test.cpp
    ../src/signals/IcebergDetector.cpp
)
target_link_libraries(iceberg_bayesian_test PRIVATE
    obj_logger obj_signal_bus obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
add_test(NAME iceberg_bayesian COMMAND iceberg_bayesian_test)

# ── iceberg_detector ───────────────────────────────────
add_executable(iceberg_detector_test
    unit/iceberg_detector_test.cpp
    ../src/signals/IcebergDetector.cpp
)
target_link_libraries(iceberg_detector_test PRIVATE
    obj_logger obj_signal_bus obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
add_test(NAME iceberg_detector COMMAND iceberg_detector_test)

# ── tape_analyzer ──────────────────────────────────────
add_executable(tape_analyzer_test
    unit/tape_analyzer_test.cpp
    ../src/signals/TapeAnalyzer.cpp
)
target_link_libraries(tape_analyzer_test PRIVATE
    obj_logger obj_signal_bus obj_trade_stream obj_orderbook obj_ticker_universe obj_universe_filters obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json)
add_test(NAME tape_analyzer COMMAND tape_analyzer_test)

# ── level_dbscan ───────────────────────────────────────
add_executable(level_dbscan_test unit/level_dbscan_test.cpp)
target_include_directories(level_dbscan_test PRIVATE ../src)
target_link_libraries(level_dbscan_test PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME level_dbscan COMMAND level_dbscan_test)

# ── level_kde ──────────────────────────────────────────
add_executable(level_kde_test unit/level_kde_test.cpp)
target_include_directories(level_kde_test PRIVATE ../src)
target_link_libraries(level_kde_test PRIVATE GTest::gtest GTest::gtest_main xsimd)
add_test(NAME level_kde COMMAND level_kde_test)

# ── level_detector ─────────────────────────────────────
add_executable(level_detector_test
    unit/level_detector_test.cpp
    ../src/signals/LevelDetector.cpp
    ../src/signals/ApproachAnalyzer.cpp
    ../src/marketdata/ClusterSnapshot.cpp
    ../src/transport/ClusterSnapshotClient.cpp
)
target_link_libraries(level_detector_test PRIVATE
    obj_logger obj_signal_bus obj_orderbook obj_codec obj_ticker_universe obj_universe_filters obj_simd_ops
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json boost::boost)
add_test(NAME level_detector COMMAND level_detector_test)

# ── approach_zigzag ────────────────────────────────────
add_executable(approach_zigzag_test unit/approach_zigzag_test.cpp)
target_include_directories(approach_zigzag_test PRIVATE ../src)
target_link_libraries(approach_zigzag_test PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME approach_zigzag COMMAND approach_zigzag_test)

# ── approach_hmm_forward ───────────────────────────────
add_executable(approach_hmm_forward_test unit/approach_hmm_forward_test.cpp)
target_include_directories(approach_hmm_forward_test PRIVATE ../src)
target_link_libraries(approach_hmm_forward_test PRIVATE GTest::gtest GTest::gtest_main)
add_test(NAME approach_hmm_forward COMMAND approach_hmm_forward_test)

# ── approach_analyzer ──────────────────────────────────
add_executable(approach_analyzer_test
    unit/approach_analyzer_test.cpp
    ../src/signals/ApproachAnalyzer.cpp
    ../src/signals/LevelDetector.cpp
    ../src/marketdata/ClusterSnapshot.cpp
    ../src/transport/ClusterSnapshotClient.cpp
)
target_link_libraries(approach_analyzer_test PRIVATE
    obj_logger obj_signal_bus obj_orderbook obj_codec obj_ticker_universe obj_universe_filters obj_simd_ops
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog absl::btree absl::flat_hash_map absl::hash xsimd nlohmann_json::nlohmann_json boost::boost)
add_test(NAME approach_analyzer COMMAND approach_analyzer_test)

# ── leader_signal ──────────────────────────────────────
add_executable(leader_signal_test
    unit/leader_signal_test.cpp
    ../src/signals/LeaderSignal.cpp
)
target_link_libraries(leader_signal_test PRIVATE
    obj_logger obj_signal_bus obj_leader_tracker obj_kalman_lag
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost absl::btree)
add_test(NAME leader_signal COMMAND leader_signal_test)

# ── strategy_engine ────────────────────────────────────
add_executable(strategy_engine_test
    unit/strategy_engine_test.cpp
    ../src/strategy/StrategyEngine.cpp
)
target_link_libraries(strategy_engine_test PRIVATE
    obj_logger obj_signal_bus obj_perf_registry
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME strategy_engine COMMAND strategy_engine_test)

# ── bounce_strategy ────────────────────────────────────
add_executable(bounce_strategy_test
    unit/bounce_strategy_test.cpp
    ../src/strategy/BounceFromDensity.cpp
)
target_link_libraries(bounce_strategy_test PRIVATE
    obj_logger obj_signal_bus obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME bounce_strategy COMMAND bounce_strategy_test)

# ── breakout_strategy ──────────────────────────────────
add_executable(breakout_strategy_test
    unit/breakout_strategy_test.cpp
    ../src/strategy/BreakoutEatThrough.cpp
)
target_link_libraries(breakout_strategy_test PRIVATE
    obj_logger obj_signal_bus obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME breakout_strategy COMMAND breakout_strategy_test)

# ── leaderlag_strategy ─────────────────────────────────
add_executable(leaderlag_strategy_test
    unit/leaderlag_strategy_test.cpp
    ../src/strategy/LeaderLag.cpp
)
target_link_libraries(leaderlag_strategy_test PRIVATE
    obj_logger obj_signal_bus obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME leaderlag_strategy COMMAND leaderlag_strategy_test)

# ── signal_level_bridge ────────────────────────────────
add_executable(signal_level_bridge_test
    unit/signal_level_bridge_test.cpp
    ../src/signals/SignalLevelBridge.cpp
    ../src/transport/SignalLevelGateway.cpp
)
target_link_libraries(signal_level_bridge_test PRIVATE
    obj_logger obj_codec obj_signal_bus
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog absl::btree xsimd nlohmann_json::nlohmann_json boost::boost)
add_test(NAME signal_level_bridge COMMAND signal_level_bridge_test)

# ── risk_manager ───────────────────────────────────────
add_executable(risk_manager_test
    unit/risk_manager_test.cpp
    ../src/risk/RiskManager.cpp
    ../src/risk/NewsCalendar.cpp
)
target_link_libraries(risk_manager_test PRIVATE
    obj_logger obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME risk_manager COMMAND risk_manager_test)

# ── r13_funding ────────────────────────────────────────
add_executable(r13_funding_test
    unit/r13_funding_test.cpp
    ../src/risk/RiskManager.cpp
    ../src/risk/NewsCalendar.cpp
)
target_link_libraries(r13_funding_test PRIVATE
    obj_logger obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME r13_funding COMMAND r13_funding_test)

# ── trading_day_reset ──────────────────────────────────
add_executable(trading_day_reset_test
    unit/trading_day_reset_idempotent_test.cpp
    ../src/risk/AccountStatePersister.cpp
)
target_link_libraries(trading_day_reset_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost)
add_test(NAME trading_day_reset COMMAND trading_day_reset_test)

# ── risk_flow ──────────────────────────────────────────
add_executable(risk_flow_test
    integration/risk_flow_test.cpp
    ../src/risk/RiskManager.cpp
    ../src/risk/NewsCalendar.cpp
)
target_link_libraries(risk_flow_test PRIVATE
    obj_logger obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json nlohmann_json_schema_validator absl::btree absl::flat_hash_map absl::hash)
add_test(NAME risk_flow COMMAND risk_flow_test)

# ── state_persistence ──────────────────────────────────
add_executable(state_persistence_test
    integration/state_persistence_test.cpp
    ../src/risk/AccountStatePersister.cpp
)
target_link_libraries(state_persistence_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json)
add_test(NAME state_persistence COMMAND state_persistence_test)

# ── metrics ────────────────────────────────────────────
add_executable(metrics_test unit/metrics_test.cpp)
target_link_libraries(metrics_test PRIVATE
    obj_metrics_registry
    GTest::gtest GTest::gtest_main)
add_test(NAME metrics COMMAND metrics_test)

# ── metrics_exporter ───────────────────────────────────
add_executable(metrics_exporter_test
    unit/metrics_exporter_test.cpp
    ../src/metrics/MetricsExporter.cpp
    ../src/transport/CurlHttpClient.cpp
)
target_link_libraries(metrics_exporter_test PRIVATE
    obj_logger obj_metrics_registry obj_perf_registry
    GTest::gtest GTest::gtest_main CURL::libcurl spdlog::spdlog boost::boost absl::btree absl::flat_hash_map absl::hash)
add_test(NAME metrics_exporter COMMAND metrics_exporter_test)

# ── startup_recovery ───────────────────────────────────
add_executable(startup_recovery_test
    unit/startup_recovery_test.cpp
    ../src/executor/StartupRecovery.cpp
    ../src/risk/AccountStatePersister.cpp
    ../src/transport/OrderGateway.cpp
)
target_link_libraries(startup_recovery_test PRIVATE
    obj_logger obj_codec
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json CURL::libcurl)
add_test(NAME startup_recovery COMMAND startup_recovery_test)

# ── executor_state_machine ─────────────────────────────
add_executable(executor_state_machine_test
    unit/executor_state_machine_test.cpp
    ../src/executor/LiveExecutor.cpp
    ../src/trading/OrderReconciliator.cpp
    ../src/transport/OrderGateway.cpp
    ../src/transport/MarketDataFeed.cpp
)
target_link_libraries(executor_state_machine_test PRIVATE
    obj_logger obj_codec obj_metrics_registry obj_config obj_perf_registry obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json CURL::libcurl absl::btree absl::flat_hash_map absl::hash)
add_test(NAME executor_state_machine COMMAND executor_state_machine_test)

# ── stop_contract ──────────────────────────────────────
add_executable(stop_contract_test
    contract/stop_contract_test.cpp
    ../src/transport/OrderGateway.cpp
    ../src/transport/MetaScalpDiscovery.cpp
    ../src/transport/CurlHttpClient.cpp
    ../src/transport/MarketDataFeed.cpp
    ../src/transport/BeastWsClient.cpp
)
target_link_libraries(stop_contract_test PRIVATE
    obj_logger obj_codec obj_metrics_registry obj_config obj_perf_registry
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog nlohmann_json::nlohmann_json CURL::libcurl boost::boost OpenSSL::SSL OpenSSL::Crypto absl::btree absl::flat_hash_map absl::hash)
add_test(NAME stop_contract COMMAND stop_contract_test)

# ── leader_correlation_welford ─────────────────────────
add_executable(leader_correlation_welford_test unit/leader_correlation_welford_test.cpp)
target_link_libraries(leader_correlation_welford_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME leader_correlation_welford COMMAND leader_correlation_welford_test)

# ── kalman_lag_estimator ───────────────────────────────
add_executable(kalman_lag_estimator_test unit/kalman_lag_estimator_test.cpp)
target_link_libraries(kalman_lag_estimator_test PRIVATE
    obj_logger obj_kalman_lag
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME kalman_lag_estimator COMMAND kalman_lag_estimator_test)

# ── leader ─────────────────────────────────────────────
add_executable(leader_test unit/leader_test.cpp)
target_link_libraries(leader_test PRIVATE
    obj_logger obj_leader_tracker obj_kalman_lag
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME leader COMMAND leader_test)

# ── leader_cusum_decorrelation ─────────────────────────
add_executable(leader_cusum_decorrelation_test unit/leader_cusum_decorrelation_test.cpp)
target_link_libraries(leader_cusum_decorrelation_test PRIVATE
    obj_logger obj_leader_tracker obj_kalman_lag
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME leader_cusum_decorrelation COMMAND leader_cusum_decorrelation_test)

# ── feature_extractor ──────────────────────────────────
add_executable(feature_extractor_test
    unit/feature_extractor_test.cpp
    ../src/features/FeatureExtractor.cpp
)
target_link_libraries(feature_extractor_test PRIVATE
    obj_logger obj_orderbook obj_trade_stream obj_leader_tracker obj_kalman_lag obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static absl::btree xsimd)
add_test(NAME feature_extractor COMMAND feature_extractor_test)

# ── feature_welford ────────────────────────────────────
add_executable(feature_welford_test
    unit/feature_welford_test.cpp
    ../src/features/FeatureExtractor.cpp
)
target_link_libraries(feature_welford_test PRIVATE
    obj_logger obj_orderbook obj_trade_stream obj_leader_tracker obj_kalman_lag obj_simd_ops
    GTest::gtest GTest::gtest_main spdlog::spdlog hdr_histogram::hdr_histogram_static absl::btree xsimd)
add_test(NAME feature_welford COMMAND feature_welford_test)

# ── universe_filters ───────────────────────────────────
add_executable(universe_filters_test unit/universe_filters_test.cpp)
target_link_libraries(universe_filters_test PRIVATE
    obj_logger obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME universe_filters COMMAND universe_filters_test)

# ── connection_selector ────────────────────────────────
add_executable(connection_selector_test
    unit/connection_selector_test.cpp
    ../src/universe/ConnectionSelector.cpp
)
target_link_libraries(connection_selector_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog)
add_test(NAME connection_selector COMMAND connection_selector_test)

# ── ticker_universe_pool ───────────────────────────────
add_executable(ticker_universe_pool_test unit/ticker_universe_pool_test.cpp)
target_link_libraries(ticker_universe_pool_test PRIVATE
    obj_logger obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash)
add_test(NAME ticker_universe_pool COMMAND ticker_universe_pool_test)

# ── ticker_universe_affinity ───────────────────────────
add_executable(ticker_universe_affinity_test unit/ticker_universe_affinity_test.cpp)
target_link_libraries(ticker_universe_affinity_test PRIVATE
    obj_logger obj_ticker_universe obj_universe_filters
    GTest::gtest GTest::gtest_main spdlog::spdlog absl::btree absl::flat_hash_map absl::hash)
add_test(NAME ticker_universe_affinity COMMAND ticker_universe_affinity_test)

# ── dashboard_server ───────────────────────────────────
add_executable(dashboard_server_test
    unit/dashboard_server_test.cpp
    ../src/control/DashboardServer.cpp
    ../src/control/DashboardServerBinary.cpp
    ../src/transport/DumpRecorder.cpp
)
target_include_directories(dashboard_server_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(dashboard_server_test PRIVATE
    obj_logger
    GTest::gtest GTest::gtest_main spdlog::spdlog nlohmann_json::nlohmann_json boost::boost OpenSSL::SSL OpenSSL::Crypto flatbuffers::flatbuffers)
add_test(NAME dashboard_server COMMAND dashboard_server_test)

# ── replay_determinism ─────────────────────────────────
add_executable(replay_determinism_test
    integration/replay_determinism_test.cpp
    ../src/transport/ReplayFeed.cpp
    ../src/transport/MarketDataFeed.cpp
    ../src/marketdata/ClusterSnapshot.cpp
    ../src/transport/ClusterSnapshotClient.cpp
    ../src/signals/DensityDetector.cpp
    ../src/signals/IcebergDetector.cpp
    ../src/signals/TapeAnalyzer.cpp
    ../src/signals/LevelDetector.cpp
    ../src/signals/ApproachAnalyzer.cpp
    ../src/features/FeatureExtractor.cpp
    ../src/strategy/StrategyEngine.cpp
    ../src/strategy/BounceFromDensity.cpp
    ../src/strategy/BreakoutEatThrough.cpp
    ../src/risk/RiskManager.cpp
    ../src/risk/NewsCalendar.cpp
    ../src/risk/AccountStatePersister.cpp
    ../src/logger/TradeJournal.cpp
)
target_link_libraries(replay_determinism_test PRIVATE
    obj_logger obj_codec obj_orderbook obj_trade_stream obj_signal_bus
    obj_leader_tracker obj_kalman_lag obj_ticker_universe obj_universe_filters obj_simd_ops
    obj_metrics_registry obj_perf_registry obj_config
    GTest::gtest GTest::gtest_main GTest::gmock spdlog::spdlog 
    nlohmann_json::nlohmann_json nlohmann_json_schema_validator
    absl::btree absl::flat_hash_map absl::hash xsimd 
    hdr_histogram::hdr_histogram_static boost::boost)
add_test(NAME replay_determinism COMMAND replay_determinism_test)
