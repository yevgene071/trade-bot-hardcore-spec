# Анализ торгового бота: граф потока, найденные проблемы, план доработки

**Дата:** 2026-05-15  
**Изучены файлы:** STRATEGIES.md, SIGNAL_DETECTION.md, ARCHITECTURE.md, METASCALP_API_CONTRACT.md,
MetaScalp-Api.md, BounceFromDensity.cpp, BreakoutEatThrough.cpp, LeaderLag.cpp,
DensityDetector.cpp, StrategyEngine.cpp, RiskManager.cpp, TickerUniverse.cpp,
LiveExecutor.cpp, LeaderSignal.cpp, BotAppInit.cpp

---

## 1. Граф потока данных (Data Flow)

```
╔══════════════════════════════════════════════════════════════════════╗
║                  MetaScalp (localhost:17845-17855)                  ║
║  REST: /tickers /orders /positions /balance /cluster-snapshot        ║
║  WS:  orderbook_subscribe  trade_subscribe  subscribe (account)      ║
║       notification_subscribe  signal_level_subscribe                 ║
╚══════════╦═══════════════════════════════════╦════════════════════════╝
           │ WS                                │ REST
           ▼                                   ▼
┌──────────────────────┐         ┌──────────────────────────┐
│   MarketDataFeed     │         │      OrderGateway         │
│  (BeastWsClient)     │         │   (CurlHttpClient)        │
└──────┬───────────────┘         └────────────┬─────────────┘
       │ orderbook_snapshot/update             │
       │ trade_update                          │
       │ position_update / order_update        │
       │ balance_update / finres_update        │
       ▼                                       │
┌──────────────────────────────────────────┐   │
│           MarketData Layer                │   │
│  ┌─────────────┐  ┌─────────────────┐    │   │
│  │  OrderBook  │  │  TradeStream    │    │   │
│  │(absl btree) │  │(Hawkes, Welford)│    │   │
│  └─────────────┘  └─────────────────┘    │   │
│  ┌─────────────┐  ┌─────────────────┐    │   │
│  │LeaderTracker│  │ClusterSnapshot  │    │   │
│  │(Kalman,Corr)│  │(REST poll 5min) │    │   │
│  └─────────────┘  └─────────────────┘    │   │
└──────────────────────┬───────────────────┘   │
                       │ 10-20 Hz              │
                       ▼                       │
              ┌─────────────────┐              │
              │FeatureExtractor │              │
              │  → FeatureFrame │              │
              └────────┬────────┘              │
                       │                       │
                       ▼                       │
         ┌─────────────────────────────┐       │
         │       Signal Detectors      │       │
         │  ┌─────────────────────┐    │       │
         │  │  DensityDetector    │    │       │  ← [BUG-1] DensityRemoved
         │  │  (DEMA avg, sticky) │    │       │     не эмитируется для
         │  │  └ DensityEating    │    │       │     подтверждённых плотностей
         │  └─────────────────────┘    │       │
         │  ┌─────────────────────┐    │       │  ← [BUG-2] EatingWindow
         │  │  IcebergDetector    │    │       │     накапливает весь volume,
         │  │  (Bayesian update)  │    │       │     не скользящее окно
         │  └─────────────────────┘    │       │
         │  ┌─────────────────────┐    │       │
         │  │  TapeAnalyzer       │    │       │
         │  │  (Hawkes/CUSUM)     │    │       │
         │  └─────────────────────┘    │       │
         │  ┌─────────────────────┐    │       │
         │  │  LevelDetector      │    │       │
         │  │ [PARTIAL: нет DBSCAN│    │       │
         │  │  нет KDE интеграции]│    │       │
         │  └─────────────────────┘    │       │
         │  ┌─────────────────────┐    │       │
         │  │  ApproachAnalyzer   │    │       │
         │  │ [PARTIAL: нет HMM,  │    │       │
         │  │  if-else пороги]    │    │       │
         │  └─────────────────────┘    │       │
         │  ┌─────────────────────┐    │       │
         │  │  LeaderSignal       │    │       │  ← [BUG-3] history без eviction
         │  │  (Welford Pearson)  │    │       │
         │  └─────────────────────┘    │       │
         └────────────┬────────────────┘       │
                      │ SignalBus               │
                      ▼                        │
         ┌────────────────────────────┐        │
         │      StrategyEngine        │        │
         │  ┌───────────────────┐     │        │
         │  │ BounceFromDensity │     │        │  ← [GAP-1] нет stop_anchor_max_bps
         │  └───────────────────┘     │        │  ← [GAP-2] dist_to_level не проверяется
         │  ┌───────────────────┐     │        │
         │  │BreakoutEatThrough │     │        │
         │  └───────────────────┘     │        │
         │  ┌───────────────────┐     │        │
         │  │    LeaderLag      │     │        │
         │  └───────────────────┘     │        │
         │  ┌───────────────────┐     │        │
         │  │ FlushReversal     │     │        │  ← [MISSING] Phase 5
         │  │ [NOT IMPLEMENTED] │     │        │
         │  └───────────────────┘     │        │
         │  classify_regime() ──────► │        │  ← [GAP-3] вычисляется, но
         │  [threshold fallback,       │        │     стратегии его НЕ читают!
         │   HMM не реализован]        │        │
         └────────────┬───────────────┘        │
                      │ TradePlan              │
                      ▼                        │
         ┌────────────────────────────┐        │
         │       RiskManager          │        │
         │  R1  Kill-switch           │        │
         │  R2  Daily loss limit      │        │
         │  R3  Max concurrent pos    │        │
         │  R4  Duplicate/per-ticker  │        │
         │  R5  Universe affinity     │        │  ← [GAP-4] affinity только
         │  R6  Stop validation       │        │     volume+spread, не полный score
         │  R7  TP / R:R check        │        │
         │  R8  Position sizing       │        │
         │  R9  Margin check          │        │
         │  R10 Trade rate limit      │        │
         │  R11 Loss streak CB        │        │
         │  R12 News blackout         │        │
         │  R13 Funding blackout      │        │
         │  R14 Single position loss  │        │
         │  R15 Entry slippage        │        │
         └────────────┬───────────────┘        │
                      │ RiskDecision           │
                      ▼                        ▼
         ┌────────────────────────────┐
         │     LiveExecutor           │
         │  PendingEntry → Open       │
         │  → TP1 (BE-stop) → Closed  │
         │  no_progress_timeout       │
         │  follow_through check      │
         │  R14 force-close           │
         └────────────────────────────┘
                      │
                      ▼
         ┌────────────────────────────┐
         │  OrderGateway (REST)       │
         │  POST /orders              │
         │  POST /orders/cancel       │
         └────────────────────────────┘
```

