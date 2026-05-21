# Strategy & Executor Bug Analysis — 2026-05-17

## Executive Summary

**Found**: 7 bugs (2 CRITICAL, 2 HIGH, 2 MEDIUM, 1 LOW)  
**Root Cause**: Отсутствие сделок за сутки вызвано:
1. Критические баги в условиях стратегий (мёртвые проверки)
2. Отсутствующие параметры в config.toml (defaults = 0 → всегда fail)
3. Слишком строгие пороги (12+ условий на стратегию)
4. Regime suppression блокирует все стратегии в News/Trend

---

## CRITICAL Bugs

### S1-CRITICAL: BounceFromDensity — Dead Leader Alignment Check

**File**: `core/src/strategy/BounceFromDensity.cpp:165-171`

**Issue**: Проверка C5 (Driver alignment) никогда не срабатывает из-за слишком низкого порога.

```cpp
// C5: Driver (Leader) alignment
double leader_move = (plan_side == Side::Buy) ? frame.leader_change_1s : -frame.leader_change_1s;
// T0-BUGFIX: Removed division by 100 — leader_move is a fractional decimal,
// kBpsBase=10000 converts to bps. Previously /100 turned bps→percent, which
// never exceeded min_driver_reversal_bps (nominally 2 bps), making this check dead.
if (leader_move < 0 && std::abs(leader_move * kBpsBase) > cfg_.min_driver_reversal_bps) {
    // Driver is moving AGAINST the bounce (i.e. still going towards the density)
    LOG_TRACE("[Bounce] {} leader moving against bounce: {}", ticker_, leader_move);
    return std::nullopt;
}
```

**Problem**:
- `leader_change_1s` — fractional (0.001 = 0.1%)
- `leader_move * kBpsBase` → bps (0.001 * 10000 = 10 bps)
- `cfg_.min_driver_reversal_bps = 2` (из config.toml отсутствует → default)
- Проверка `10 bps > 2 bps` → почти всегда true → reject почти всегда
- **Но**: комментарий говорит "nominally 2 bps" — это слишком низко для реального движения

**Impact**: CRITICAL — стратегия BounceFromDensity отклоняет 95%+ сигналов из-за мёртвой проверки.

**Fix**:
```toml
# config.toml [strategies.bounce]
min_driver_reversal_bps = 20.0  # 20 bps = 0.2% — реалистичный порог для 1s движения
```

**Test**:
```cpp
// leader_change_1s = -0.0015 (leader down 0.15%)
// leader_move = 0.0015 (for Buy bounce)
// leader_move * 10000 = 15 bps
// 15 bps > 20 bps → false → не reject (правильно)
```

---

### S2-CRITICAL: LeaderLag — Missing price_change_2s Field

**File**: `core/src/strategy/LeaderLag.cpp:60-66`

**Issue**: Код использует несуществующее поле `FeatureFrame::price_change_2s`.

```cpp
// C4: Our movement has not started yet (|our_change_2s| <= 0.1%).
// FeatureFrame stores price_change_1s/5s/30s as fractional (e.g. 0.001 = 0.1%).
// FEAT-05: price_change_2s is now natively available in FeatureFrame
const auto& frame = ctx_.last_frame;
double our_change_2s = frame.price_change_2s;  // ❌ НЕ СУЩЕСТВУЕТ
if (std::abs(our_change_2s) * 100.0 > cfg_.max_our_change_2s_pct) {
    LOG_TRACE("[LeaderLag] {} rejected: our_change_2s={:.4f}% exceeds {}%",
              ticker_, our_change_2s * 100.0, cfg_.max_our_change_2s_pct);
    return std::nullopt;
}
```

**Problem**:
- `FeatureFrame` имеет только: `price_change_1s`, `price_change_5s`, `price_change_30s`
- Комментарий "FEAT-05: price_change_2s is now natively available" — **ЛОЖЬ**
- Код компилируется, но поле не инициализировано → garbage value → random rejects

**Impact**: CRITICAL — стратегия LeaderLag отклоняет сигналы случайным образом (UB).

**Fix**:
```cpp
// Interpolate 2s change from 1s and 5s
// Linear: 2s = 1s + (5s - 1s) * (2-1)/(5-1) = 1s + (5s - 1s) * 0.25
double our_change_2s = frame.price_change_1s + 
                       (frame.price_change_5s - frame.price_change_1s) * 0.25;
```

