# Adaptive Thresholds System — Proposal

## Проблема

**Текущая ситуация:**
- Пороги в стратегиях жёстко заданы в config.toml (например, `min_approach_speed_bps_1s = 15.0`)
- Один порог для всех монет: BTC ($30B volume) и TON ($100M volume) используют одинаковые пороги
- Результат: либо слишком строго для мелких монет, либо слишком слабо для крупных

**Из STRATEGIES.md:**
- § 0.7: Affinity score уже адаптивный (soft criteria с весами)
- § [universe.dynamic_thresholds]: Динамическое масштабирование для signal detectors
- **НО**: Strategy conditions используют фиксированные пороги

## Решение: Adaptive Strategy Thresholds

### Архитектура

```
TickerUniverse
  ├─ Pool filters (volume, spread, volatility)
  ├─ Affinity scoring (per-strategy, soft criteria)
  └─ Threshold scaling (NEW)
       ├─ Signal detector thresholds (EXISTING)
       └─ Strategy condition thresholds (NEW)
```

### Принцип масштабирования

**Formula** (из config.toml [universe.dynamic_thresholds]):
```
scale_factor = sqrt(ticker_volume_24h / reference_volume_24h)
scale_factor = clamp(scale_factor, min_scale_factor, max_scale_factor)
calibrated_threshold = config_threshold * scale_factor
calibrated_threshold = max(calibrated_threshold, config_threshold * floor_ratio)
```

**Пример:**
```toml
reference_volume_24h_usd = 500000000   # 500M USD
min_scale_factor = 0.15                # 15%
max_scale_factor = 5.0                 # 500%
floor_ratio = 0.10                     # 10% absolute floor
```

**Для BTC ($30B volume):**
- `scale = sqrt(30000 / 500) = sqrt(60) ≈ 7.75`
- `clamped = min(7.75, 5.0) = 5.0`
- `min_approach_speed_bps_1s = 15.0 * 5.0 = 75 bps` (более строго)

**Для TON ($100M volume):**
- `scale = sqrt(100 / 500) = sqrt(0.2) ≈ 0.447`
- `clamped = max(0.447, 0.15) = 0.447`
- `min_approach_speed_bps_1s = 15.0 * 0.447 ≈ 6.7 bps` (более мягко)
- `floor = max(6.7, 15.0 * 0.10) = max(6.7, 1.5) = 6.7 bps` ✓

### Какие параметры масштабировать

#### BounceFromDensity
```toml
[strategies.bounce.adaptive]
# Масштабируемые (зависят от ликвидности)
min_approach_speed_bps_1s = true       # Скорость подхода
min_relative_density = true            # Относительный размер плотности
min_driver_reversal_bps = true         # Движение лидера
min_density_age_ms = true              # Возраст плотности

# Фиксированные (структурные)
max_tape_speed_ratio = false           # Соотношение скоростей (безразмерное)
tp1_r = false                          # R:R ratio (структурный)
entry_offset_bps = false               # Offset от уровня (риск-параметр)
stop_buffer_bps = false                # Buffer стопа (риск-параметр)
```

#### BreakoutEatThrough
```toml
[strategies.breakout.adaptive]
min_tape_aggression = true             # Агрессия ленты
min_relative_volume = false            # Ratio (безразмерное)
min_distance_from_best_bps = true      # Дистанция от best
support_search_range_bps = true        # Диапазон поиска поддержки
```

#### LeaderLag
```toml
[strategies.leaderlag.adaptive]
max_our_change_2s_pct = true           # Наше движение
density_on_path_search_bps = true      # Диапазон поиска плотности
stop_distance_bps = true               # Дистанция стопа
```

#### FlushReversal
```toml
[strategies.flush.adaptive]
min_flush_dist_bps = true              # Дистанция прострела
min_price_reversal_bps = true          # Разворот цены
entry_offset_bps = true                # Offset входа
stop_buffer_bps = true                 # Buffer стопа
```

### Реализация

#### 1. Расширить TickerUniverse

```cpp
// TickerUniverse.hpp
struct TickerMetrics {
    double volume_24h_usd;
    double avg_spread_bps;
    double volatility_1min_bps;
    // NEW:
    double threshold_scale_factor;  // Calculated once per refresh
};

class TickerUniverse {
    // ...
    double calculate_threshold_scale(double volume_24h_usd) const;
    double scale_threshold(const Ticker& ticker, double base_threshold) const;
};
```

