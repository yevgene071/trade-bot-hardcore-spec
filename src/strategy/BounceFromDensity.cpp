#include "BounceFromDensity.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

BounceFromDensity::BounceFromDensity(Ticker ticker, const Config& cfg)
    : ticker_(std::move(ticker))
    , name_("BounceFromDensity")
    , cfg_(cfg) {}

BounceFromDensity::BounceFromDensity(Ticker ticker)
    : BounceFromDensity(std::move(ticker), Config{}) {}

void BounceFromDensity::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) {
        ctx_.update(frame);
    }
}

void BounceFromDensity::on_signal(const Signal& signal) {
    if (signal.ticker == ticker_) {
        ctx_.update(signal);
    }
}

std::optional<TradePlan> BounceFromDensity::tick(std::chrono::system_clock::time_point now) {
    if (active_plan_) {
        // TTL check
        if (now > active_plan_->valid_until) {
            active_plan_ = std::nullopt;
        } else {
            return std::nullopt;
        }
    }

    // 1. Check conditions C1-C6 (Pattern Recall: Scan history for matching levels)
    
    // C1: LevelApproach (most recent)
    auto it_approach = ctx_.recent_signals.find(SignalKind::LevelApproach);
    if (it_approach == ctx_.recent_signals.end() || 
        (now - it_approach->second.timestamp) > std::chrono::seconds(5)) return std::nullopt;
    
    double level_price = it_approach->second.price;
    
    // C3: DensityDetected on the same level (Search History)
    auto it_density = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if (sig.kind != SignalKind::DensityDetected) return false;
        if ((now - sig.timestamp) > std::chrono::seconds(15)) return false;
        double dist_bps = std::abs(sig.price - level_price) / level_price * kBpsBase;
        return dist_bps <= 1.5; // 1.5 bps tolerance
    });
    if (it_density == ctx_.signal_history.rend()) return std::nullopt;

    // C4: TapeFade
    auto it_fade = ctx_.recent_signals.find(SignalKind::TapeFade);
    if (it_fade == ctx_.recent_signals.end() ||
        (now - it_fade->second.timestamp) > std::chrono::seconds(5)) return std::nullopt;

    // C5: No Iceberg on this level
    auto it_iceberg = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if (sig.kind != SignalKind::IcebergSuspected) return false;
        if ((now - sig.timestamp) > std::chrono::seconds(30)) return false;
        double dist_bps = std::abs(sig.price - level_price) / level_price * kBpsBase;
        return dist_bps <= 2.0; 
    });
    if (it_iceberg != ctx_.signal_history.rend()) return std::nullopt;

    // Direction
    std::string side_str = it_density->payload.value("side", "");
    Side plan_side = Side::None;
    if (side_str == "Bid") plan_side = Side::Buy; // Bounce from support -> Long
    else if (side_str == "Ask") plan_side = Side::Sell; // Bounce from resistance -> Short
    else return std::nullopt;

    // Calculate Prices
    double entry_price, stop_price;
    double offset = level_price * (cfg_.entry_offset_bps / kBpsBase);
    double buffer = level_price * (cfg_.stop_buffer_bps / kBpsBase);

    if (plan_side == Side::Buy) {
        entry_price = level_price + offset;
        stop_price = level_price - buffer;
    } else {
        entry_price = level_price - offset;
        stop_price = level_price + buffer;
    }

    double risk_per_coin = std::abs(entry_price - stop_price);
    double tp1_price = (plan_side == Side::Buy) ? entry_price + cfg_.tp1_r * risk_per_coin 
                                               : entry_price - cfg_.tp1_r * risk_per_coin;

    TradePlan plan {
        .ticker = ticker_,
        .side = plan_side,
        .entry_type = OrderType::Limit,
        .entry_price = entry_price,
        .stop_price = stop_price,
        .tp1_price = tp1_price,
        .tp2_price = std::nullopt,
        .tp1_size_ratio = cfg_.tp1_size_ratio,
        .size_coin = 0.0,
        .risk_usd = 0.0,
        .strategy_name = name_,
        .reason = "Bounce from " + side_str + " density",
        .evidence = {it_density->second, it_approach->second, it_fade->second},
        .valid_until = now + cfg_.entry_timeout
    };

    active_plan_ = plan;
    return plan;
}

} // namespace trade_bot