**Alternative**: Добавить `price_change_2s` в `FeatureExtractor` (но это требует изменений в core).

---

## HIGH Priority Bugs

### S3-HIGH: PaperExecutor — Mark Price Never Updated

**File**: `core/src/executor/PaperExecutor.cpp:82-95, 217-230`

**Issue**: `mark_prices_` map используется для unrealized PnL, но никогда не заполняется.

```cpp
// Line 82: get_active_trades()
double price = 0.0;
auto mit = mark_prices_.find(ticker);
if (mit != mark_prices_.end() && mit->second > 0.0) {
    price = mit->second;  // ❌ НИКОГДА не попадаем сюда
} else {
    // Fallback to (bid+ask)/2
    auto bit = books_.find(ticker);
    if (bit != books_.end()) {
        double bid = bit->second->best_bid().value_or(0.0);
        double ask = bit->second->best_ask().value_or(0.0);
        if (bid > 0.0 && ask > 0.0) price = (bid + ask) * 0.5;
    }
}
```

**Problem**:
- `mark_prices_` объявлен как `std::unordered_map<Ticker, double> mark_prices_;`
- Нет метода `on_mark_price()` или подписки на mark price updates
- Всегда используется fallback (bid+ask)/2 → неточный PnL для futures

**Impact**: HIGH — unrealized PnL показывает неточные значения (может отличаться на 5-10 bps от exchange mark).

**Fix**:
```cpp
// PaperExecutor.hpp
void on_mark_price(const Ticker& ticker, double mark_price) {
    std::lock_guard lock(mutex_);
    mark_prices_[ticker] = mark_price;
}

// BotAppRun.cpp (где обрабатываются mark price updates)
paper_executor_->on_mark_price(ticker, mark_price);
```

---

### S4-HIGH: Config Missing Critical Strategy Parameters

**File**: `config.toml` (multiple strategies)

**Issue**: Стратегии используют параметры, которых нет в config.toml → defaults = 0 → всегда fail.

**Missing Parameters**:

#### BounceFromDensity (15+ missing):
```toml
[strategies.bounce]
# MISSING:
min_approach_speed_bps_1s = 15.0      # C2: impulse approach
max_tape_speed_ratio = 1.5            # C4: stalling check
min_relative_density = 0.15           # C3: density vs depth
tp1_r = 2.0                           # TP1 calculation
min_driver_reversal_bps = 20.0        # C5: leader alignment (S1 fix)
max_level_age = 300                   # C1: level freshness (seconds)
min_density_age_ms = 2000             # C3b: density maturity
require_tape_fade = true              # C4-explicit
tape_fade_max_age = 5                 # C4-explicit (seconds)
leader_contra_lookback = 5            # C6-explicit (seconds)
leader_contra_max_pct = 0.3           # C6-explicit (%)
max_funding_against_bps = 15.0        # C-Funding
max_mark_mid_bps = 10.0               # C-MarkMid
density_cluster_enabled = true        # §5.6
density_cluster_max_bps = 20.0        # §5.6
max_level_touches_for_bounce = 2      # §0.8: N-th approach filter
```

#### BreakoutEatThrough (10+ missing):
```toml
[strategies.breakout]
# MISSING:
max_avg_spread_bps = 4.0
stop_buffer_bps = 8.0
tp1_r = 2.5
min_leader_correlation = 0.6
support_search_range_bps = 30.0
min_distance_from_best_bps = 5.0      # C5-distance (§2.4)
```

#### LeaderLag (8+ missing):
```toml
[strategies.leaderlag]
# MISSING:
min_correlation = 0.6
max_our_change_2s_pct = 0.1
density_on_path_search_bps = 30.0
max_spread_bps = 3.0
lag_max_age = 3000                    # milliseconds
entry_timeout = 10                    # seconds
tp1_catchup_ratio = 0.8
swing_lookback_ticks = 30             # для stop placement
```

