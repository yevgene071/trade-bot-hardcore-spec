#include "ProbePipeline.hpp"
#include "TraceLogger.hpp"
#include "TraceEvent.hpp"

#include "transport/IHttpClient.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "transport/ClusterSnapshotClient.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "perf/TraceContext.hpp"
#include "perf/TraceTimeBuffer.hpp"
#include "config/Config.hpp"

#include "strategy/BounceFromDensity.hpp"
#include "strategy/BreakoutEatThrough.hpp"
#include "strategy/LeaderLag.hpp"
#include "strategy/FlushReversal.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <print>

namespace trade_bot::probe {

using namespace trade_bot;
using namespace std::chrono;

// ── Offline stubs ────────────────────────────────────────────────────────────
// Same no-op pattern as perf_replay/main.cpp — all transport calls are inert
// during offline replay / synth modes.

namespace {

RiskManager::Config load_risk_config() {
    RiskManager::Config rm;
    rm.max_daily_loss_pct        = Config::get_or<double> ("risk.max_daily_loss_pct",        3.0);
    rm.max_concurrent_positions  = static_cast<int>(Config::get_or<int64_t>("risk.max_concurrent_positions",  3));
    rm.max_per_trade_risk_pct    = Config::get_or<double> ("risk.max_per_trade_risk_pct",    0.5);
    rm.max_position_value_pct    = Config::get_or<double> ("risk.max_position_value_pct",   10.0);
    rm.max_leverage              = static_cast<int>(Config::get_or<int64_t>("risk.max_leverage",              5));
    rm.news_blackout_min         = static_cast<int>(Config::get_or<int64_t>("risk.news_blackout_min",         5));
    rm.news_calendar_check_min   = static_cast<int>(Config::get_or<int64_t>("risk.news_calendar_check_min",  60));
    rm.news_calendar_require_fresh = Config::get_or<bool>("risk.news_calendar_require_fresh", false);
    rm.funding_blackout_pre_sec  = static_cast<int>(Config::get_or<int64_t>("risk.funding_blackout_pre_sec", 30));
    rm.funding_blackout_post_sec = static_cast<int>(Config::get_or<int64_t>("risk.funding_blackout_post_sec",30));
    rm.min_stop_bps              = Config::get_or<double> ("risk.min_stop_bps",              3.0);
    rm.max_stop_bps              = Config::get_or<double> ("risk.max_stop_bps",             20.0);
    rm.min_rr_ratio              = Config::get_or<double> ("risk.min_rr_ratio",              1.0);
    rm.max_positions_per_ticker  = static_cast<int>(Config::get_or<int64_t>("risk.max_positions_per_ticker",  1));
    rm.whitelist_tickers.push_back("SYNTH"); // Always whitelist synthetic ticker in probe runs
    rm.whitelist_tickers.push_back("BTC_USDT");
    rm.whitelist_tickers.push_back("ZEC_USDT");
    rm.whitelist_tickers.push_back("ZEC_BTC");
    return rm;
}

class NullHttpClient final : public IHttpClient {
public:
    HttpResponse get(const std::string&) override              { return {0, "", {}}; }
    HttpResponse post(const std::string&, const std::string&) override { return {0, "", {}}; }
    HttpResponse put(const std::string&, const std::string&) override  { return {0, "", {}}; }
    HttpResponse del(const std::string&) override              { return {0, "", {}}; }
    void set_timeout_ms(int) override {}
};

class BookTraceListener final : public IMarketDataListener {
public:
    BookTraceListener(ProbePipeline* pipeline, TickerController* target, InvariantChecker* inv, SummaryCollector* s, const CliOptions& opts)
        : pipeline_(pipeline), target_(target), inv_(inv), summary_(s), opts_(opts) {}

    void on_orderbook_snapshot(const OrderBookSnapshot& snap) override {
        if (snap.ticker != target_->book->ticker()) return;
        target_->on_orderbook_snapshot(snap);
        log_book(snap.ts, snap.asks.size() + snap.bids.size());
        pipeline_->drive_tick(snap.ticker, snap.ts);
        check_limit();
    }

