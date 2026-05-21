#pragma once

#include "domain/Types.hpp"
#include "executor/IExecutor.hpp"
#include "perf/MpmcQueue.hpp"
#include "perf/TraceContext.hpp"

#include <boost/lockfree/spsc_queue.hpp>
#include <simdjson.h>
#include "absl/container/btree_map.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace trade_bot {

// Forward declarations
class OrderBook;
class TradeStream;
class LeaderTracker;
class FeatureExtractor;
class SignalBus;
class StrategyEngine;
class RiskManager;
class TickerController;

/**
 * Raw WebSocket event structure for zero-copy transport from WS thread to processor thread.
 * Pre-allocated arena avoids heap allocations in hot path.
 */
struct RawWsEvent {
    static constexpr size_t kMaxPayload = 8192;

    TraceId trace_id{0};
    uint64_t recv_ns{0};  // monotonic ns at WS receive
    // Extra 64 bytes satisfy simdjson's SIMDJSON_PADDING requirement without heap allocation.
    std::array<char, kMaxPayload + 64> payload{};
    uint32_t payload_size{0};
    
    RawWsEvent() = default;
    
    // Copy/move enabled for boost::lockfree::spsc_queue compatibility
    RawWsEvent(const RawWsEvent&) = default;
    RawWsEvent& operator=(const RawWsEvent&) = default;
    RawWsEvent(RawWsEvent&&) noexcept = default;
    RawWsEvent& operator=(RawWsEvent&&) noexcept = default;
};

/**
 * PipelineProcessor: Event-driven processing thread for hot path.
 * 
 * Owns:
 * - SpscQueue<RawWsEvent> (input from WS thread)
 * - Registry of OrderBook, TradeStream, LeaderTracker per ticker
 * - Pointers to FeatureExtractor, SignalBus, StrategyEngine, RiskManager, IExecutor
 * 
 * Main loop: busy-wait on queue, process events through full pipeline
 * (OrderBook → Feature → Signal → Strategy → Risk → Submit) without
 * waiting for tick timer.
 * 
 * Thread is CPU-pinned to hot_path_cpu from config for minimal scheduling latency.
 */
class PipelineProcessor {
public:
    struct Config {
        int hot_path_cpu = 2;
        size_t spsc_queue_capacity = 65536;
        double feature_rate_hz = 20.0;
    };
    
    explicit PipelineProcessor(Config cfg);
    ~PipelineProcessor();
    
    // Non-copyable, non-movable (owns thread)
    PipelineProcessor(const PipelineProcessor&) = delete;
    PipelineProcessor& operator=(const PipelineProcessor&) = delete;
    PipelineProcessor(PipelineProcessor&&) = delete;
    PipelineProcessor& operator=(PipelineProcessor&&) = delete;
    
    /**
     * Start the processor thread (CPU-pinned busy loop).
     */
    void start();
    
    /**
     * Stop the processor thread gracefully.
     */
    void stop();
    
    /**
     * Enqueue a raw WS event for processing (called from WS thread).
     * Non-blocking push to lock-free SPSC queue.
     * 
     * @return true if enqueued, false if queue full (overflow)
     */
    bool enqueue(RawWsEvent&& event);
    
    /**
     * Register market data components for a ticker.
     * Must be called before start() or while processor is stopped.
     */
    void register_ticker(const Ticker& ticker,
                        OrderBook* ob,
                        TradeStream* ts,
                        LeaderTracker* lt,
                        FeatureExtractor* fe,
                        TickerController* ctrl);

    /**
     * Drain closed trades collected from the executor by the processor thread.
     * Called by the main thread on the slow tick.
     */
    void drain_closed_trades(std::vector<IExecutor::ClosedTrade>& out);
    
    /**
     * Set pipeline components (shared across all tickers).
     * Must be called before start().
     */
    void set_pipeline_components(SignalBus* signal_bus,
                                 StrategyEngine* strategy_engine,
                                 RiskManager* risk_manager,
                                 IExecutor* executor);
    
    /**
     * Get queue utilization (0.0 to 1.0) for monitoring.
     */
    double queue_utilization() const;
    
private:
    struct TickerComponents {
        OrderBook* book{nullptr};
        TradeStream* stream{nullptr};
        LeaderTracker* leader{nullptr};
        FeatureExtractor* features{nullptr};
        TickerController* ctrl{nullptr};

        // Rate limiting for feature extraction (event-driven but capped at feature_rate_hz)
        uint64_t next_emit_ns{0};
    };
    
    void run_();
    void process_event_(const RawWsEvent& event);
    void handle_orderbook_update_(simdjson::ondemand::document& doc);
    void handle_trade_update_(simdjson::ondemand::document& doc);
    void handle_mark_price_update_(simdjson::ondemand::document& doc);
    void trigger_feature_extraction_(const Ticker& ticker, TickerComponents& comp);
    
    Config cfg_;
    
    // Lock-free SPSC queue (WS thread → processor thread)
    boost::lockfree::spsc_queue<RawWsEvent> queue_;
    
    // Per-ticker components registry
    std::unordered_map<Ticker, TickerComponents> tickers_;
    
    // Shared pipeline components
    SignalBus* signal_bus_{nullptr};
    StrategyEngine* strategy_engine_{nullptr};
    RiskManager* risk_manager_{nullptr};
    IExecutor* executor_{nullptr};
    
    // Thread control
    std::atomic<bool> stop_{false};
    std::unique_ptr<std::thread> thread_;
    
    // Metrics
    std::atomic<uint64_t> events_processed_{0};
    std::atomic<uint64_t> events_dropped_{0};

    // Mark price cache — updated by handle_mark_price_update_, consumed by executor_
    absl::btree_map<Ticker, double> mark_prices_;

    // Closed trades drained from executor_ by processor thread, consumed by main thread
    MpmcQueue<IExecutor::ClosedTrade> closed_trade_queue_;
};

} // namespace trade_bot