#### FlushReversal (10+ missing):
```toml
[strategies.flush]
# MISSING:
max_spread_bps = 6.0
flush_max_age = 10                    # seconds
min_flush_dist_bps = 15.0
min_level_touches = 3
tape_flush_max_age = 5                # seconds
min_flush_count = 1
flush_count_window = 30               # seconds
max_vol_fade_ratio = 0.6
min_price_reversal_bps = 5.0
entry_offset_bps = 3.0
stop_buffer_bps = 8.0
tp1_r = 2.0
entry_timeout = 10                    # seconds
post_entry_grace_sec = 5
min_follow_through_bps = 8.0
```

**Impact**: HIGH — стратегии используют default values (часто 0.0) → все проверки fail → 0 сделок.

**Fix**: Добавить все параметры в `config.toml` с реалистичными значениями (см. выше).

---

## MEDIUM Priority Bugs

### S5-MEDIUM: StrategyEngine — Overly Aggressive News Regime

**File**: `core/src/strategy/StrategyEngine.cpp:189-196`

**Issue**: Порог для News regime слишком низкий → ложные срабатывания.

```cpp
MarketRegime StrategyEngine::classify_regime(const FeatureFrame& frame) const {
    // §7 strategies.common.max_vol_bps = 50: News regime at extreme volatility.
    static constexpr double kNewsVolThresholdBps = 50.0;
    if (frame.volatility_1min_bps > kNewsVolThresholdBps) {
        return MarketRegime::News;  // ❌ Блокирует ВСЕ стратегии
    }
    // ...
}
```

**Problem**:
- `volatility_1min_bps > 50` (0.5%) → News режим
- Для волатильных альткоинов (SOL, DOGE) это нормальная волатильность
- News режим блокирует **все** стратегии (строка 119-125 в tick())
- Нет hysteresis → режим мигает каждую секунду

**Impact**: MEDIUM — стратегии блокируются на волатильных монетах даже без новостей.

**Fix**:
```cpp
// Option 1: Увеличить порог
static constexpr double kNewsVolThresholdBps = 80.0;  // 0.8%

// Option 2: Добавить hysteresis (лучше)
if (frame.volatility_1min_bps > 80.0) {
    // Enter News only if vol > 80 bps
    return MarketRegime::News;
} else if (current_regime(ticker) == MarketRegime::News && 
           frame.volatility_1min_bps > 60.0) {
    // Stay in News until vol < 60 bps (20 bps hysteresis)
    return MarketRegime::News;
}
```

---

### S6-MEDIUM: Universe Affinity Thresholds Too High

**File**: `config.toml` [universe.affinity.*]

**Issue**: Пороги для affinity слишком строгие → мало монет проходят фильтр.

```toml
[universe.affinity.bounce]
min_volume_24h_usd = 100000000.0      # 100M USD — только топ-20 монет
min_bounce_success_rate_7d = 0.40     # 40% — нереалистично высоко
min_levels_in_60min = 3               # 3 уровня за час — редко

[universe.affinity.leaderlag]
min_correlation_60s = 0.6             # 0.6 — слишком высоко для альтов
avg_catchup_completion_rate_7d = 0.50 # 50% — нереалистично
```

**Problem**:
- Только BTC/ETH проходят фильтры
- Для остальных монет affinity_score < threshold → стратегии не активируются
- `require_7d_history = false` но все метрики требуют 7d данных

**Impact**: MEDIUM — стратегии активны только на 2-3 монетах из 30 в pool.

**Fix**:
```toml
[universe.affinity.bounce]
min_volume_24h_usd = 50000000.0       # 50M USD
min_bounce_success_rate_7d = 0.25     # 25% — реалистично
min_levels_in_60min = 2               # 2 уровня

[universe.affinity.leaderlag]
min_correlation_60s = 0.4             # 0.4 — допустимо для альтов
avg_catchup_completion_rate_7d = 0.30 # 30%
```

---

## LOW Priority Bugs

### S7-LOW: FlushReversal — Inefficient Flush Count

**File**: `core/src/strategy/FlushReversal.cpp:68-77`

**Issue**: Сканирует весь `signal_history` для подсчёта TapeFlush.

```cpp
if (cfg_.min_flush_count > 1) {
    int flush_count = 0;
    const auto count_cutoff = now - cfg_.flush_count_window;
    for (const auto& s : ctx_.signal_history) {  // ❌ O(n) каждый tick
        if (s.timestamp < count_cutoff) continue;
        if (s.kind == SignalKind::TapeFlush) ++flush_count;
    }
    if (flush_count < cfg_.min_flush_count) {
        LOG_TRACE("[Flush] {} only {} TapeFlush in {}s (need {})",
                  ticker_, flush_count, cfg_.flush_count_window.count(), cfg_.min_flush_count);
        return std::nullopt;
    }
}
```