    void on_orderbook_update(const OrderBookUpdate& upd) override {
        if (target_->book->ticker() == "ZEC_USDT") {
            static int zec_book_upd_count = 0;
            if (++zec_book_upd_count % 1000 == 0 || zec_book_upd_count < 5) {
                std::print(stderr, "[DEBUG listener on_orderbook_update] target=ZEC_USDT upd.ticker={} target_ticker={} count={}\n", upd.ticker, target_->book->ticker(), zec_book_upd_count);
            }
        }
        if (upd.ticker != target_->book->ticker()) return;
        target_->on_orderbook_update(upd);
        log_book(upd.ts, upd.changes.size());
        pipeline_->drive_tick(upd.ticker, upd.ts);
        check_limit();
    }

    void on_trade(const Ticker& tkr, const Trade& t) override {
        if (tkr != target_->book->ticker()) return;
        target_->on_trade(tkr, t);

        TraceEvent ev;
        ev.ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t.timestamp.time_since_epoch()).count());
        ev.trace_id = current_trace_context().trace_id;
        ev.ticker = tkr;
        ev.stage = "trades";
        ev.message = "trade price=" + std::to_string(t.price) + " size=" + std::to_string(t.size) + " side=" + (t.side == Side::Buy ? "Buy" : "Sell");
        ev.payload = {
            {"price", t.price},
            {"size", t.size},
            {"side", t.side == Side::Buy ? "Buy" : "Sell"}
        };
        TraceLogger::instance().enqueue(std::move(ev));
        pipeline_->drive_tick(tkr, t.timestamp);
        check_limit();
    }

    void on_trades(const Ticker& tkr, const std::vector<Trade>& trades) override {
        for (const auto& t : trades) {
            on_trade(tkr, t);
        }
    }

    void on_order_update(const OrderUpdate& u) override { target_->on_order_update(u); }
    void on_position_update(const PositionUpdate& u) override { target_->on_position_update(u); }
    void on_balance_update(const BalanceUpdate& u) override { target_->on_balance_update(u); }
    void on_finres_update(const FinresUpdate& u) override { target_->on_finres_update(u); }
    void on_error(const std::string& m) override { target_->on_error(m); }

private:
    void check_limit() {
        summary_->record_message();
        if (opts_.limit_messages > 0 && summary_->messages_parsed() >= opts_.limit_messages) {
            if (pipeline_->limit_callback()) {
                pipeline_->limit_callback()();
            }
        }
    }

    void log_book(std::chrono::system_clock::time_point ts, size_t delta_levels) {
        auto b = target_->book.get();
        double bid = b->best_bid().value_or(0.0);
        double ask = b->best_ask().value_or(0.0);
        double mid = b->mid().value_or(0.0);
        double spread = b->spread().value_or(0.0);
        double spread_bps = mid > 0.0 ? (spread / mid) * 10000.0 : 0.0;

        uint64_t trace_id = current_trace_context().trace_id;

        inv_->check_book(trace_id, target_->book->ticker(), bid, ask, mid, spread_bps);

        TraceEvent ev;
        ev.ts_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(ts.time_since_epoch()).count());
        ev.trace_id = trace_id;
        ev.ticker = target_->book->ticker();
        ev.stage = "book";
        ev.message = "mid=" + std::to_string(mid) + " spread_bps=" + std::to_string(spread_bps) + " bid=" + std::to_string(bid) + " ask=" + std::to_string(ask);
        ev.payload = {
            {"mid", mid},
            {"spread_bps", spread_bps},
            {"best_bid", bid},
            {"best_ask", ask},
            {"delta_levels", delta_levels}
        };
        TraceLogger::instance().enqueue(std::move(ev));
    }

    ProbePipeline* pipeline_;
    TickerController* target_;
    InvariantChecker* inv_;
    SummaryCollector* summary_;
    CliOptions opts_;
};

} // namespace



static uint64_t ts_to_ns(system_clock::time_point tp) noexcept {
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(tp.time_since_epoch()).count());
}

// ── Constructor ──────────────────────────────────────────────────────────────