---

## 2. КРИТИЧЕСКИЕ БАГИ (нарушают логику немедленно)

### [BUG-1] DensityDetector: DensityRemoved не эмитируется для подтверждённых плотностей

**Файл:** `core/src/signals/DensityDetector.cpp:131-149`

**Проблема:**  
Когда уровень удаляется из книги (`level.size == 0`), код проверяет `age < fake_threshold` и эмитирует `DensityRemoved` ТОЛЬКО для "фальшивых" плотностей (появились и пропали быстро). Для **подтверждённых** плотностей (age ≥ 2000 мс, `emitted=true`) — никакого события не генерируется, запись просто удаляется из `tracked_`.

```cpp
// Текущий код — НЕПРАВИЛЬНО:
if (age < cfg_.fake_threshold) {
    bus_.publish(DensityRemoved{fake=true});
}
tracked_.erase(it);   // подтверждённая плотность исчезает МОЛЧА
```

**Последствия:**
- `BounceFromDensity::check_close_conditions()` (строки 267–282) проверяет наличие `DensityRemoved` для закрытия позиции после входа — **сигнал никогда не придёт**
- Инвалидация до входа (строки 89–99 в BounceFromDensity) проверяет `DensityRemoved` в истории — **работает некорректно**
- Плотность, которую «съели» и она исчезла, не инвалидирует план Bounce

