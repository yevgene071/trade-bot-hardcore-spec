#include "BreakoutEatThrough.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

namespace {
double round_to_tick(double val, double tick) {
    if (tick <= 0) return val;
    return std::round(val / tick) * tick;
}
}

BreakoutEatThrough::BreakoutEatThrough(Ticker ticker, TickerInfo info, const Config& cfg)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("BreakoutEatThrough")
    , cfg_(cfg) {}

BreakoutEatThrough::BreakoutEatThrough(Ticker ticker, TickerInfo info)
    : BreakoutEatThrough(std::move(ticker), std::move(info), Config{}) {}

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
    const auto& frame = ctx_.last_frame;
    
    // C1: DensityEating
    auto it_eating = ctx_.recent_signals.find(SignalKind::DensityEating);
    if (it_eating == ctx_.recent_signals.end() || 
        (now - it_eating->second.timestamp) > std::chrono::seconds(3)) return std::nullopt;
    
    double density_price = it_eating->second.price;
    double density_original_size = it_eating->second.payload.value("original_size", 0.0);
    double density_remaining_size = it_eating->second.payload.value("remaining_size", 0.0);
    
    // "Заходим когда от плотности останется 1/2 - 1/3"
    // If we have size info, ensure it's sufficiently eaten but not gone yet
    if (density_original_size > 0 && density_remaining_size > 0) {
        double ratio = density_remaining_size / density_original_size;
        if (ratio > 0.5) return std::nullopt; // Too early
    }

    std::string side_str = it_eating->second.payload.value("side", "");
    Side breakout_side = (side_str == "Ask") ? Side::Buy : Side::Sell; // Eat Sell -> Buy move

    // C2: TapeBurst & Aggression
    // "Активная лента... без просаживаний... объем обратных принтов недостаточен"
    auto it_burst = ctx_.recent_signals.find(SignalKind::TapeBurst);
    if (it_burst == ctx_.recent_signals.end() ||
        (now - it_burst->second.timestamp) > std::chrono::seconds(3)) return std::nullopt;
    
    std::string burst_side_str = it_burst->second.payload.value("side", "");
    if ((breakout_side == Side::Buy && burst_side_str != "Buy") ||
        (breakout_side == Side::Sell && burst_side_str != "Sell")) return std::nullopt;

    // Tape Aggression check
    double aggression = (breakout_side == Side::Buy) ? frame.tape_aggression : -frame.tape_aggression;
    if (aggression < cfg_.min_tape_aggression) return std::nullopt;

    // C3: Volume participation
    // "Нарастание объемов... перед потенциальным пробоем"
    double current_vol = (breakout_side == Side::Buy) ? frame.buy_vol_5s : frame.sell_vol_5s;
    double avg_vol = (breakout_side == Side::Buy) ? (frame.buy_vol_30s / 6.0) : (frame.sell_vol_30s / 6.0);
    if (avg_vol > 0 && (current_vol / avg_vol) < cfg_.min_relative_volume) return std::nullopt;

    // C4: Driver (Leader) alignment
    // "Резкое движение поводыря в направлении противоположному пробою... может произойти отскок"
    double leader_dir = (breakout_side == Side::Buy) ? frame.leader_change_1s : -frame.leader_change_1s;
    if (frame.leader_correlation > cfg_.min_leader_correlation && leader_dir < 0) return std::nullopt;

    // C5: Resistance clusters check
    // "В сопротивление - это когда в сопротивление стоит большое скопление небольших лимитных заявок... пробой врядли будет импульсным"
    double resistance_vol = 0.0;
    for (const auto& sig : ctx_.signal_history) {
        if ((now - sig.timestamp) > std::chrono::seconds(10)) continue;
        if (sig.kind == SignalKind::DensityDetected) { // Assuming ResistanceCluster is tagged as DensityDetected with small sizes or a specific tag
             std::string s = sig.payload.value("side", "");
             bool is_resistance = (breakout_side == Side::Buy && s == "Ask" && sig.price > density_price) ||
                                  (breakout_side == Side::Sell && s == "Bid" && sig.price < density_price);
             if (is_resistance) {
                 resistance_vol += sig.payload.value("size", 0.0);
             }
        }
    }
    if (density_original_size > 0 && (resistance_vol / density_original_size) > cfg_.max_resistance_cluster_ratio) {
        return std::nullopt;
    }

    // C6: Support behind (DensityDetected or IcebergSuspected on our side)
    // "Чем больше подставляют поддержек в нашу сторону... тем нам стоять спокойнее"
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
    entry_price = round_to_tick(entry_price, info_.price_increment);

    double stop_price = (breakout_side == Side::Buy) ? support_price - density_price * (cfg_.stop_buffer_bps / kBpsBase)
                                                    : support_price + density_price * (cfg_.stop_buffer_bps / kBpsBase);
    stop_price = round_to_tick(stop_price, info_.price_increment);

    double risk_per_coin = std::abs(entry_price - stop_price);
    double tp1_price = (breakout_side == Side::Buy) ? entry_price + cfg_.tp1_r * risk_per_coin 
                                                   : entry_price - cfg_.tp1_r * risk_per_coin;
    tp1_price = round_to_tick(tp1_price, info_.price_increment);

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