ProbePipeline::ProbePipeline(const CliOptions& opts)
    : opts_(opts)
    , summary_()
    , inv_checker_(opts, &summary_)
    , clock_(std::make_shared<VirtualClock>())
    , null_http_(std::make_unique<NullHttpClient>())
    , null_cluster_client_(std::make_unique<ClusterSnapshotClient>(*null_http_, "", 0))
    , cluster_mgr_(std::make_unique<ClusterSnapshotManager>(*null_cluster_client_))
    , universe_()
    , news_()
    , risk_mgr_(universe_, news_, load_risk_config(), clock_)
    , bus_()
    , engine_(bus_, clock_)
{
    // Setup screener/volume stats fallback lookup in offline/replay mode
    universe_.set_stats_lookup([](const Ticker& t) -> std::optional<TickerStats> {
        TickerStats stats;
        if (t == "BTC_USDT") {
            stats.volume_24h_usd = 1'100'000'000.0;
        } else if (t == "ETH_USDT") {
            stats.volume_24h_usd = 400'000'000.0;
        } else {
            // Default altcoin volume stats for typical scaling
            stats.volume_24h_usd = 25'000'000.0;
        }
        return stats;
    });

    for (const auto& t : opts_.tickers) {
        universe_.cache_meta(t, TickerMeta{0.01, 1e-4, 0.001, 10000.0});
    }
    universe_.cache_meta("SYNTH", TickerMeta{0.01, 1e-4, 0.001, 10000.0});
    universe_.cache_meta("BTC_USDT", TickerMeta{0.01, 1e-4, 0.001, 10000.0});
    universe_.cache_meta("ZEC_USDT", TickerMeta{0.01, 1e-4, 0.001, 10000.0});
    universe_.cache_meta("ZEC_BTC", TickerMeta{0.01, 1e-4, 0.001, 10000.0});

    // Build per-ticker controllers, mirroring perf_replay/main.cpp
    Ticker leader_ticker = MetaScalpCodec::normalize_ticker(
        Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));
    for (const auto& t : opts_.tickers) {
        std::optional<Ticker> opt_leader = std::nullopt;
        if (t != leader_ticker) {
            opt_leader = leader_ticker;
        }
        auto ctrl = std::make_unique<TickerController>(t, bus_, universe_, *cluster_mgr_, opt_leader);
        books_[t] = ctrl->book.get();
        auto decorated = std::make_unique<BookTraceListener>(this, ctrl.get(), &inv_checker_, &summary_, opts_);
        trace_listeners_.push_back(std::move(decorated));
        controllers_.push_back(std::move(ctrl));
    }

    paper_exec_ = std::make_unique<PaperExecutor>(books_);

    account_.equity_usd          = opts_.equity_usd;
    account_.starting_equity_usd = opts_.equity_usd;
    account_.free_balance_usd    = opts_.equity_usd;

    // Initialize and scale trading strategies unless strategies are bypassed
    if (!opts_.no_strategy) {
        // Load base configurations (using defaults as standard fallbacks)
        BounceFromDensity::Config bounce_cfg;
        bounce_cfg.entry_offset_bps = Config::get_or<double>("strategies.bounce.entry_offset_bps", 3.0);
        bounce_cfg.stop_buffer_bps  = Config::get_or<double>("strategies.bounce.stop_buffer_bps",  5.0);
        bounce_cfg.max_spread_bps   = Config::get_or<double>("strategies.bounce.max_spread_bps",   3.0);
        bounce_cfg.tp1_size_ratio   = Config::get_or<double>("strategies.bounce.tp1_size_ratio",   0.5);
        bounce_cfg.burst_contra_exit_sec = std::chrono::seconds(
            Config::get_or<int64_t>("strategies.bounce.burst_contra_exit_sec", 5));
        bounce_cfg.min_density_age_ms = std::chrono::milliseconds(
            Config::get_or<int64_t>("strategies.bounce.min_density_age_ms", 5000));
        bounce_cfg.no_progress_timeout_sec = Config::get_or<double>("strategies.bounce.no_progress_timeout_sec", 120.0);

        BreakoutEatThrough::Config breakout_cfg;
        breakout_cfg.aggressive_offset_bps       = Config::get_or<double>("strategies.breakout.aggressive_offset_bps",       2.0);
        breakout_cfg.stop_buffer_bps             = Config::get_or<double>("strategies.breakout.stop_buffer_bps",             5.0);
        breakout_cfg.tp1_size_ratio              = Config::get_or<double>("strategies.breakout.tp1_size_ratio",              0.5);
        breakout_cfg.min_tape_aggression         = Config::get_or<double>("strategies.breakout.min_tape_aggression",         0.3);
        breakout_cfg.min_relative_volume         = Config::get_or<double>("strategies.breakout.min_relative_volume",         1.5);
        breakout_cfg.max_resistance_cluster_ratio = Config::get_or<double>("strategies.breakout.max_resistance_cluster_ratio", 0.7);
        breakout_cfg.fade_contra_exit_sec   = std::chrono::seconds(Config::get_or<int64_t>("strategies.breakout.fade_contra_exit_sec",   5));
        breakout_cfg.leader_contra_exit_sec = std::chrono::seconds(Config::get_or<int64_t>("strategies.breakout.leader_contra_exit_sec", 5));
        breakout_cfg.leader_exit_contra_pct = Config::get_or<double>("strategies.breakout.leader_exit_contra_pct", 0.15);
        breakout_cfg.no_progress_timeout_sec = Config::get_or<double>("strategies.breakout.no_progress_timeout_sec", 60.0);
        breakout_cfg.post_entry_grace_sec    = Config::get_or<double>("strategies.breakout.post_entry_grace_sec",    5.0);
        breakout_cfg.min_follow_through_bps  = Config::get_or<double>("strategies.breakout.min_follow_through_bps",  10.0);

        LeaderLag::Config leaderlag_cfg;
        leaderlag_cfg.stop_distance_bps          = Config::get_or<double>("strategies.leaderlag.stop_distance_bps",          8.0);
        leaderlag_cfg.tp1_size_ratio             = Config::get_or<double>("strategies.leaderlag.tp1_size_ratio",             0.6);
        leaderlag_cfg.correlation_exit_threshold = Config::get_or<double>("strategies.leaderlag.correlation_exit_threshold", 0.3);
        leaderlag_cfg.leader_exit_reversal_bps   = Config::get_or<double>("strategies.leaderlag.leader_exit_reversal_bps",   5.0);
        leaderlag_cfg.swing_lookback_ticks      = static_cast<int>(
            Config::get_or<int64_t>("strategies.leaderlag.swing_lookback_ticks", 30));
        leaderlag_cfg.no_progress_timeout_sec   = Config::get_or<double>("strategies.leaderlag.no_progress_timeout_sec", 15.0);

        FlushReversal::Config flushreversal_cfg;
        flushreversal_cfg.entry_offset_bps        = Config::get_or<double>("strategies.flushreversal.entry_offset_bps",        2.0);
        flushreversal_cfg.stop_buffer_bps         = Config::get_or<double>("strategies.flushreversal.stop_buffer_bps",         8.0);
        flushreversal_cfg.tp1_r                   = Config::get_or<double>("strategies.flushreversal.tp1_r",                   2.0);
        flushreversal_cfg.tp1_size_ratio          = Config::get_or<double>("strategies.flushreversal.tp1_size_ratio",          0.6);
        flushreversal_cfg.max_spread_bps          = Config::get_or<double>("strategies.flushreversal.max_spread_bps",          4.0);
        flushreversal_cfg.min_flush_dist_bps      = Config::get_or<double>("strategies.flushreversal.min_flush_dist_bps",      3.0);
        flushreversal_cfg.max_vol_fade_ratio      = Config::get_or<double>("strategies.flushreversal.max_vol_fade_ratio",      0.5);
        flushreversal_cfg.min_price_reversal_bps  = Config::get_or<double>("strategies.flushreversal.min_price_reversal_bps",  1.5);
        flushreversal_cfg.no_progress_timeout_sec = Config::get_or<double>("strategies.flushreversal.no_progress_timeout_sec", 30.0);
        flushreversal_cfg.min_level_touches = static_cast<int>(
            Config::get_or<int64_t>("strategies.flushreversal.min_level_touches", 2));

        // Parse list of enabled strategies
        std::set<std::string> active_strategies;
        if (opts_.strategies.empty()) {
            active_strategies = { "bounce", "breakout", "leaderlag", "flushreversal" };
        } else {
            for (const auto& s : opts_.strategies) {
                std::string lower_s = s;
                std::transform(lower_s.begin(), lower_s.end(), lower_s.begin(), ::tolower);
                active_strategies.insert(lower_s);
            }
        }

        // Add strategies for each ticker
        for (const auto& ticker : opts_.tickers) {
            auto meta = universe_.meta(ticker).value_or(TickerMeta{0.01, 1e-6, 0.0, 0.0});
            TickerInfo info{ticker, "", "", true, meta.price_increment, meta.size_increment, meta.min_size, meta.max_size};

            const double sf       = universe_.volume_scale_factor(ticker);
            const double sqrt_sf  = std::sqrt(sf);
            const double inv_sqrt = 1.0 / sqrt_sf;

            if (active_strategies.contains("bounce")) {
                auto cfg = bounce_cfg;
                cfg.stop_buffer_bps           *= inv_sqrt;
                cfg.entry_offset_bps          *= inv_sqrt;
                cfg.min_approach_speed_bps_1s *= sqrt_sf;
                cfg.no_progress_timeout_sec    = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 300.0);
                auto strat = std::make_unique<BounceFromDensity>(ticker, info, cfg, clock_);
                strat->set_universe(&universe_);
                engine_.add_strategy(std::move(strat));
            }
            if (active_strategies.contains("breakout")) {
                auto cfg = breakout_cfg;
                cfg.stop_buffer_bps          *= inv_sqrt;
                cfg.aggressive_offset_bps    *= inv_sqrt;
                cfg.min_relative_volume      *= sqrt_sf;
                cfg.no_progress_timeout_sec   = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 120.0);
                cfg.post_entry_grace_sec      *= inv_sqrt;
                auto strat = std::make_unique<BreakoutEatThrough>(ticker, info, cfg, clock_);
                engine_.add_strategy(std::move(strat));
            }
            if (active_strategies.contains("leaderlag")) {
                auto cfg = leaderlag_cfg;
                cfg.stop_distance_bps        *= inv_sqrt;
                cfg.no_progress_timeout_sec   = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 60.0);
                auto strat = std::make_unique<LeaderLag>(ticker, info, cfg, clock_);
                engine_.add_strategy(std::move(strat));
            }
            if (active_strategies.contains("flushreversal")) {
                auto cfg = flushreversal_cfg;
                cfg.stop_buffer_bps          *= inv_sqrt;
                cfg.entry_offset_bps         *= inv_sqrt;
                cfg.no_progress_timeout_sec   = std::min(cfg.no_progress_timeout_sec * inv_sqrt, 60.0);
                auto strat = std::make_unique<FlushReversal>(ticker, info, cfg, clock_);
                engine_.add_strategy(std::move(strat));
            }
        }
    }

    setup_hooks();
}