#### 2. Добавить метод масштабирования

```cpp
// TickerUniverse.cpp
double TickerUniverse::calculate_threshold_scale(double volume_24h_usd) const {
    const double ref_vol = cfg_.dynamic_thresholds.reference_volume_24h_usd;
    if (ref_vol <= 0.0 || volume_24h_usd <= 0.0) return 1.0;
    
    double scale = std::sqrt(volume_24h_usd / ref_vol);
    scale = std::clamp(scale, 
                       cfg_.dynamic_thresholds.min_scale_factor,
                       cfg_.dynamic_thresholds.max_scale_factor);
    return scale;
}

double TickerUniverse::scale_threshold(const Ticker& ticker, double base_threshold) const {
    auto it = metrics_.find(ticker);
    if (it == metrics_.end()) return base_threshold;
    
    double scaled = base_threshold * it->second.threshold_scale_factor;
    double floor = base_threshold * cfg_.dynamic_thresholds.floor_ratio;
    return std::max(scaled, floor);
}
```

#### 3. Использовать в стратегиях

```cpp
// BounceFromDensity.cpp
std::optional<TradePlan> BounceFromDensity::tick(...) {
    // ...
    
    // OLD: fixed threshold
    // if (approach_speed < cfg_.min_approach_speed_bps_1s) return std::nullopt;
    
    // NEW: adaptive threshold
    const double min_speed = universe_->scale_threshold(
        ticker_, cfg_.min_approach_speed_bps_1s);
    if (approach_speed < min_speed) {
        LOG_TRACE("[Bounce] {} approach speed {:.1f} < {:.1f} (scaled from {:.1f})",
                  ticker_, approach_speed, min_speed, cfg_.min_approach_speed_bps_1s);
        return std::nullopt;
    }
    
    // ...
}
```

### Config Changes

```toml
[universe.dynamic_thresholds]
enabled = true
reference_volume_24h_usd = 500000000   # 500M USD
min_scale_factor = 0.15                # 15%
max_scale_factor = 5.0                 # 500%
floor_ratio = 0.10                     # 10% absolute floor

# NEW: Per-strategy adaptive flags
[strategies.bounce.adaptive]
min_approach_speed_bps_1s = true
min_relative_density = true
min_driver_reversal_bps = true
min_density_age_ms = true

[strategies.breakout.adaptive]
min_tape_aggression = true
min_distance_from_best_bps = true
support_search_range_bps = true

[strategies.leaderlag.adaptive]
max_our_change_2s_pct = true
density_on_path_search_bps = true
stop_distance_bps = true

[strategies.flush.adaptive]
min_flush_dist_bps = true
min_price_reversal_bps = true
```

### Преимущества

1. **Автоматическая адаптация** — один config для всех монет
2. **Справедливость** — BTC и TON оцениваются по своим меркам
3. **Больше сигналов** — мелкие монеты не отсекаются слишком строгими порогами
4. **Меньше шума** — крупные монеты не генерируют ложные сигналы
5. **Совместимость** — можно отключить (`enabled = false`) для тестирования

### Недостатки и Риски

1. **Сложность отладки** — пороги меняются для каждой монеты
2. **Риск переоптимизации** — sqrt может быть не оптимальной функцией
3. **Нужна калибровка** — reference_volume, min/max scale требуют подбора
4. **Больше кода** — каждая стратегия должна вызывать `scale_threshold()`

### Альтернативы

#### A1: Tier-based Thresholds (проще)
```toml
[strategies.bounce.tiers]
# Volume tiers
tier_1_volume_usd = 1000000000  # >$1B (BTC, ETH)
tier_2_volume_usd = 100000000   # $100M-$1B
tier_3_volume_usd = 0           # <$100M

# Thresholds per tier
min_approach_speed_bps_1s = [75.0, 15.0, 8.0]  # tier1, tier2, tier3
```

**Pros:** Проще реализовать, легче отлаживать  
**Cons:** Резкие скачки на границах тиров, меньше гибкости

#### A2: ML-based Adaptive (Phase 5)
```python
# Обучить модель на исторических данных
threshold = model.predict(ticker_features)
```

