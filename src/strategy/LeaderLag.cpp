#include "LeaderLag.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

namespace {
double round_to_tick(double val, double tick) {
    if (tick <= 0) return val;
    return std::round(val / tick) * tick;
}
}

LeaderLag::LeaderLag(Ticker ticker, TickerInfo info, const Config& cfg)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("LeaderLag")
    , cfg_(cfg) {}

LeaderLag::LeaderLag(Ticker ticker, TickerInfo info)
    : LeaderLag(std::move(ticker), std::move(info), Config{}) {}

void LeaderLag::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) {
        ctx_.update(frame);
    }
}

void LeaderLag::on_signal(const Signal& signal) {
    if (signal.ticker == ticker_) {
        ctx_.update(signal);
    }
}

std::optional<TradePlan> LeaderLag::tick(std::chrono::system_clock::time_point now) {
    if (active_plan_) {
        if (now > active_plan_->valid_until) active_plan_ = std::nullopt;
        else return std::nullopt;
    }

    // 1. Check conditions C1-C6
    
    // C1: LeaderMove
    auto it_move = ctx_.recent_signals.find(SignalKind::LeaderMove);
    if (it_move == ctx_.recent_signals.end() || 
        (now - it_move->second.timestamp) > cfg_.lag_max_age) return std::nullopt;
    
    double lag_pct = it_move->second.payload.value("lag_pct", 0.0);
    double corr = it_move->second.payload.value("correlation", 0.0);
    Side plan_side = (lag_pct > 0) ? Side::Buy : Side::Sell;

    // C3: Rolling correlation
    if (std::abs(corr) < cfg_.min_correlation) return std::nullopt;

    // C6: Spread
    double spread_bps = (ctx_.last_frame.best_ask - ctx_.last_frame.best_bid) / ctx_.last_frame.best_bid * 10000.0;
    if (spread_bps > cfg_.max_spread_bps) return std::nullopt;

    // Calculate Prices
    double mid = ctx_.last_frame.mid;
    double entry_price = round_to_tick(mid, info_.price_increment);
    double stop_dist = mid * (cfg_.stop_distance_bps / 10000.0);
    double stop_price = round_to_tick((plan_side == Side::Buy) ? mid - stop_dist : mid + stop_dist, info_.price_increment);

    double expected_catchup_bps = std::abs(lag_pct) * 100.0;
    double tp1_offset = mid * (expected_catchup_bps * cfg_.tp1_catchup_ratio / 10000.0);
    double tp1_price = round_to_tick((plan_side == Side::Buy) ? mid + tp1_offset : mid - tp1_offset, info_.price_increment);

    TradePlan plan {
        .ticker = ticker_,
        .side = plan_side,
        .entry_type = OrderType::Market,
        .entry_price = entry_price,
        .stop_price = stop_price,
        .tp1_price = tp1_price,
        .tp2_price = std::nullopt,
        .tp1_size_ratio = cfg_.tp1_size_ratio,
        .size_coin = 0.0,
        .risk_usd = 0.0,
        .strategy_name = name_,
        .reason = "Leader lag detected",
        .evidence = {it_move->second},
        .valid_until = now + cfg_.entry_timeout
    };

    active_plan_ = plan;
    return plan;
}

} // namespace trade_bot