ProbePipeline::~ProbePipeline() = default;

// ── Hook wiring ──────────────────────────────────────────────────────────────

void ProbePipeline::setup_hooks() {
    // ── 1. Signal bus subscription ───────────────────────────────────────────
    bus_.subscribe([this](const Signal& sig) {
        auto kind_name = signal_kind_name(sig.kind);
        summary_.record_signal(kind_name);
        inv_checker_.check_signal(sig.trigger_trace_id, sig);

        TraceEvent ev;
        ev.ts_ns    = ts_to_ns(sig.timestamp);
        ev.trace_id = sig.trigger_trace_id;
        ev.ticker   = sig.ticker;
        ev.stage    = "signal";
        ev.message  = kind_name
                    + " price=" + std::to_string(sig.price)
                    + " conf="  + std::to_string(sig.confidence);
        ev.payload  = {
            {"kind",      kind_name},
            {"price",     sig.price},
            {"confidence", sig.confidence},
            {"side",      std::string(sig.payload.side.c_str())},
            {"size_usd",  sig.payload.size_usd}
        };
        TraceLogger::instance().enqueue(std::move(ev));
    });

    // ── 2. Strategy engine on_plan callback ──────────────────────────────────
    engine_.set_on_plan([this](const TradePlan& plan) {
        std::string strat_name(plan.strategy_name.c_str());
        summary_.record_plan(strat_name);
        inv_checker_.check_plan(plan.trace_id, plan);

        // Emit strategy trace event
        {
            TraceEvent sev;
            sev.ts_ns    = ts_to_ns(clock_->now());
            sev.trace_id = plan.trace_id;
            sev.ticker   = plan.ticker;
            sev.stage    = "strategy";
            sev.message  = strat_name
                         + " side="  + (plan.side == Side::Buy ? "Buy" : "Sell")
                         + " entry=" + std::to_string(plan.entry_price)
                         + " stop="  + std::to_string(plan.stop_price)
                         + " tp1="   + std::to_string(plan.tp1_price);
            sev.payload = {
                {"strategy",    strat_name},
                {"side",        plan.side == Side::Buy ? "Buy" : "Sell"},
                {"entry_price", plan.entry_price},
                {"stop_price",  plan.stop_price},
                {"tp1_price",   plan.tp1_price},
                {"size_coin",   plan.size_coin}
            };
            TraceLogger::instance().enqueue(std::move(sev));
        }

        // ── No-risk fast path ────────────────────────────────────────────────
        if (opts_.no_risk) {
            summary_.record_risk_decision(true, "None");

            if (!opts_.no_executor) {
                paper_exec_->submit(plan);
                summary_.record_submit();

                TraceEvent eev;
                eev.ts_ns    = ts_to_ns(clock_->now());
                eev.trace_id = plan.trace_id;
                eev.ticker   = plan.ticker;
                eev.stage    = "executor";
                eev.message  = "submit (risk bypassed)";
                eev.payload  = {
                    {"strategy",    strat_name},
                    {"entry_price", plan.entry_price},
                    {"size_coin",   plan.size_coin},
                    {"risk_bypass", true}
                };
                TraceLogger::instance().enqueue(std::move(eev));
            }
            return;
        }

        // ── Risk evaluation ──────────────────────────────────────────────────
        RiskDecision decision = risk_mgr_.evaluate(plan, account_);
        auto reason_name = reject_reason_name(decision.reason);
        summary_.record_risk_decision(decision.accepted, reason_name);
        inv_checker_.check_risk(plan.trace_id, plan.ticker, decision.accepted, reason_name);

        // Emit risk trace event
        {
            TraceEvent rev;
            rev.ts_ns    = ts_to_ns(clock_->now());
            rev.trace_id = plan.trace_id;
            rev.ticker   = plan.ticker;
            rev.stage    = "risk";
            rev.severity = decision.accepted ? "info" : "warn";
            rev.message  = std::string(decision.accepted ? "ACCEPTED" : "REJECTED")
                         + " reason=" + reason_name
                         + " risk_usd=" + std::to_string(decision.risk_usd);
            rev.payload = {
                {"accepted",           decision.accepted},
                {"reason",             reason_name},
                {"risk_usd",           decision.risk_usd},
                {"adjusted_size_coin", decision.adjusted_size_coin},
                {"details",            decision.details}
            };
            TraceLogger::instance().enqueue(std::move(rev));
        }

        // Submit to executor if accepted, or if risk_observe mode (log-only rejections)
        if (decision.accepted || opts_.risk_observe) {
            if (!opts_.no_executor) {
                auto adjusted_plan = plan;
                if (decision.accepted) {
                    adjusted_plan.size_coin = decision.adjusted_size_coin;
                }
                paper_exec_->submit(adjusted_plan);
                summary_.record_submit();

                TraceEvent eev;
                eev.ts_ns    = ts_to_ns(clock_->now());
                eev.trace_id = plan.trace_id;
                eev.ticker   = plan.ticker;
                eev.stage    = "executor";
                eev.message  = "submit"
                             + std::string(opts_.risk_observe && !decision.accepted
                                           ? " (risk_observe override)" : "");
                eev.payload = {
                    {"strategy",           strat_name},
                    {"entry_price",        adjusted_plan.entry_price},
                    {"size_coin",          adjusted_plan.size_coin},
                    {"risk_observe",       opts_.risk_observe && !decision.accepted}
                };
                TraceLogger::instance().enqueue(std::move(eev));
            }
        }
    });

    // ── 3. Strategy close callback (invalidation-driven exits) ───────────────
    engine_.set_close_callback([this](const Ticker& ticker, const FixedString<32>& reason) {
        if (opts_.no_executor) return;

        paper_exec_->close_trade(ticker, reason);

        TraceEvent cev;
        cev.ts_ns    = ts_to_ns(clock_->now());
        cev.ticker   = ticker;
        cev.stage    = "executor";
        cev.message  = "close_trade reason=" + std::string(reason.c_str());
        cev.payload  = {
            {"action", "close"},
            {"reason", std::string(reason.c_str())}
        };
        TraceLogger::instance().enqueue(std::move(cev));
    });
}