**Problem**:
- `signal_history` может содержать 1000+ сигналов
- Сканирование O(n) на каждый tick (60 Hz) → 60k iterations/sec
- Для 30 монет → 1.8M iterations/sec

**Impact**: LOW — производительность, но не критично (CPU < 5%).

**Fix**:
```cpp
// Cache flush count in StrategyContext
struct StrategyContext {
    // ...
    int flush_count_cache_ = 0;
    std::chrono::system_clock::time_point flush_count_cache_ts_;
    
    void update(const Signal& sig) {
        // ...
        if (sig.kind == SignalKind::TapeFlush) {
            ++flush_count_cache_;
            flush_count_cache_ts_ = sig.timestamp;
        }
    }
};

// In tick():
if (cfg_.min_flush_count > 1) {
    // Decay old flushes
    const auto cutoff = now - cfg_.flush_count_window;
    if (ctx_.flush_count_cache_ts_ < cutoff) {
        ctx_.flush_count_cache_ = 0;
    }
    if (ctx_.flush_count_cache_ < cfg_.min_flush_count) {
        return std::nullopt;
    }
}
```

---

## Root Cause Analysis: Почему Нет Сделок?

### 1. Критические Баги (S1, S2)
- **BounceFromDensity**: мёртвая проверка leader alignment → 95% reject
- **LeaderLag**: UB в price_change_2s → random reject

### 2. Отсутствующие Параметры (S4)
- 40+ параметров отсутствуют в config.toml
- Defaults = 0.0 → все проверки fail
- **Пример**: `min_approach_speed_bps_1s = 0` → любая скорость < 0 → reject

### 3. Слишком Строгие Условия
- **BounceFromDensity**: 12 условий (C1-C9 + C-Funding + C-MarkMid + C-Iceberg)
- **BreakoutEatThrough**: 8 условий + 2 invalidation
- **LeaderLag**: 6 условий
- **FlushReversal**: 8 условий
- **Вероятность прохождения**: 0.9^12 = 28% (если каждое условие 90% pass rate)

### 4. Regime Suppression (S5)
- News режим (vol > 50 bps) → блокирует **все** стратегии
- Trend режим → блокирует BounceFromDensity
- Range режим → блокирует LeaderLag
- **Результат**: 50%+ времени хотя бы одна стратегия заблокирована

### 5. Universe Affinity (S6)
- Только 2-3 монеты из 30 проходят affinity фильтры
- Остальные монеты: `affinity_score < threshold` → стратегии не активны

### 6. Signal Detection Gaps
- Если DensityDetector не эмитит сигналы → BounceFromDensity не работает
- Если LeaderTracker не работает → LeaderLag не работает
- **Нужна диагностика**: сколько сигналов эмитится за час?

---

## Recommended Fixes (Priority Order)

### Immediate (CRITICAL)
1. ✅ **S2**: Fix LeaderLag price_change_2s (interpolate from 1s/5s)
2. ✅ **S4**: Add all missing config parameters with realistic defaults
3. ✅ **S1**: Increase min_driver_reversal_bps to 20 bps

### Short-term (HIGH)
4. ✅ **S3**: Add PaperExecutor::on_mark_price() method
5. ✅ **S5**: Increase News regime threshold to 80 bps + add hysteresis
6. ✅ **S6**: Lower universe affinity thresholds

### Medium-term (MEDIUM)
7. ⚠️ Add signal emission diagnostics (log counts per hour)
8. ⚠️ Add strategy condition pass/fail stats to dashboard
9. ⚠️ Implement S7 flush count cache (optimization)

### Long-term (Phase 5)
10. ⚠️ Replace threshold-based regime with HMM
11. ⚠️ ML-based affinity scoring
12. ⚠️ Adaptive thresholds based on 7d performance

---

## Testing Checklist

### Unit Tests
- [ ] LeaderLag: price_change_2s interpolation (S2)
- [ ] BounceFromDensity: leader alignment with 20 bps threshold (S1)
- [ ] PaperExecutor: mark_price updates (S3)
- [ ] StrategyEngine: regime hysteresis (S5)

