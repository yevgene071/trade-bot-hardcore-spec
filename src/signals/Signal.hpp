#pragma once

#include "domain/Types.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <string>

namespace trade_bot {

enum class SignalKind {
    DensityDetected,     // в стакане появилась плотность
    DensityRemoved,      // плотность ушла (манипуляция)
    DensityEating,       // плотность активно проедается
    IcebergSuspected,    // видимый размер < реализованного
    TapeBurst,           // всплеск агрессии в одну сторону
    TapeFade,            // затухание ленты
    TapeFlush,           // "прострел"
    TapeDistribution,    // консолидация / раздача (низкая дисперсия + сохранённый объём)
    LevelFormed,         // сформирован новый горизонтальный уровень
    LevelApproach,       // подход к уровню
    LevelRejection,      // отбой от уровня
    LevelBreak,          // пробой уровня
    LeaderMove           // поводырь двинулся, а мы — нет
};

struct Signal {
    SignalKind kind;
    std::chrono::system_clock::time_point timestamp;
    Ticker ticker;
    double price;                // цена, к которой привязан сигнал
    double confidence;           // [0, 1], внутренняя уверенность детектора
    nlohmann::json payload;      // детали (размер плотности, окно расчёта...)

    bool operator==(const Signal&) const = default;
};

} // namespace trade_bot
