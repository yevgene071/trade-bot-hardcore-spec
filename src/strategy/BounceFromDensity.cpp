#include "BounceFromDensity.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

namespace {
double round_to_tick(double val, double tick) {
    if (tick <= 0) return val;
    return std::round(val / tick) * tick;
}
}

BounceFromDensity::BounceFromDensity(Ticker ticker, TickerInfo info, const Config& cfg)
    : ticker_(std::move(ticker))
    , info_(std::move(info))
    , name_("BounceFromDensity")
    , cfg_(cfg) {}

BounceFromDensity::BounceFromDensity(Ticker ticker, TickerInfo info)
    : BounceFromDensity(std::move(ticker), std::move(info), Config{}) {}

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

    // 1. Check conditions C1-C7
    const auto& frame = ctx_.last_frame;
    
    // C1: LevelApproach (most recent)
    auto it_approach = ctx_.recent_signals.find(SignalKind::LevelApproach);
    if (it_approach == ctx_.recent_signals.end() || 
        (now - it_approach->second.timestamp) > std::chrono::seconds(5)) return std::nullopt;
    
    double level_price = it_approach->second.price;
    
    // C2: Impulse approach requirement
    // "Подход к плотности должен быть быстрым и импульсным... в идеале безоткатным"
    double approach_speed = std::abs(frame.price_change_1s) * kBpsBase / 100.0;
    if (approach_speed < cfg_.min_approach_speed_bps_1s) return std::nullopt;

    // C3: DensityDetected on the same level
    auto it_density = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if (sig.kind != SignalKind::DensityDetected) return false;
        if ((now - sig.timestamp) > std::chrono::seconds(15)) return false;
        double dist_bps = std::abs(sig.price - level_price) / level_price * kBpsBase;
        return dist_bps <= 1.5; // 1.5 bps tolerance
    });
    if (it_density == ctx_.signal_history.rend()) return std::nullopt;

    // Relative density check
    std::string side_str = it_density->payload.value("side", "");
    double density_size = it_density->payload.value("size", 0.0);
    double book_depth = (side_str == "Bid") ? frame.bid_depth_10 : frame.ask_depth_10;
    if (book_depth > 0 && (density_size / book_depth) < cfg_.min_relative_density) return std::nullopt;

    // C4: Stalling check (Tape Speed Reduction)
    // "Скорость уменьшается... замирания в финале"
    double vol_1s = frame.buy_vol_1s + frame.sell_vol_1s;
    double vol_5s = frame.buy_vol_5s + frame.sell_vol_5s;
    if (vol_5s > 0) {
        double speed_ratio = (vol_1s / (vol_5s / 5.0)); // Normalize 5s to 1s
        if (speed_ratio > cfg_.max_tape_speed_ratio) return std::nullopt;
    }

    // C5: Driver (Leader) alignment
    // "Поводырь: Желательно чтобы шел в сторону отскока / Разворачивался"
    Side plan_side = (side_str == "Bid") ? Side::Buy : Side::Sell;
    double leader_move = (plan_side == Side::Buy) ? frame.leader_change_1s : -frame.leader_change_1s;
    if (leader_move < 0 && std::abs(leader_move * kBpsBase / 100.0) > cfg_.min_driver_reversal_bps) {
        // Driver is moving AGAINST the bounce (i.e. still going towards the density)
        return std::nullopt;
    }

    // C6: No Iceberg on this level
    auto it_iceberg = std::find_if(ctx_.signal_history.rbegin(), ctx_.signal_history.rend(), [&](const auto& sig) {
        if (sig.kind != SignalKind::IcebergSuspected) return false;
        if ((now - sig.timestamp) > std::chrono::seconds(30)) return false;
        double dist_bps = std::abs(sig.price - level_price) / level_price * kBpsBase;
        return dist_bps <= 2.0; 
    });
    if (it_iceberg != ctx_.signal_history.rend()) return std::nullopt;

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

    entry_price = round_to_tick(entry_price, info_.price_increment);
    stop_price = round_to_tick(stop_price, info_.price_increment);

    double risk_per_coin = std::abs(entry_price - stop_price);
    double tp1_price = (plan_side == Side::Buy) ? entry_price + cfg_.tp1_r * risk_per_coin 
                                               : entry_price - cfg_.tp1_r * risk_per_coin;
    tp1_price = round_to_tick(tp1_price, info_.price_increment);

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
        .reason = "Bounce from " + side_str + " density (Impulse + Stalling)",
        .evidence = {it_density->second, it_approach->second},
        .valid_until = now + cfg_.entry_timeout
    };

    active_plan_ = plan;
    return plan;
}

} // namespace trade_bot
