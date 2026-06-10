#pragma once

#include "domain/Types.hpp"
#include "perf/LatencyTracer.hpp"
#include "utils/FixedString.hpp"
#include <chrono>
#include <functional>
#include <optional>

namespace trade_bot {

enum class SignalKind {
    DensityDetected = 0,     // в стакане появилась плотность
    DensityRemoved = 1,      // плотность ушла (манипуляция)
    DensityEating = 2,       // плотность активно проедается
    IcebergSuspected = 3,    // видимый размер < реализованного
    TapeBurst = 4,           // всплеск агрессии в одну сторону
    TapeFade = 5,            // затухание ленты
    TapeFlush = 6,           // "прострел"
    TapeDistribution = 7,    // консолидация / раздача (низкая дисперсия + сохранённый объём)
    LevelFormed = 8,         // сформирован новый горизонтальный уровень
    LevelApproach = 9,       // подход к уровню
    LevelRejection = 10,     // отбой от уровня
    LevelBreak = 11,         // пробой уровня
    LeaderMove = 12,         // поводырь двинулся, а мы — нет
    DensityStack = 13        // завал плотностей (same-side cluster)
};

/**
 * T4-PERF: SignalPayload is now a POD-like struct instead of nlohmann::json.
 * This eliminates heap allocations in the hot path (on_frame/on_trade).
 */
struct SignalPayload {
    FixedString<8>  side = "";         // "Bid", "Ask", "Buy", "Sell"
    double          size = 0.0;
    double          size_usd = 0.0;
    double          total_eaten_usd = 0.0;
    double          original_size = 0.0;
    double          remaining_size = 0.0;
    double          eaten_ratio = 0.0;
    double          remaining_ratio = 0.0;
    double          first_price = 0.0;
    double          last_price = 0.0;
    double          width_bps = 0.0;
    double          total_size_usd = 0.0;
    double          stop_anchor_price = 0.0;
    double          lag_pct = 0.0;
    double          correlation = 0.0;
    double          dist_bps = 0.0;
    double          delta_bps = 0.0;
    double          leader_move_pct = 0.0;
    double          our_move_pct = 0.0;
    double          expected_move_pct = 0.0;
    double          lag_ms = 0.0;
    double          ratio = 0.0;
    double          intensity = 0.0;
    double          peak_rate = 0.0;
    double          current_rate = 0.0;
    double          cusum = 0.0;
    double          volatility_bps = 0.0;
    double          volume_usd_30s = 0.0;
    double          max_range_bps = 0.0;
    double          speed_bps = 0.0;
    FixedString<16> approach_type = "";
    int             touches = 0;
    int             prints = 0;
    int             age_ms = 0;
    int             refill_events = 0;
    bool            fake = false;
    FixedString<16> id = "";
    FixedString<16> source = "";

    bool operator==(const SignalPayload&) const = default;
};

struct Signal {
    SignalKind kind;
    std::chrono::system_clock::time_point timestamp;
    Ticker ticker;
    double price;                // цена, к которой привязан сигнал
    double confidence;           // [0, 1], внутренняя уверенность детектора
    SignalPayload payload;       // детали (размер плотности, окно расчёта...)
    TraceId trigger_trace_id{0}; // trace ID события, вызвавшего сигнал (для e2e latency)

    bool operator==(const Signal&) const = default;
};

} // namespace trade_bot

template <>
struct std::hash<trade_bot::SignalKind> {
    size_t operator()(trade_bot::SignalKind k) const noexcept {
        return std::hash<int>{}(static_cast<int>(k));
    }
};
