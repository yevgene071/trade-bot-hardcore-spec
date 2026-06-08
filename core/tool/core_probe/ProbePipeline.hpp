#pragma once

#include "CliOptions.hpp"
#include "SummaryCollector.hpp"
#include "InvariantChecker.hpp"
#include "TraceLogger.hpp"

#include "control/TickerController.hpp"
#include "executor/PaperExecutor.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "perf/LatencyTracer.hpp"
#include "perf/PerfRegistry.hpp"
#include "perf/TraceContext.hpp"
#include "perf/TraceTimeBuffer.hpp"
#include "risk/AccountState.hpp"
#include "risk/NewsCalendar.hpp"
#include "risk/RiskManager.hpp"
#include "signals/SignalBus.hpp"
#include "strategy/StrategyEngine.hpp"
#include "strategy/IStrategy.hpp"
#include "transport/Clocks.hpp"
#include "transport/IHttpClient.hpp"
#include "transport/ReplayFeed.hpp"
#include "universe/TickerUniverse.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <set>

namespace trade_bot::probe {

/// Central pipeline assembler that wires the full instrument chain
/// (book → features → signals → strategy → risk → executor) with
/// TraceEvent emission at every stage transition.  Follows the
/// perf_replay/main.cpp construction pattern but substitutes PerfRegistry
/// histograms with structured trace events for diagnostic observability.
class ProbePipeline {
public:
    explicit ProbePipeline(const CliOptions& opts);
    ~ProbePipeline();

    // Non-copyable, non-movable — holds self-referential callbacks
    ProbePipeline(const ProbePipeline&) = delete;
    ProbePipeline& operator=(const ProbePipeline&) = delete;

    // ── Accessors ────────────────────────────────────────────────────────────
    SignalBus&         signal_bus()       { return bus_; }
    StrategyEngine&    strategy_engine()  { return engine_; }
    RiskManager&       risk_manager()     { return risk_mgr_; }
    PaperExecutor&     paper_executor()   { return *paper_exec_; }
    SummaryCollector&  summary()          { return summary_; }
    InvariantChecker&  invariants()       { return inv_checker_; }
    AccountState&      account()          { return account_; }
    const std::map<Ticker, const OrderBook*>& books() const { return books_; }

    using LimitCallback = std::function<void()>;
    void set_limit_callback(LimitCallback cb) { limit_cb_ = std::move(cb); }
    const LimitCallback& limit_callback() const noexcept { return limit_cb_; }

    /// Returns IMarketDataListener pointers for connecting to a feed.
    /// The TickerControllers implement IMarketDataListener directly.
    std::vector<IMarketDataListener*> get_listeners();

    /// Drive one processing tick after market data has been applied.
    /// Extracts features, runs strategy engine, ticks executor, drains closes.
    void drive_tick(const Ticker& ticker, std::chrono::system_clock::time_point ts);

    /// Finalize: drain remaining executor state and compute final stats.
    void finalize();

private:
    static std::string signal_kind_name(SignalKind kind);
    static std::string reject_reason_name(RejectReason reason);

    /// Wire up signal bus subscription, strategy on_plan, and close callbacks.
    void setup_hooks();

    CliOptions       opts_;
    SummaryCollector summary_;
    InvariantChecker inv_checker_;
    std::shared_ptr<VirtualClock> clock_;

    // Offline infrastructure stubs (order matters: destroyed after dependents)
    std::unique_ptr<IHttpClient>            null_http_;
    std::unique_ptr<ClusterSnapshotClient>  null_cluster_client_;
    std::unique_ptr<ClusterSnapshotManager> cluster_mgr_;

    TickerUniverse universe_;
    NewsCalendar   news_;
    RiskManager    risk_mgr_;
    SignalBus      bus_;
    StrategyEngine engine_;

    std::map<Ticker, const OrderBook*>              books_;
    std::vector<std::unique_ptr<TickerController>>  controllers_;
    std::vector<std::unique_ptr<IMarketDataListener>> trace_listeners_;
    std::unique_ptr<PaperExecutor>                  paper_exec_;
    AccountState                                    account_;
    LimitCallback                                   limit_cb_;

    // Guard against double-close on the same ticker within one session
    std::set<std::string> closed_tickers_;
    bool finalized_{false};

    std::chrono::system_clock::time_point start_time_;
    std::chrono::system_clock::time_point end_time_;
    bool has_start_time_{false};
};

} // namespace trade_bot::probe