// ── drive_tick ───────────────────────────────────────────────────────────────

void ProbePipeline::drive_tick(const Ticker& ticker, system_clock::time_point ts) {
    if (!has_start_time_) {
        start_time_ = ts;
        has_start_time_ = true;
    }
    end_time_ = ts;
    clock_->set(ts);

    // Find the controller for this ticker
    TickerController* ctrl = nullptr;
    for (auto& c : controllers_) {
        if (c->book->ticker() == ticker) {
            ctrl = c.get();
            break;
        }
    }
    if (!ctrl) return;

    // Extract features
    auto frame = ctrl->tick(ts);

    inv_checker_.check_features(frame.derived_from, ticker, frame);

    // Emit features trace event (verbose-only by convention, but logger filters it)
    {
        TraceEvent fev;
        fev.ts_ns    = ts_to_ns(ts);
        fev.trace_id = frame.derived_from;
        fev.ticker   = ticker;
        fev.stage    = "features";
        fev.message  = "mid=" + std::to_string(frame.mid)
                     + " spread=" + std::to_string(frame.spread_bps) + "bps"
                     + " imb=" + std::to_string(frame.imbalance);
        fev.payload = {
            {"mid",            frame.mid},
            {"spread_bps",     frame.spread_bps},
            {"imbalance",      frame.imbalance},
            {"bid_depth_10",   frame.bid_depth_10},
            {"ask_depth_10",   frame.ask_depth_10},
            {"volatility_bps", frame.volatility_1min_bps}
        };
        TraceLogger::instance().enqueue(std::move(fev));
    }

    // Drive strategy engine
    engine_.on_frame(frame);

    // Dynamic routing of leader frame to other tickers
    static const Ticker leader_ticker = MetaScalpCodec::normalize_ticker(
        ::trade_bot::Config::get_or<std::string>("universe.affinity.leaderlag.require_leader", "BTC_USDT"));
    if (frame.ticker == leader_ticker) {
        for (auto& c : controllers_) {
            if (c->book->ticker() != leader_ticker) {
                c->on_leader_frame(frame);
            }
        }
    }

    engine_.tick(ts);

    // Tick executor
    if (!opts_.no_executor) {
        paper_exec_->tick(ts);

        // Drain closed trades → emit events and update account
        auto closed = paper_exec_->pop_closed_trades();
        for (const auto& ct : closed) {
            bool is_loss = ct.pnl_usd < 0.0;
            summary_.record_close(ct.pnl_usd, is_loss);
            risk_mgr_.record_trade_end(is_loss, ts);

            account_.realized_pnl_today_usd += ct.pnl_usd;
            account_.equity_usd              = account_.starting_equity_usd + account_.realized_pnl_today_usd;
            account_.free_balance_usd        = account_.equity_usd;

            TraceEvent tev;
            tev.ts_ns    = ts_to_ns(ts);
            tev.trace_id = ct.plan.trace_id;
            tev.ticker   = ct.plan.ticker;
            tev.stage    = "executor";
            tev.severity = is_loss ? "warn" : "info";
            tev.message  = "closed"
                         + std::string(" pnl=") + std::to_string(ct.pnl_usd)
                         + " entry=" + std::to_string(ct.entry_price)
                         + " exit="  + std::to_string(ct.exit_price)
                         + " reason=" + std::string(ct.reason.c_str());
            tev.payload = {
                {"action",      "closed"},
                {"pnl_usd",     ct.pnl_usd},
                {"entry_price", ct.entry_price},
                {"exit_price",  ct.exit_price},
                {"size_filled", ct.size_filled},
                {"reason",      std::string(ct.reason.c_str())},
                {"strategy",    std::string(ct.plan.strategy_name.c_str())}
            };
            TraceLogger::instance().enqueue(std::move(tev));

            // Emit account state event
            TraceEvent aev;
            aev.ts_ns  = ts_to_ns(ts);
            aev.ticker = ct.plan.ticker;
            aev.stage  = "account";
            aev.message = "equity=" + std::to_string(account_.equity_usd)
                        + " realized_pnl=" + std::to_string(account_.realized_pnl_today_usd);
            aev.payload = {
                {"equity_usd",          account_.equity_usd},
                {"starting_equity_usd", account_.starting_equity_usd},
                {"realized_pnl_usd",    account_.realized_pnl_today_usd},
                {"free_balance_usd",    account_.free_balance_usd}
            };
            TraceLogger::instance().enqueue(std::move(aev));
        }
    }
}