**Pros:** Оптимальные пороги для каждой монеты  
**Cons:** Требует ML инфраструктуры, Phase 5

#### A3: Percentile-based (статистический)
```cpp
// Для каждой монеты: собрать распределение approach_speed за 7 дней
// Порог = P25 (25-й перцентиль) → отсекаем нижние 25% подходов
double min_speed = ticker_stats.approach_speed_p25;
```

**Pros:** Автоматически адаптируется к поведению монеты  
**Cons:** Требует 7d warmup, может быть нестабильным

### Рекомендация

**Phase 0-4:** Использовать **Tier-based** (A1) — проще, быстрее, достаточно эффективно.

**Phase 5:** Перейти на **sqrt-based adaptive** (основное предложение) после накопления статистики.

**Phase 6+:** Рассмотреть **ML-based** (A2) или **Percentile-based** (A3) для максимальной точности.

---

## Immediate Action Plan

### 1. Добавить Tier-based Thresholds (Quick Win)

```toml
# config.toml
[strategies.bounce.tiers]
enabled = true
volume_tiers_usd = [1000000000, 100000000, 0]  # >$1B, $100M-$1B, <$100M

# Thresholds per tier (tier1, tier2, tier3)
min_approach_speed_bps_1s = [50.0, 15.0, 8.0]
min_relative_density = [0.25, 0.15, 0.10]
min_driver_reversal_bps = [30.0, 20.0, 12.0]

[strategies.breakout.tiers]
enabled = true
volume_tiers_usd = [1000000000, 100000000, 0]
min_tape_aggression = [0.35, 0.20, 0.12]
min_distance_from_best_bps = [8.0, 5.0, 3.0]
```

### 2. Реализация (2 часа)

```cpp
// TickerUniverse.hpp
struct TierConfig {
    std::vector<double> volume_tiers_usd;
    std::map<std::string, std::vector<double>> thresholds;
};

class TickerUniverse {
    int get_tier(const Ticker& ticker) const;
    double get_tiered_threshold(const Ticker& ticker, const std::string& param_name) const;
};

// BounceFromDensity.cpp
const double min_speed = universe_->get_tiered_threshold(ticker_, "min_approach_speed_bps_1s");
```

### 3. Тестирование

- [ ] Unit test: tier assignment (BTC→tier1, SOL→tier2, TON→tier3)
- [ ] Integration test: 3 монеты разных тиров, проверить разные пороги
- [ ] Backtest: сравнить fixed vs tiered (ожидаем +30% сигналов на tier3)

---

## Appendix: Comparison with STRATEGIES.md

### Что уже есть в STRATEGIES.md

✅ **§ 0.7 Affinity score** — адаптивная система для выбора монет  
✅ **[universe.dynamic_thresholds]** — масштабирование для signal detectors  
✅ **Soft criteria с весами** — гибкая оценка кандидатов  

### Что отсутствует

❌ **Adaptive strategy thresholds** — пороги внутри стратегий фиксированные  
❌ **Tier-based system** — нет упоминания тиров по объёму  
❌ **Per-ticker calibration** — нет механизма подстройки под конкретную монету  

### Предложение: Дополнить STRATEGIES.md

Добавить новый раздел:

```markdown
## 0.11. Adaptive Thresholds — Масштабирование порогов

Пороги в стратегиях (например, `min_approach_speed_bps_1s`) масштабируются
в зависимости от ликвидности тикера. Используется tier-based система:

| Tier | Volume 24h | Scale Factor | Example (BTC/TON) |
|------|-----------|--------------|-------------------|
| 1    | >$1B      | 3.0×         | BTC: 15→45 bps    |
| 2    | $100M-$1B | 1.0×         | SOL: 15→15 bps    |
| 3    | <$100M    | 0.5×         | TON: 15→7.5 bps   |

Параметры, подлежащие масштабированию, помечены в config как `adaptive = true`.
Структурные параметры (R:R ratios, безразмерные соотношения) остаются фиксированными.
```

---

**Version**: 1.0  
**Date**: 2026-05-17  
**Status**: Proposal (requires implementation)  
**Estimated Effort**: 4 hours (tier-based), 12 hours (sqrt-based)