**Исправление:**
```cpp
} else {
    auto it = tracked_.find(tick);
    if (it != tracked_.end()) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.first_seen);
        Signal s {
            .kind = SignalKind::DensityRemoved,
            .timestamp = now,
            .ticker = ticker_,
            .price = level.price,
            .confidence = 1.0,
            .payload = {.age_ms = static_cast<int>(age.count()),
                        .fake = age < cfg_.fake_threshold}   // fake=true или false
        };
        bus_.publish(s);   // ВСЕГДА эмитировать
        tracked_.erase(it);
    }
}
```

---

### [BUG-2] DensityEating: скользящее окно не реализовано

**Файл:** `core/src/signals/DensityDetector.cpp:59-84` (метод `on_trade`)

**Проблема:**  
`meta.eaten_volume` накапливается кумулятивно за всё время жизни плотности, а не за последние `eating_window_ms` (3000 мс). Параметр `eating_window_ms` определён в конфиге, но нигде не используется для очистки.

**Последствия:**  
Медленно "поедаемая" плотность (10% за 30 секунд) через 5 минут накопит eaten_ratio = 1.0 и выдаст `DensityEating`. Это ложный сигнал: в реальности она не пробивается агрессивно сейчас. BreakoutEatThrough получит неверный сигнал.

**Исправление:**  
Хранить `meta.hit_timestamps` (ring-buffer последних N принтов), при проверке суммировать только объём принтов за последние `eating_window_ms`:

```cpp
struct LevelMeta {
    // ... existing fields ...
    struct Hit { std::chrono::system_clock::time_point ts; double volume; };
    std::deque<Hit> hits;  // вместо eaten_volume + print_count
};

// В on_trade:
meta.hits.push_back({now, trade.size});
// Убираем старые:
while (!meta.hits.empty() &&
       (now - meta.hits.front().ts) > cfg_.eating_window) {
    meta.hits.pop_front();
}
double eaten_in_window = 0;
int prints_in_window = 0;
for (auto& h : meta.hits) {
    eaten_in_window += h.volume;
    ++prints_in_window;
}
double eaten_ratio = eaten_in_window / meta.initial_size;
```

---

### [BUG-3] LeaderSignal: история без ограничения размера

**Файл:** `core/src/signals/LeaderSignal.cpp:22-23`

**Проблема:**  
`our_history_` и `leader_history_` — это `std::vector<pair<ts, price>>`, в который постоянно пушатся новые записи (`push_back`) без eviction старых. При работе 8+ часов и 10 Гц это 288 000 записей в каждом векторе. Метод `get_move` итерирует с конца — O(n) на каждый кадр.

**Исправление:** Использовать кольцевой буфер (уже есть `CircularBuffer.hpp` в проекте) или удалять записи старше 60 секунд при каждом push.

---

## 3. ЛОГИЧЕСКИЕ НЕСООТВЕТСТВИЯ СО СПЕЦИФИКАЦИЕЙ

### [GAP-1] Режим рынка вычисляется но не используется

**Файл:** `core/src/strategy/StrategyEngine.cpp:78, 201-219`

Метод `classify_regime()` вызывается на каждом фрейме и результат сохраняется в `regimes_[frame.ticker]`. Однако **ни одна стратегия не читает текущий режим** перед принятием решения. Матрица из STRATEGIES.md §6 (Trend/Range/News × стратегия) не работает.

**Пример:** LeaderLag не должен работать в News-режиме (корреляция ломается), но сейчас он генерирует планы при любом режиме.

**Что нужно:** Добавить проверку `strategy_engine_.current_regime(ticker)` в `tick()` каждой стратегии:
```cpp
// В LeaderLag::tick():
auto regime = engine_.current_regime(ticker_);
if (regime == MarketRegime::News) return std::nullopt;
```

**Также:** Сам `classify_regime()` — это пороговый if-else (fallback), а не HMM. Порог `trend_slope = price_change_30s > 0.3%` — статический. HMM, упомянутый в STRATEGIES.md §6, не реализован.