// ── get_listeners ────────────────────────────────────────────────────────────

std::vector<IMarketDataListener*> ProbePipeline::get_listeners() {
    std::vector<IMarketDataListener*> out;
    out.reserve(trace_listeners_.size());
    for (auto& tl : trace_listeners_) {
        out.push_back(tl.get());
    }
    return out;
}

// ── finalize ─────────────────────────────────────────────────────────────────

void ProbePipeline::finalize() {
    // Final tick on executor to flush any pending fills at virtual clock's now()
    if (!opts_.no_executor) {
        auto now = clock_->now();
        paper_exec_->tick(now);

        auto closed = paper_exec_->pop_closed_trades();
        for (const auto& ct : closed) {
            bool is_loss = ct.pnl_usd < 0.0;
            summary_.record_close(ct.pnl_usd, is_loss);
            account_.realized_pnl_today_usd += ct.pnl_usd;
            account_.equity_usd = account_.starting_equity_usd + account_.realized_pnl_today_usd;
            account_.free_balance_usd = account_.equity_usd;
        }
    }

    // Populate start/end timestamps and market duration
    if (has_start_time_) {
        summary_.set_start_time(ts_to_ns(start_time_));
        summary_.set_end_time(ts_to_ns(end_time_));
        double duration_sec = std::chrono::duration<double>(end_time_ - start_time_).count();
        summary_.set_market_duration_sec(duration_sec);
    }

    // Emit summary trace event using virtual clock time
    TraceEvent sev;
    sev.ts_ns  = ts_to_ns(clock_->now());
    sev.stage  = "summary";
    sev.message = "Pipeline finalized";
    sev.payload = {
        {"total_signals",  summary_.total_signals()},
        {"total_plans",    summary_.total_plans()},
        {"total_submits",  summary_.total_submits()},
        {"total_closes",   summary_.total_closes()},
        {"total_pnl_usd",  account_.realized_pnl_today_usd},
        {"final_equity",   account_.equity_usd}
    };
    TraceLogger::instance().enqueue(std::move(sev));
}