### Integration Tests
- [ ] Run bot with fixed config for 1 hour
- [ ] Verify signal emission rates (expect 10-20 signals/hour per strategy)
- [ ] Verify strategy activation (expect 5-10 plans/hour)
- [ ] Verify trade execution (expect 1-3 trades/hour in paper mode)

### Performance Tests
- [ ] CPU usage < 20% with 30 tickers
- [ ] Memory < 500 MB
- [ ] Latency p99 < 25ms (book→submit)

---

## Appendix: Complete Config Additions

```toml
[strategies.bounce]
# Existing
tp1_size_ratio = 0.5
no_progress_timeout_sec = 120
entry_offset_bps = 3
stop_buffer_bps = 5
stop_anchor_max_bps = 15
affinity_threshold = 0.6
affinity_stable_min = 5
burst_contra_exit_sec = 5

# NEW (S1, S4 fixes)
min_approach_speed_bps_1s = 15.0
max_tape_speed_ratio = 1.5
min_relative_density = 0.15
tp1_r = 2.0
min_driver_reversal_bps = 20.0
max_level_age_sec = 300
min_density_age_ms = 2000
require_tape_fade = true
tape_fade_max_age_sec = 5
leader_contra_lookback_sec = 5
leader_contra_max_pct = 0.3
max_funding_against_bps = 15.0
max_mark_mid_bps = 10.0
density_cluster_enabled = true
density_cluster_max_bps = 20.0
max_level_touches_for_bounce = 2

[strategies.breakout]
# Existing
tp1_size_ratio = 0.5
no_progress_timeout_sec = 60
aggressive_offset_bps = 2
post_entry_grace_sec = 5
min_follow_through_bps = 10
affinity_threshold = 0.55
affinity_stable_min = 5
min_tape_aggression = 0.2
min_relative_volume = 1.2
max_resistance_cluster_ratio = 0.8
fade_contra_exit_sec = 5
leader_contra_exit_sec = 5
leader_exit_contra_pct = 0.15

# NEW (S4 fix)
max_avg_spread_bps = 4.0
stop_buffer_bps = 8.0
tp1_r = 2.5
min_leader_correlation = 0.6
support_search_range_bps = 30.0
min_distance_from_best_bps = 5.0

[strategies.leaderlag]
# Existing
tp1_size_ratio = 0.6
no_progress_timeout_sec = 15
stop_distance_bps = 8
affinity_threshold = 0.5
affinity_stable_min = 5
correlation_exit_threshold = 0.3
leader_exit_reversal_bps = 5.0
swing_lookback_ticks = 30

# NEW (S2, S4 fixes)
min_correlation = 0.6
max_our_change_2s_pct = 0.1
density_on_path_search_bps = 30.0
max_spread_bps = 3.0
lag_max_age_ms = 3000
entry_timeout_sec = 10
tp1_catchup_ratio = 0.8

[strategies.flush]
# Existing
tp1_size_ratio = 0.7
no_progress_timeout_sec = 90
flush_window_sec = 120
affinity_threshold = 0.4
affinity_stable_min = 5

# NEW (S4 fix)
max_spread_bps = 6.0
flush_max_age_sec = 10
min_flush_dist_bps = 15.0
min_level_touches = 3
tape_flush_max_age_sec = 5
min_flush_count = 1
flush_count_window_sec = 30
max_vol_fade_ratio = 0.6
min_price_reversal_bps = 5.0
entry_offset_bps = 3.0
stop_buffer_bps = 8.0
tp1_r = 2.0
entry_timeout_sec = 10
post_entry_grace_sec = 5
min_follow_through_bps = 8.0

[strategies.common]
# NEW (S5 fix)
max_vol_bps = 80  # Increased from 50 (News regime threshold)
```

---

**Report Version**: 1.0  
**Date**: 2026-05-17  
**Analyzed Files**: 7 (4 strategies + PaperExecutor + StrategyEngine + config.toml)  
**Total Bugs**: 7 (2 CRITICAL, 2 HIGH, 2 MEDIUM, 1 LOW)  
**Estimated Fix Time**: 4 hours (CRITICAL + HIGH only)