---

### [GAP-2] Affinity score — только 2 критерия вместо полной формулы

**Файл:** `core/src/app/BotAppInit.cpp:93-120`

Affinity-лямбды в `register_strategy_affinities_` проверяют только `volume_24h_usd` и `avg_spread_bps`. Полная взвешенная формула из STRATEGIES.md §0.7 (soft/hard критерии с warmup-penalty) **не реализована**.

Например, для bounce аффинити:
- ✅ `min_volume_24h_usd` (hard) — реализовано
- ✅ `max_avg_spread_bps` (hard) — реализовано
- ❌ `min_density_events_per_hour` (soft, weight 0.50) — НЕ реализовано
- ❌ `min_levels_in_60min` (soft, weight 0.30) — НЕ реализовано
- ❌ `min_bounce_success_rate_7d` (soft, weight 0.20) — НЕ реализовано
- ❌ `require_low_news_density` (hard) — НЕ реализовано

В итоге стратегии активируются на любом тикере с нужным объёмом/спредом, даже если плотности там почти не возникают.

---

### [GAP-3] BounceFromDensity: отсутствует `stop_anchor_max_bps` проверка

**Файл:** `core/src/strategy/BounceFromDensity.cpp` (раздел расчёта цен)

STRATEGIES.md §1.8: _"если за плотностью нет другого уровня ближе `stop_anchor_max_bps` (15 bps), стратегия отказывается от сделки"_.

Сейчас стоп вычисляется как `density_price ± stop_buffer_bps` — фиксированный отступ от плотности, без проверки есть ли за плотностью "якорный" уровень. Стоп может оказаться слишком широким и не пройти RiskManager, но без явного логирования причины "нет якоря".

---

### [GAP-4] BounceFromDensity: C1 проверяет возраст сигнала, не расстояние до уровня

**Файл:** `core/src/strategy/BounceFromDensity.cpp:45-47`

Спек (STRATEGIES.md §1.4): условие C1 — `dist_to_level_bps <= 10`.  
Код: `(now - it_approach->second.timestamp) > std::chrono::seconds(5)` — проверяется возраст сигнала, не дистанция.

Это работает косвенно (LevelApproach эмитируется когда dist <= approach_trigger_bps=10), но текущая цена могла уйти от уровня за те 5 секунд пока сигнал "свежий".

---

### [GAP-5] Направление плотности в BounceFromDensity — потенциальная инверсия

**Файл:** `core/src/strategy/BounceFromDensity.cpp:85`

Код: `Side plan_side = (side_str == "Bid") ? Side::Buy : Side::Sell;`  
Спек §1.4: `"Плотность в ask → покупать; Плотность в bid → продавать"`

Это **противоположно** тому, что написано в спеке. Необходима ручная проверка на живых данных какая интерпретация корректна:
- Код: Bid-density = поддержка снизу → BUY (стандартная трактовка стакана)
- Спек: Ask-density → BUY (возможная опечатка или специфичная трактовка мануала)

**Требует уточнения у автора стратегии.**

---

### [GAP-6] BreakoutEatThrough: C3 в спеке требует плотность/айсберг в 20 bps, код — конфиг

**Файл:** `core/src/strategy/BreakoutEatThrough.cpp:156-181`

Спек §2.4 C3: "поддержка сзади в пределах 20 bps". Код использует `cfg_.support_search_range_bps` — параметр из конфига. Нужно убедиться, что дефолт = 20 bps и он прописан в `config.toml`.

---

### [GAP-7] LeaderLag: find_swing_low — простой min, не настоящий swing

**Файл:** `core/src/strategy/LeaderLag.cpp:37-48`

`find_swing_low` возвращает минимум цены за lookback_ticks, а не настоящий локальный экстремум (swing low = ценовая точка с более высокими соседями слева и справа). Для быстрой леаговой стратегии это некритично, но стоп может быть дальше необходимого.

---

## 4. НЕЗАВЕРШЁННЫЕ КОМПОНЕНТЫ