// ── Enum → string helpers ────────────────────────────────────────────────────

std::string ProbePipeline::signal_kind_name(SignalKind kind) {
    switch (kind) {
        case SignalKind::DensityDetected:  return "DensityDetected";
        case SignalKind::DensityRemoved:   return "DensityRemoved";
        case SignalKind::DensityEating:    return "DensityEating";
        case SignalKind::IcebergSuspected: return "IcebergSuspected";
        case SignalKind::TapeBurst:        return "TapeBurst";
        case SignalKind::TapeFade:         return "TapeFade";
        case SignalKind::TapeFlush:        return "TapeFlush";
        case SignalKind::TapeDistribution: return "TapeDistribution";
        case SignalKind::LevelFormed:      return "LevelFormed";
        case SignalKind::LevelApproach:    return "LevelApproach";
        case SignalKind::LevelRejection:   return "LevelRejection";
        case SignalKind::LevelBreak:       return "LevelBreak";
        case SignalKind::LeaderMove:       return "LeaderMove";
    }
    return "Unknown";
}

std::string ProbePipeline::reject_reason_name(RejectReason reason) {
    switch (reason) {
        case RejectReason::None:                       return "None";
        case RejectReason::KillSwitchActive:           return "KillSwitchActive";
        case RejectReason::DailyLossLimitHit:          return "DailyLossLimitHit";
        case RejectReason::TooManyPositions:           return "TooManyPositions";
        case RejectReason::DuplicatePosition:          return "DuplicatePosition";
        case RejectReason::NotInUniverse:              return "NotInUniverse";
        case RejectReason::StopTooTight:               return "StopTooTight";
        case RejectReason::StopTooWide:                return "StopTooWide";
        case RejectReason::InvalidStopSide:            return "InvalidStopSide";
        case RejectReason::PoorRewardRisk:             return "PoorRewardRisk";
        case RejectReason::SizeBelowMinimum:           return "SizeBelowMinimum";
        case RejectReason::InsufficientMargin:         return "InsufficientMargin";
        case RejectReason::TradeRateLimitHit:          return "TradeRateLimitHit";
        case RejectReason::LossStreakCircuitBreaker:   return "LossStreakCircuitBreaker";
        case RejectReason::NewsBlackout:               return "NewsBlackout";
        case RejectReason::FundingBlackout:            return "FundingBlackout";
        case RejectReason::SinglePositionLossExceeded: return "SinglePositionLossExceeded";
        case RejectReason::EntrySlippageExceeded:      return "EntrySlippageExceeded";
        case RejectReason::InternalError:              return "InternalError";
    }
    return "Unknown";
}

} // namespace trade_bot::probe
