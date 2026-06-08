#include "PipelineProcessor.hpp"

#include "logger/Logger.hpp"

#include "control/TickerController.hpp"
#include "executor/IExecutor.hpp"
#include "features/FeatureExtractor.hpp"
#include "marketdata/LeaderTracker.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "perf/CpuPinning.hpp"
#include "perf/LatencyTracer.hpp"
#include "perf/PerfRegistry.hpp"
#include "risk/RiskManager.hpp"
#include "signals/SignalBus.hpp"
#include "strategy/StrategyEngine.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "utils/TickerSymbol.hpp"

#include <simdjson.h>
#include "config/Config.hpp"

#include <chrono>
#include <pthread.h>
#include <signal.h>
#include <thread>

namespace trade_bot {

namespace {
constexpr const char* kStageEventToBook = "event_to_book_us";
constexpr const char* kStageFeatureToSignal = "feature_to_signal_us";
constexpr const char* kStageSignalToStrategy = "signal_to_strategy_us";
constexpr const char* kStageStrategyToRisk = "strategy_to_risk_us";
constexpr const char* kStageRiskToSubmit = "risk_to_submit_us";
constexpr const char* kStageEndToEnd = "end_to_end_book_to_submit_us";

// Compatibility normalization for simple symbols. Spot/futures identity must be
// resolved by explicit connection/market mapping before entering the pipeline.
static Ticker normalize_ticker(std::string_view raw) {
    return to_internal_ticker(raw);
}
}

PipelineProcessor::PipelineProcessor(Config cfg)
    : cfg_(cfg)
    , queue_(cfg.spsc_queue_capacity) {
}

PipelineProcessor::~PipelineProcessor() {
    stop();
}

void PipelineProcessor::start() {
    if (thread_) {
        LOG_WARN("PipelineProcessor already started");
        return;
    }
    
    stop_.store(false, std::memory_order_release);
    thread_ = std::make_unique<std::thread>([this]() {
        // M2: Block SIGPIPE in worker threads; only main thread handles signals.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
        run_();
    });
    
    LOG_INFO("PipelineProcessor started on CPU {}", cfg_.hot_path_cpu);
}

void PipelineProcessor::stop() {
    if (!thread_) return;
    
    stop_.store(true, std::memory_order_release);
    if (thread_->joinable()) {
        thread_->join();
    }
    thread_.reset();
    
    LOG_INFO("PipelineProcessor stopped (processed={}, dropped={})",
             events_processed_.load(), events_dropped_.load());
}

void PipelineProcessor::drain_closed_trades(std::vector<IExecutor::ClosedTrade>& out) {
    IExecutor::ClosedTrade ct;
    while (closed_trade_queue_.try_pop(ct)) {
        out.push_back(std::move(ct));
    }
}

bool PipelineProcessor::enqueue(RawWsEvent&& event) {
    if (queue_.push(std::move(event))) {
        return true;
    }
    events_dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void PipelineProcessor::register_ticker(const Ticker& ticker,
                                       OrderBook* ob,
                                       TradeStream* ts,
                                       LeaderTracker* lt,
                                       FeatureExtractor* fe,
                                       TickerController* ctrl) {
    TickerComponents comp;
    comp.book = ob;
    comp.stream = ts;
    comp.leader = lt;
    comp.features = fe;
    comp.ctrl = ctrl;
    comp.next_emit_ns = 0;

    tickers_[ticker] = comp;
    LOG_DEBUG("PipelineProcessor: registered ticker {}", ticker);
}

void PipelineProcessor::set_pipeline_components(SignalBus* signal_bus,
                                               StrategyEngine* strategy_engine,
                                               RiskManager* risk_manager,
                                               IExecutor* executor) {
    signal_bus_ = signal_bus;
    strategy_engine_ = strategy_engine;
    risk_manager_ = risk_manager;
    executor_ = executor;
}

double PipelineProcessor::queue_utilization() const {
    // Approximate utilization (read_available is not exact but good enough for monitoring)
    return static_cast<double>(queue_.read_available()) / cfg_.spsc_queue_capacity;
}

void PipelineProcessor::run_() {
    // Pin thread to configured CPU
    if (!pin_thread_to_cpu(cfg_.hot_path_cpu)) {
        LOG_ERROR("Failed to pin processor thread to CPU {}", cfg_.hot_path_cpu);
    }
    
    LOG_INFO("PipelineProcessor thread running (CPU={})", cfg_.hot_path_cpu);
    
    [[maybe_unused]] uint64_t idle_count = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        // Busy-loop: drain queue as fast as possible
        RawWsEvent event;
        bool did_work = false;
        while (queue_.pop(event)) {
            process_event_(event);
            events_processed_.fetch_add(1, std::memory_order_relaxed);
            did_work = true;
        }

        // AL5: drain closed trades periodically regardless of idle state
        // to prevent unbounded accumulation under constant load.
        static thread_local uint64_t last_drain_count = 0;
        uint64_t processed = events_processed_.load(std::memory_order_relaxed);
        if (executor_ && (processed - last_drain_count >= 5000)) {
            for (auto& ct : executor_->pop_closed_trades()) {
                closed_trade_queue_.push(ct);
            }
            last_drain_count = processed;
        }
        if (did_work) idle_count = 0;

        // AL4: use CPU pause instead of yield to avoid scheduler ping on pinned core
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield" ::: "memory");
#else
        // After many empty spins, briefly yield to prevent 100% CPU burn
        if ((++idle_count & 0xFFF) == 0) {
            std::this_thread::yield();
        }
#endif
    }
    
    LOG_INFO("PipelineProcessor thread exiting");
}

void PipelineProcessor::process_event_(const RawWsEvent& event) {
    // Set trace context for this event
    TraceContext ctx(event.trace_id, event.recv_ns);
    
    // Thread-local simdjson parser (reused across events)
    thread_local simdjson::ondemand::parser parser;
    
    try {
        // Parse JSON using simdjson (zero-copy for hot messages)
        simdjson::padded_string_view json_view(
            event.payload.data(),
            event.payload_size,
            event.payload.size()
        );
        
        simdjson::ondemand::document doc;
        auto error = parser.iterate(json_view).get(doc);
        if (error) {
            LOG_ERROR("simdjson parse error: {}", simdjson::error_message(error));
            return;
        }
        
        // Determine message type
        std::string_view type_str;
        auto type_result = doc["Type"].get_string();
        if (type_result.error()) {
            // Fallback: try lowercase "type"
            type_result = doc["type"].get_string();
            if (type_result.error()) {
                LOG_WARN("Message missing 'Type' field");
                return;
            }
        }
        type_str = type_result.value();
        
        // Route to appropriate handler based on message type
        if (type_str == "orderbook_update") {
            handle_orderbook_update_(doc);
        } else if (type_str == "trade_update") {
            handle_trade_update_(doc);
        } else if (type_str == "mark_price_update") {
            handle_mark_price_update_(doc);
        } else {
            // Non-hot-path messages: fall back to nlohmann::json
            // (error, subscribed, balance_update, finres_update, etc.)
            // These are rare and don't need zero-copy optimization
            LOG_TRACE("Non-hot-path message type: {}", type_str);
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("PipelineProcessor event processing error: {}", e.what());
    }
}

void PipelineProcessor::handle_orderbook_update_(simdjson::ondemand::document& doc) {
    static auto& hist_event_to_book = PerfRegistry::instance().get_or_create(kStageEventToBook);
    static auto& hist_end_to_end = PerfRegistry::instance().get_or_create(trade_bot::kStageEndToEnd);
    
    LatencyTracer trace_event_to_book(hist_event_to_book);
    LatencyTracer trace_end_to_end(hist_end_to_end);
    
    // Extract ticker
    std::string_view ticker_str;
    auto ticker_result = doc["Ticker"].get_string();
    if (ticker_result.error()) {
        ticker_result = doc["ticker"].get_string();
        if (ticker_result.error()) return;
    }
    ticker_str = ticker_result.value();
    Ticker ticker = normalize_ticker(ticker_str);
    
    // Find ticker components
    auto it = tickers_.find(ticker);
    if (it == tickers_.end()) return;
    auto& comp = it->second;
    if (!comp.book) return;
    
    // Parse orderbook updates (simdjson zero-copy)
    auto updates_result = doc["Updates"].get_array();
    if (updates_result.error()) {
        updates_result = doc["updates"].get_array();
        if (updates_result.error()) return;
    }
    
    OrderBookUpdate upd;
    upd.ticker = ticker;
    
    // AL1: Use server timestamp for determinism
    std::string_view ts_str;
    auto ts_result = doc["Time"].get_string();
    if (ts_result.error()) ts_result = doc["time"].get_string();
    
    if (!ts_result.error()) {
        upd.ts = MetaScalpCodec::parse_timestamp(std::string(ts_result.value()));
    } else {
        upd.ts = std::chrono::system_clock::now();
    }

    for (auto update_val : updates_result.value()) {
        simdjson::ondemand::object update_obj;
        if (update_val.get_object().get(update_obj)) continue;

        double price = 0.0;
        double size = 0.0;
        int64_t side_int = 0;

        auto price_result = update_obj["Price"].get_double();
        if (!price_result.error()) price = price_result.value();

        auto size_result = update_obj["Size"].get_double();
        if (!size_result.error()) size = size_result.value();

        auto type_result = update_obj["Type"].get_int64();
        if (!type_result.error()) side_int = type_result.value();

        Side side = (side_int == 1) ? Side::Buy : (side_int == 2) ? Side::Sell : Side::None;
        if (side == Side::None) continue;

        upd.changes.push_back({price, size, side});
    }

    if (upd.changes.empty()) return;

    // Route through TickerController — applies to book AND runs all detectors
    comp.ctrl->on_book_update(upd);

    // trace_event_to_book stops automatically in destructor

    // Trigger feature extraction (rate-limited)
    trigger_feature_extraction_(ticker, comp);
}

void PipelineProcessor::handle_trade_update_(simdjson::ondemand::document& doc) {
    // Extract ticker
    std::string_view ticker_str;
    auto ticker_result = doc["Ticker"].get_string();
    if (ticker_result.error()) {
        ticker_result = doc["ticker"].get_string();
        if (ticker_result.error()) return;
    }
    ticker_str = ticker_result.value();
    Ticker ticker = normalize_ticker(ticker_str);
    
    // Find ticker components
    auto it = tickers_.find(ticker);
    if (it == tickers_.end()) return;
    auto& comp = it->second;
    if (!comp.stream) return;
    
    // Parse trades array
    auto trades_result = doc["Trades"].get_array();
    if (trades_result.error()) {
        trades_result = doc["trades"].get_array();
        if (trades_result.error()) return;
    }
    
    // AL2: Use server timestamp for determinism
    std::string_view ts_str;
    auto ts_result = doc["Time"].get_string();
    if (ts_result.error()) ts_result = doc["time"].get_string();
    
    std::chrono::system_clock::time_point now;
    if (!ts_result.error()) {
        now = MetaScalpCodec::parse_timestamp(std::string(ts_result.value()));
    } else {
        now = std::chrono::system_clock::now();
    }
    for (auto trade_val : trades_result.value()) {
        simdjson::ondemand::object trade_obj;
        if (trade_val.get_object().get(trade_obj)) continue;

        double price = 0.0;
        double size = 0.0;
        int64_t side_int = 0;

        auto price_result = trade_obj["Price"].get_double();
        if (!price_result.error()) price = price_result.value();

        auto size_result = trade_obj["Size"].get_double();
        if (!size_result.error()) size = size_result.value();

        auto side_result = trade_obj["Side"].get_int64();
        if (!side_result.error()) side_int = side_result.value();

        Side side = (side_int == 1) ? Side::Buy : (side_int == 2) ? Side::Sell : Side::None;
        if (side == Side::None) continue;

        // Route through TickerController — feeds TradeStream AND runs detectors
        Trade trade{price, size, side, now};
        comp.ctrl->on_trade(trade);
    }

    // Trigger feature extraction (rate-limited)
    trigger_feature_extraction_(ticker, comp);
}

void PipelineProcessor::handle_mark_price_update_(simdjson::ondemand::document& doc) {
    std::string_view ticker_str;
    auto ticker_result = doc["Ticker"].get_string();
    if (ticker_result.error()) {
        ticker_result = doc["ticker"].get_string();
        if (ticker_result.error()) return;
    }
    ticker_str = ticker_result.value();

    double price = 0.0;
    auto price_result = doc["Price"].get_double();
    if (price_result.error()) return;
    price = price_result.value();

    const Ticker ticker = normalize_ticker(ticker_str);
    mark_prices_[ticker] = price;
    if (executor_) executor_->set_mark_price(ticker, price);
}

void PipelineProcessor::trigger_feature_extraction_(const Ticker& /*ticker*/, TickerComponents& comp) {
    if (!comp.ctrl || !strategy_engine_) return;

    // Rate limiting: emit features at most feature_rate_hz times per second
    const uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    const uint64_t interval_ns = static_cast<uint64_t>(1e9 / cfg_.feature_rate_hz);
    if (now_ns < comp.next_emit_ns) {
        return;
    }
    comp.next_emit_ns = now_ns + interval_ns;

    // tick() extracts features, runs all detectors (on_frame), emits signals on SignalBus,
    // updates chart history, and returns the FeatureFrame — all under its own mutex.
    FeatureFrame frame = comp.ctrl->tick(std::chrono::system_clock::now());
    strategy_engine_->on_frame(frame);

    // Dynamic routing of leader frame to other tickers
    static const Ticker leader_ticker = normalize_ticker(
        ::trade_bot::Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));
    if (frame.ticker == leader_ticker) {
        for (auto& [t, other_comp] : tickers_) {
            if (t != leader_ticker && other_comp.ctrl) {
                other_comp.ctrl->on_leader_frame(frame);
            }
        }
    }
}

} // namespace trade_bot