| Компонент | Статус | Приоритет |
|-----------|--------|-----------|
| **FlushReversal стратегия** | ❌ Не реализована (Phase 5) | P3 |
| **LiquidationDetector** | ❌ Не реализован (Phase 5) | P3 |
| **HMM для classify_regime()** | 🟡 Threshold fallback | P2 |
| **HMM для ApproachAnalyzer** | 🟡 If-else fallback | P2 |
| **DBSCAN в LevelDetector** | 🟡 Требует проверки | P2 |
| **KDE для cluster-snapshot уровней** | 🟡 Требует проверки | P2 |
| **t-digest для TapeFlush** | 🟡 Используется hardcoded min_size_usd | P2 |
| **FeatureRecorder (Parquet/CSV)** | 🟡 Не подключён к pipeline | P3 |
| **T2-LABELING инструмент** | ❌ Не реализован | P2 |
| **Replay determinism тест (10×)** | ❌ Не реализован | P2 |
| **Funding via MetaScalp WS** | 🟡 Используется Binance REST | P2 |
| **mark_price_subscribe** | 🟡 Марк-цены в Executor не подписаны через WS | P1 |
| **Affinity полная формула** | 🟡 Только volume+spread | P1 |
| **Режим рынка → фильтр стратегий** | ❌ Вычисляется, не применяется | P1 |
| **stop_anchor_max_bps в Bounce** | ❌ Отсутствует | P1 |

---

## 5. НЕИСПОЛЬЗОВАННЫЕ ВОЗМОЖНОСТИ MetaScalp API

### 5.1. `mark_price_subscribe` WS

Документировано в API. LiveExecutor использует `mark_prices_` для расчёта unrealized PnL, no_progress_timeout и R14. **Но подписки через WS нет** — mark цены устаревают или не обновляются вообще.

**Что добавить:**
```json
{"Type": "mark_price_subscribe", "Data": {"ConnectionId": 1, "Ticker": "BTCUSDT"}}
```
Обработчик `mark_price_update` в MarketDataFeed → `executor.set_mark_prices()`.

### 5.2. `funding_subscribe` WS

API поддерживает реалтайм funding через WebSocket. Сейчас R13 (funding blackout) использует Binance REST через external feed. Это лишняя зависимость и задержка. MetaScalp уже подключён к бирже и может транслировать funding rate.

### 5.3. Signal Levels API (для UI-интеграции)

`POST /api/connections/{id}/signal-levels` — бот может автоматически выставлять сигнальные уровни в MetaScalp UI когда видит плотность (для визуального контроля оператором).

`SignalLevelBridge` уже создан в BotApp — **но не наполнен логикой** (пустая реализация). Когда `DensityDetected` эмитируется, можно размещать signal level на этой цене в UI.

### 5.4. `orderbook_settings` — автокалибровка

`GET /api/connections/{id}/orderbook-settings?Ticker=` возвращает `LargeAmountUsd` — операторский порог для конкретного тикера. `TickerUniverse.density_min_size_usd()` это уже учитывает — **подключено корректно** ✅.

### 5.5. `notification_subscribe` → `BigTick` / `BigOrderBookAmount`

Уже реализовано через NotificationFeed → `universe_.on_big_tick()` / `on_big_amount()` — **подключено** ✅.

---

## 6. ПЛАН ДОРАБОТКИ (приоритеты)

### P0 — Критические баги (немедленно, блокируют корректную работу)

| # | Задача | Файл | Сложность |
|---|--------|------|-----------|
| B1 | Fix DensityRemoved для confirmed плотностей | `DensityDetector.cpp:131-149` | 30 мин |
| B2 | Fix DensityEating скользящее окно (deque вместо кумулятивного) | `DensityDetector.cpp:59-84` | 2 часа |
| B3 | Fix LeaderSignal history eviction (ограничить 60 сек) | `LeaderSignal.cpp:22` | 30 мин |

### P1 — Нарушение стратегической логики

