#include "BreakoutEatThrough.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

BreakoutEatThrough::BreakoutEatThrough(Ticker ticker, const Config& cfg)
    : ticker_(std::move(ticker))
    , name_("BreakoutEatThrough")
    , cfg_(cfg) {}

BreakoutEatThrough::BreakoutEatThrough(Ticker ticker)
    : BreakoutEatThrough(std::move(ticker), Config{}) {}

void BreakoutEatThrough::on_frame(const FeatureFrame& frame) {
    if (frame.ticker == ticker_) {
        ctx_.update(frame);
    }
}

void BreakoutEatThrough::on_signal(const Signal& signal) {
    if (signal.ticker == ticker_) {
        ctx_.update(signal);
    }
}

std::optional<TradePlan> BreakoutEatThrough::tick(std::chrono::system_clock::time_point now) {
    if (active_plan_) {
        if (now > active_plan_->valid_until) active_plan_ = std::nullopt;
        else return std::nullopt;
    }

    // 1. Check conditions C1-C5
    
    // C1: DensityEating
    auto it_eating = ctx_.recent_signals.find(SignalKind::DensityEating);
    if (it_eating == ctx_.recent_signals.end() || 
        (now - it_eating->second.timestamp) > std::chrono::seconds(3)) return std::nullopt;
    
    double density_price = it_eating->second.price;
    std::string side_str = it_eating->second.payload.value("side", "");
    Side breakout_side = (side_str == "Ask") ? Side::Buy : Side::Sell; // Eat Sell -> Buy move

    // C2: TapeBurst in direction
    auto it_burst = ctx_.recent_signals.find(SignalKind::TapeBurst);
    if (it_burst == ctx_.recent_signals.end() ||
        (now - it_burst->second.timestamp) > std::chrono::seconds(3)) return std::nullopt;
    
    std::string burst_side_str = it_burst->second.payload.value("side", "");
    if ((breakout_side == Side::Buy && burst_side_str != "Buy") ||
        (breakout_side == Side::Sell && burst_side_str != "Sell")) return std::nullopt;

    // C3: Support behind (DensityDetected or IcebergSuspected on our side)
    // Scan history for recent signals on our side
    auto it_support = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if ((now - sig.timestamp) > std::chrono::seconds(30)) return false;
        
        bool is_support = false;
        if (sig.kind == SignalKind::DensityDetected) {
            std::string s = sig.payload.value("side", "");
            if ((breakout_side == Side::Buy && s == "Bid") ||
                (breakout_side == Side::Sell && s == "Ask")) is_support = true;
        } else if (sig.kind == SignalKind::IcebergSuspected) {
             std::string s = sig.payload.value("side", "");
             if ((breakout_side == Side::Buy && s == "Bid") ||
                 (breakout_side == Side::Sell && s == "Ask")) is_support = true;
        }
        
        if (is_support) {
            double dist_bps = std::abs(sig.price - density_price) / density_price * kBpsBase;
            return dist_bps <= cfg_.support_search_range_bps;
        }
        return false;
    });
    
    if (it_support == ctx_.signal_history.rend()) return std::nullopt;
    double support_price = it_support->price;

    // Prices
    double entry_price = (breakout_side == Side::Buy) ? density_price + density_price * (cfg_.aggressive_offset_bps / kBpsBase)
                                                    : density_price - density_price * (cfg_.aggressive_offset_bps / kBpsBase);
    double stop_price = (breakout_side == Side::Buy) ? support_price - density_price * (cfg_.stop_buffer_bps / kBpsBase)
                                                    : support_price + density_price * (cfg_.stop_buffer_bps / kBpsBase);

    double risk_per_coin = std::abs(entry_price - stop_price);
    double tp1_price = (breakout_side == Side::Buy) ? entry_price + cfg_.tp1_r * risk_per_coin 
                                                   : entry_price - cfg_.tp1_r * risk_per_coin;

    TradePlan plan {
        .ticker = ticker_,
        .side = breakout_side,
        .entry_type = OrderType::Market,
        .entry_price = entry_price,
        .stop_price = stop_price,
        .tp1_price = tp1_price,
        .tp2_price = std::nullopt,
        .tp1_size_ratio = cfg_.tp1_size_ratio,
        .size_coin = 0.0,
        .risk_usd = 0.0,
        .strategy_name = name_,
        .reason = "Breakout through " + side_str + " density",
        .evidence = {it_eating->second, it_burst->second},
        .valid_until = now + cfg_.entry_timeout
    };

    active_plan_ = plan;
    return plan;
}

} // namespace trade_bot