| # | Задача | Сложность |
|---|--------|-----------|
| S1 | Подключить режим рынка к фильтрации стратегий | 2 часа |
| S2 | Добавить stop_anchor_max_bps проверку в BounceFromDensity | 1 час |
| S3 | Прояснить инверсию направления (Bid→Buy vs Ask→Buy) | уточнение |
| S4 | Реализовать mark_price_subscribe в MarketDataFeed | 3 часа |
| S5 | Affinity: добавить хотя бы density_events_per_hour критерий | 3 часа |

### P2 — Алгоритмические улучшения

| # | Задача | Сложность |
|---|--------|-----------|
| A1 | HMM regime classifier (3-state: Trend/Range/News) | 1–2 дня |
| A2 | HMM для ApproachAnalyzer | 1–2 дня |
| A3 | DBSCAN кластеризация в LevelDetector (проверить и доделать) | 1 день |
| A4 | t-digest адаптивный порог в TapeFlush | 4 часа |
| A5 | Реальный swing-low/high через ZigZag в LeaderLag | 2 часа |
| A6 | funding_subscribe через MetaScalp WS | 2 часа |

### P3 — Phase 5 компоненты

| # | Задача | Сложность |
|---|--------|-----------|
| F1 | FlushReversal стратегия | 3–5 дней |
| F2 | LiquidationDetector (Binance forceOrder feed) | 2 дня |
| F3 | SignalLevelBridge — авто-размещение signal levels в UI | 1 день |
| F4 | T2-LABELING инструмент | 2–3 дня |
| F5 | Replay determinism тест | 1 день |

---

## 7. БЫСТРАЯ КАРТА СОСТОЯНИЯ (snapshot)

```
TRANSPORT   ████████████████████ 95%  ← полностью готов
ORDERBOOK   ████████████████████ 95%  ← fixed-point, SIMD, sanity check
TRADESTREAM ████████████████████ 90%  ← Hawkes, CUSUM, Welford OK
LEADERTRACK ███████████████░░░░░ 75%  ← Kalman OK, history leak
DENSITY     ████████████████░░░░ 80%  ← [BUG-1 BUG-2] критические
ICEBERG     ████████████████░░░░ 80%  ← Bayesian OK
TAPE        ███████████████████░ 90%  ← все sub-детекторы OK
LEVEL       █████████████░░░░░░░ 65%  ← DBSCAN/KDE неясны
APPROACH    ████████████░░░░░░░░ 60%  ← HMM отсутствует
LEADERSIG   ████████████████░░░░ 80%  ← [BUG-3] history
STRATEGY_B  ████████████████░░░░ 80%  ← [GAP-1,3,4,5] частичные
STRATEGY_BR ████████████████░░░░ 80%  ← C3 window уточнить
STRATEGY_LL ████████████████░░░░ 80%  ← swing-low упрощён
STRATEGY_FR ░░░░░░░░░░░░░░░░░░░░  0%  ← Phase 5
STRATENGINE ████████████░░░░░░░░ 60%  ← [GAP-3] режим не применяется
TICKERUNI   █████████████░░░░░░░ 65%  ← affinity упрощён
RISKMANAGER ████████████████████ 95%  ← R1-R15 все реализованы
EXECUTOR    ████████████████████ 90%  ← state machine, BE-stop OK
BOTAPP      ████████████████░░░░ 80%  ← wiring корректный
```

---

## 8. ИТОГ

Бот имеет **хорошую архитектурную основу**: транспорт, OrderBook, TradeStream, RiskManager и LiveExecutor реализованы качественно. Три стратегии функционально работают.

**Критические баги (P0)** в DensityDetector делают невозможной корректную инвалидацию после входа в BounceFromDensity и искажают DensityEating сигнал для Breakout — эти три фикса нужно сделать первыми.

**Главный стратегический пробел (P1)**: режим рынка вычисляется но не используется. Бот торгует при любом режиме, включая News (когда все стратегии должны быть заблокированы).

**API MetaScalp**: `mark_price_subscribe` и `funding_subscribe` — быстрые wins, убирают внешние зависимости и улучшают realtime PnL tracking.
