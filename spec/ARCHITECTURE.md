# ARCHITECTURE

Техническая архитектура торгового бота для MetaScalp.

## 0. Технологический стек
- **Язык:** C++23 (ISO/IEC 14882:2024). Использование `std::expected`, `std::print`, `std::mdspan`, улучшенных ranges и корутин.
- **Платформы:** Кроссплатформенная разработка (Linux Pop!_OS / Windows 11).
- **SIMD:** AVX2 baseline (AVX-512 не используется для обеспечения кроссплатформенной стабильности и соответствия целевому железу).
- **Сборка:** CMake 3.28+.


---

## 1. Высокоуровневая схема

```
                ┌─────────────────────────────────────────────────┐
                │              MetaScalp (localhost)              │
                │   HTTP REST :17845-17855    WS :17845-17855     │
                └────────┬─────────────────────────┬──────────────┘
                         │ REST                    │ WebSocket
                         ▼                         ▼
          ┌─────────────────────────────────────────────┐
          │          Transport Layer (§ 2)              │
          │  HttpClient  │  WsClient  │  ReplaySource   │
          └──────┬──────────────┬─────────────────┬─────┘
                 │              │                 │
          ┌──────▼───────┐  ┌───▼───────┐  ┌──────▼──────┐
          │  OrderGateway│  │MarketData │  │  (replay)   │
          │  (REST calls)│  │ (WS feed) │  │ dump reader │
          └──────┬───────┘  └───┬───────┘  └──────┬──────┘
                                │
                       ┌────────▼────────┐
                       │ TickerUniverse  │  → каталог + screener
                       │ (REST tickers + │     filtered list
                       │  notification_  │     на который подписан
                       │  subscribe WS)  │     MarketDataFeed
                       └────────┬────────┘
                 │              │                 │
                 │     ┌────────▼───────┐         │
                 │     │ OrderBook      │◄────────┘
                 │     │ TradeStream    │
                 │     │ LeaderTracker  │
                 │     └────────┬───────┘
                 │              │
                 │     ┌────────▼───────┐
                 │     │ FeatureExtractor│  → FeatureFrame (10-20 Hz)
                 │     └────────┬───────┘
                 │              │
                 │     ┌────────▼────────┐
                 │     │ SignalDetectors │  → Signal events
                 │     │ (density,       │
                 │     │  iceberg, tape, │
                 │     │  level, leader) │
                 │     └────────┬────────┘
                 │              │
                 │     ┌────────▼────────┐
                 │     │ StrategyEngine  │  → TradePlan
                 │     └────────┬────────┘
                 │              │
                 │     ┌────────▼────────┐
                 │     │  RiskManager    │  → approved / rejected
                 │     └────────┬────────┘
                 │              │
                 └──────────────▼
                        Executor
                   (places real orders)
```

---

## 2. Модули

### 2.1. Transport Layer (`src/transport/`)

Тонкая обёртка над HTTP и WebSocket. **Цель: изолировать нестабильный MetaScalp API — при его изменении правим только этот слой.**

- `IHttpClient` — интерфейс (get/post/put/delete, JSON in/out)
- `CurlHttpClient` — реализация на libcurl (или `cpr`)
- `IWsClient` — интерфейс (connect, send, on_message, on_close)
- `BeastWsClient` — реализация на Boost.Beast
- `MetaScalpDiscovery` — сканирование портов 17845–17855, `/ping`, проверка `App == "MetaScalp"`

**Правило:** никакие типы из transport-слоя не утекают выше — всё конвертируется в доменные типы из `src/domain/`.

### 2.2. Domain (`src/domain/`)

Чистые структуры данных без зависимостей.

- `Ticker`, `Side`, `OrderType`, `OrderStatus` — enum и value-types
- `PriceLevel { price, size, side }`
- `Trade { price, size, side, timestamp }`
- `OrderBookSnapshot`, `OrderBookUpdate`
- `OrderUpdate`, `PositionUpdate`, `BalanceUpdate`
- `RestOrder`, `PlaceOrderResult` — отдельные типы для REST
  `GET /api/connections/{id}/orders?Ticker=...` и
  `POST /api/connections/{id}/orders`, потому что REST и WS payload отличаются

### 2.3. Market Data (`src/marketdata/`)

Живая реплика стакана + агрегаты.

- `OrderBook` — реплика на **`absl::btree_map<PriceTick, SizeFix, ...>`**
  с **fixed-point ключами** (см. § 11.3):
  - `PriceTick = int64_t` = `price / TickerMeta.PriceIncrement` —
    точное равенство, никаких `epsilon`-сравнений; +30% к latency apply_update
    относительно `double` ключей (нет нормализации FP при каждом lookup)
  - `SizeFix = int64_t` = `size / SizeIncrement` — детерминизм аккумуляций
  - `absl::btree_map<PriceTick, SizeFix, std::greater<>>` — bids (по убыванию)
  - `absl::btree_map<PriceTick, SizeFix>` — asks (по возрастанию)
  - **Почему `absl::btree_map`:** cache-friendly B-tree layout, O(log N) с
    ~4× меньше cache-miss'ов чем `std::map` (red-black tree), стабильная
    производительность при 30+ тикеров × 1000 events/sec. Не `flat_map` —
    частые insert/erase на середине вектора дороже B-tree.
    *   **Важно:** Бенчмарк в T1-ORDERBOOK обязан подтвердить p99 apply_update < 200 µs **в условиях реальной нагрузки и джиттера**. Логарифмическая сложность операций `insert`/`erase` может стать узким местом при экстремально высокой частоте `orderbook_update`.

  - `apply_snapshot(OrderBookSnapshot)` — полная замена; ёмкость предзаложена `reserve(book_capacity_levels=128)`
  - `apply_update(OrderBookUpdate)` — branchless update path (`[[likely]]`
    на «обновить размер»), batched — параллельная обработка через `std::execution::par_unseq` для batch ≥ 8 уровней
  - `best_bid()`, `best_ask()`, `spread()`, `mid()` — **O(1) через cached
    top-of-book** (обновляется только при изменении первого уровня)
  - `depth(N)` — сумма объёма в первых N уровнях; для N=10 SIMD-summa AVX2.
    *   **Примечание:** Заявленные 2.5 нс на `depth(10)` являются **оптимистичной оценкой**. Реальный прирост производительности от SIMD будет сильно зависеть от фактического расположения данных в памяти (`absl::btree_map` не гарантирует плотного размещения) и накладных расходов на подготовку данных для векторных инструкций. Требуется тщательное профилирование.

  - `volume_at_range(price_low, price_high)` — объём в диапазоне цен (B-tree range scan)
  - **Hot-path constraints** (см. § 10.1): zero allocations,
    `noexcept` на всех публичных методах, никаких `std::string` в горячих
    структурах (тикер хранится как `TickerId = uint16_t` + lookup-table)
  - **Sanity check:** раз в `orderbook_sanity_check_sec` (default **30**)
    переподписка через `orderbook_subscribe` → получаем свежий snapshot →
    сверяем top-20 уровней с локальным. При расхождении > 3 уровней →
    WARN + полный resync (apply_snapshot). Защита от silent gap в WS.
- `ClusterSnapshot` (`src/marketdata/ClusterSnapshot.{hpp,cpp}`)
  - тонкая обёртка над `GET /api/connections/{id}/cluster-snapshot?Ticker&TimeFrame&ZoomIndex`
  - polling по `clusters.poll_interval_sec` для каждого тикера из `universe.active()` и каждого таймфрейма из `clusters.poll_timeframes`
  - используется `LevelDetector` (точки концентрации объёма из M5/M15/H1) и `ApproachAnalyzer` (распределение объёма у уровня)
  - результат — `std::vector<ClusterColumn>` с `Items[].{Price, AskSize, BidSize}`
- `TradeStream` — потоковые агрегаты на **incremental algorithms**
  (никаких полных пересчётов окна на каждый trade):
  - скользящее окно: ring-buffer `std::vector<Trade>` фикс. capacity (избегаем `std::deque` page allocations) + индекс по timestamp; eviction по deadline через `std::lower_bound`
  - **Welford's online algorithm** (§ 11.1) для `avg_print_size`, `stdev_print_size` — численно стабильный, O(1) на trade
  - **Hawkes process intensity** для `prints_per_sec`: `λ(t) = μ + Σ α·exp(-β(t-tᵢ))` — exponentially-decaying intensity. Бэйзлайн μ — из 30s окна; всплеск идентифицируется когда `λ(t) > k·μ` (для TapeBurst k=3, конфиг)
  - **CUSUM detector** (Page 1954) для `TapeFade` change-point: позитивная сторона CUSUM на `prints_per_sec - peak_rate * threshold` — раннее детектирование затухания на 200–500 мс быстрее наивного «сравнить current vs peak»
  - SIMD-prefix-sum для cumulative volumes по окнам 1s/5s/30s (одним проходом всё окно).
    *   **Примечание:** Эффективность SIMD для `TradeStream` также требует верификации. Накладные расходы на подготовку данных и относительно небольшие размеры окон могут нивелировать преимущества векторных операций. Профилирование является ключевым.

  - все «volume» поля — fixed-point (`int64_t` cents) с **Kahan summation** (§ 11.2)
  - эвент-колбэки `on_trade(Trade)` для TapeAnalyzer

- `LeaderTracker` — потоковый анализ корреляции и лага:
  - список поводырей (например: основной инструмент — `BTCUSDT`, поводырь — тоже `BTCUSDT` спот, либо для альта — `BTCUSDT` фьючерс)
  - для каждого: собственный `OrderBook` + `TradeStream`
  - **Online Pearson correlation** через Welford-расширение (Schubert &
    Gertz 2018, "Numerically Stable Parallel Computation of (Co-)Variance"):
    ```
    Δx = xₙ − x̄ₙ₋₁;  x̄ₙ = x̄ₙ₋₁ + Δx/n
    Δy = yₙ − ȳₙ₋₁;  ȳₙ = ȳₙ₋₁ + Δy/n
    Cₙ = Cₙ₋₁ + Δx · (yₙ − ȳₙ)
    Mxₙ = Mxₙ₋₁ + Δx·(xₙ − x̄ₙ);   Myₙ = Myₙ₋₁ + Δy·(yₙ − ȳₙ)
    corrₙ = Cₙ / sqrt(Mxₙ · Myₙ)
    ```
    O(1) на каждый sample, численно стабильный, не требует пересчёта окна.
  - **Оценка лага через Kalman filter** вместо argmax cross-correlation:
    state = `[lag_ms, lag_drift_ms_per_sec]`, observation = текущий
    наилучший сдвиг по короткому скользящему окну (10 сек). Преимущества
    над argmax: устойчивость к шумным наблюдениям, smooth tracking при
    дрифте лага во времени, явная uncertainty (`P` covariance) → используем
    как `confidence` для LeaderSignal.
    *   **Примечание:** Точность фильтра Калмана критически зависит от качества входных данных и их синхронизации. Различные задержки и джиттер от MetaScalp для разных тикеров могут привести к зашумленным наблюдениям, снижая точность оценки лага.
  - **fallback** при недостатке данных (warmup < 60 sec) — argmax
    cross-correlation с шагом 100 мс (та же логика, что описана в § 6
    SIGNAL_DETECTION).


### 2.4. Feature Extractor (`src/features/`)

Снапшот всех наблюдаемых фич на момент T (частота 10–20 Гц, настраиваемо).

#### FeatureFrame

```cpp
struct FeatureFrame {
    std::chrono::nanoseconds timestamp;
    Ticker ticker;

    // Order book
    double best_bid;
    double best_ask;
    double mid;
    double spread_abs;
    double spread_bps;      // bps = (ask-bid)/mid * 10000
    double bid_depth_10;    // объём в первых 10 уровнях bid
    double ask_depth_10;
    double imbalance;       // (bid_depth_10 - ask_depth_10) / (bid_depth_10 + ask_depth_10) ∈ [-1, 1]

    // Trade stream
    double buy_vol_1s, buy_vol_5s, buy_vol_30s;
    double sell_vol_1s, sell_vol_5s, sell_vol_30s;
    double prints_per_sec;
    double avg_print_size;
    double max_print_size_5s;
    double tape_aggression;  // (buy - sell) / (buy + sell) за окно

    // Price dynamics
    double price_change_1s;  // в %
    double price_change_5s;
    double price_change_30s;
    double volatility_1min;      // stdev лог-доходностей (безразмерный)
    double volatility_1min_bps;  // та же волатильность, выраженная в bps
                                 // (используется фильтром STRATEGIES.md § 5)

    // Leader
    double leader_change_1s; // цена поводыря, %
    double leader_change_5s;
    double leader_correlation;  // rolling
    double leader_lag_ms;       // оценённый лаг

    // Levels (из LevelDetector)
    std::optional<double> nearest_support;
    std::optional<double> nearest_resistance;
    double dist_to_support_bps;
    double dist_to_resistance_bps;
};
```

Частота генерации настраивается (`feature_rate_hz` в конфиге, дефолт 10).

### 2.5. Signal Detectors (`src/signals/`)

См. `SIGNAL_DETECTION.md` — детальные формулы и пороги.

Все детекторы:
- подписаны на `FeatureFrame` и/или сырые события (trade, book update)
- порождают `Signal` со своим `SignalKind`
- пороги берутся из конфига (не хардкод)

```cpp
enum class SignalKind {
    DensityDetected,     // в стакане появилась плотность
    DensityRemoved,      // плотность ушла (манипуляция)
    DensityEating,       // плотность активно проедается
    IcebergSuspected,    // видимый размер < реализованного
    TapeBurst,           // всплеск агрессии в одну сторону
    TapeFade,            // затухание ленты
    TapeFlush,           // "прострел"
    LevelFormed,         // сформирован новый горизонтальный уровень
    LevelApproach,       // подход к уровню
    LevelRejection,      // отбой от уровня (закрытая свеча/tick отвергнут)
    LevelBreak,          // пробой уровня
    LeaderMove,          // поводырь двинулся, а мы — нет (лаг-возможность)
};

struct Signal {
    SignalKind kind;
    std::chrono::nanoseconds timestamp;
    Ticker ticker;
    double price;                // цена, к которой привязан сигнал
    double confidence;           // [0, 1], внутренняя уверенность детектора
    nlohmann::json payload;      // детали (размер плотности, окно расчёта...)
};
```

Шина `SignalBus` — просто `std::function<void(const Signal&)>` подписчики; никакой MQ не нужно для single-process.

### 2.6. Strategy Engine (`src/strategy/`)

См. `STRATEGIES.md` — бизнес-логика стратегий.

- `IStrategy` — интерфейс `void on_frame(const FeatureFrame&); void on_signal(const Signal&); std::optional<TradePlan> tick();`
- Конкретные стратегии: `BounceFromDensity`, `BreakoutEatThrough`, `LeaderLag`, …
- `StrategyEngine` — держит список активных стратегий, маршрутизирует фреймы/сигналы, собирает TradePlan'ы

#### TradePlan

```cpp
struct TradePlan {
    Ticker ticker;
    Side side;                   // Buy / Sell
    EntryType entry_type;        // Limit / Market (см. STRATEGIES.md § 1.4, § 2.4, § 3.4)
    double entry_price;          // для Limit/StopLimit
    double stop_price;           // обязательно
    double tp1_price;            // обязательно: первая цель (≥ 1R)
    std::optional<double> tp2_price;  // опционально: дальняя цель
    double tp1_size_ratio;       // сколько закрыть на TP1, per-strategy (0.5-0.7, см. STRATEGIES.md)
    double size_coin;            // размер позиции в монете
    double risk_usd;              // ожидаемый убыток в $ при срабатывании стопа
    std::string strategy_name;
    std::string reason;           // human-readable "почему вошли"
    std::vector<Signal> evidence; // какие сигналы обосновали вход
    std::chrono::nanoseconds valid_until;  // TTL — после срока план сбрасывается
};
```

### 2.7. Risk Manager (`src/risk/`)

См. `RISK_MANAGEMENT.md` — правила и числа.

Проверяет каждый `TradePlan` перед отправкой в Executor:
- лимиты депозита
- максимальное число позиций одновременно
- дневной лимит убытка
- корректность сайзинга
- whitelist/blacklist тикеров
- текущее состояние "морж"

Возвращает `RiskDecision { accepted; reason; adjusted_size }`.

### 2.8. Executor (`src/executor/`)

- принимает одобренный `TradePlan`
- размещает entry-ордер через `OrderGateway.placeOrder`
- при филле — размещает stop и TP
- слушает `order_update`/`position_update` по WS и обновляет внутреннее состояние `ActiveTrade`
- при триггере стопа / TP1 / TP2 — частичное закрытие либо закрытие полностью
- logging каждого действия

#### ActiveTrade

```cpp
struct ActiveTrade {
    TradePlan plan;
    int64_t entry_order_id;
    std::optional<int64_t> stop_order_id;
    std::optional<int64_t> tp1_order_id;
    std::optional<int64_t> tp2_order_id;
    double filled_size;
    double avg_fill_price;       // = PositionUpdate.AvgPriceFix (см. README OQ-5): weighted avg entry orders only
    double avg_fill_price_dyn;   // = PositionUpdate.AvgPriceDyn (см. README OQ-5): adjusted by realized exit profit
    TradeState state;            // state-machine ниже
    std::chrono::steady_clock::time_point opened_at;
    double reserved_margin_usd;  // локальный резерв до BalanceUpdate/timeout (см. README OQ-4)
};
```

#### State machine (обязательна для устранения race'ов cancel ↔ order_update)

```
PendingEntry  ── order_update(Closed, size=plan.size)        ─► Open
PendingEntry  ── order_update(Closed, size<plan.size) partial ─► PartialFill (см. ниже)
PendingEntry  ── submit_timeout_after_send + no match         ─► SubmitUnknown
SubmitUnknown ── recovery found order/position                ─► Open/Cancelling (по факту сервера)
SubmitUnknown ── operator ack no server order/position         ─► Closed
PendingEntry  ── cancel_sent                                  ─► Cancelling
Cancelling    ── order_update(Closed, size=0)                 ─► Closed
Cancelling    ── order_update(Closed, size>0) race!           ─► Open (cancel опоздал — позиция уже открыта)
Open          ── tp1/stop fill                                ─► Exiting
Exiting       ── position.size == 0                           ─► Closed
PartialFill   ── filled >= min_partial_fill_ratio * plan.size ─► Open (stop/TP на filled size)
PartialFill   ── filled <  min_partial_fill_ratio * plan.size ─► Closing (cancel остаток + market-close)
```

#### Правило partial fill (entry)

При частичном fill entry-ордера:
- Если `filled_size / plan.size >= min_partial_fill_ratio` (default **0.5**,
  конфиг `[executor]`) — продолжаем: cancel остаток entry, выставляем
  stop/TP на `filled_size`. R:R не меняется, абсолютный risk пропорционально
  меньше.
- Если `filled_size / plan.size < min_partial_fill_ratio` — cancel остаток
  entry + market-close заполненного. Overhead комиссий > profit potential
  на мелком объёме.

**Правило разрешения race'ов:** истина — подтверждённые события сервера:
`order_update` и `position_update`. В API у `order_update` есть `Time`, у
`position_update` timestamp не документирован, поэтому нельзя проектировать
логику на несуществующем `ServerTime`. Локальный cancel только помечает
состояние `Cancelling`; переход в `Closed` происходит по подтверждению сервера
или по REST reconciliation.

#### Ambiguous submit handling для `POST /api/connections/{id}/orders`

Решение — см. `spec/README.md § OQ-1` и `METASCALP_API_CONTRACT.md § 3`.
Документированный `GET /api/connections/{id}/orders?Ticker=...` принимает только `Ticker` и возвращает open
orders; `ts_window`/`Status`/history фильтров нет. Перед retry на timeout/5xx
`LiveExecutor` делает best-effort reconciliation:
1. проверяет локальный буфер recent `order_update` по этому `Ticker`;
2. делает `GET /api/connections/{id}/orders?Ticker=...`;
3. **для market-ордеров дополнительно:** `GET /api/connections/{id}/positions` —
   market исполняется мгновенно, в open orders его уже нет (`Closed`).
   Если появилась новая/увеличившаяся позиция по тикеру — считать ордер
   исполненным, создать `ActiveTrade` по фактической позиции;
4. сверяет `(Ticker, Side, Type, Price, Size)` для limit-ордеров.

Если совпадение найдено — retry отменяется. Если не найдено и ошибка случилась
после отправки request body, в live второй POST **не отправляется**: сделка
переходит в `SubmitUnknown`, тикер ставится на pause, дальше работает recovery
и операторский ack. Retry разрешён только когда HTTP-слой доказал, что request
body не был отправлен.

#### Recovery после рестарта бота

При старте `LiveExecutor`:
1. читает `journal/account_state.json` — снапшот последнего
   `AccountState` (см. RISK § 5)
2. выполняет `GET /api/connections/{id}/positions` один раз на connection и
   `GET /api/connections/{id}/orders?Ticker=...` по каждому тикеру из persisted active_tickers +
   static whitelist (API возвращает только open orders конкретного ticker)
3. для каждой найденной позиции создаёт `ActiveTrade` со `state=Recovered`
   и поднимает её в RiskManager как известную
4. для каждого висящего ордера: если он относится к recovered-позиции —
   продолжаем управление; если «сирота» (нет позиции) — отменяем
5. если после п.2–4 обнаружена позиция без стопа — выставляем
   emergency-stop по `max_recovery_stop_bps` (default 30 bps от
   avg_price) до полной проверки оператором

**Ограничение REST positions:** `GET /api/connections/{id}/positions`
возвращает только `AvgPrice`, не `AvgPriceFix`/`AvgPriceDyn` (подробнее —
`METASCALP_API_CONTRACT.md § 9`). При recovery используем `AvgPrice` как
приближение к `AvgPriceFix` (совпадают, пока нет partial exit). После
первого WS `position_update` значение обновляется на настоящий
`AvgPriceFix` для корректного BE-стопа.

Детали — T4-RECOVERY.

### 2.9. Logger & Journal (`src/logger/`)

- `spdlog` в app-log (человекочитаемые логи)
- `TradeJournal` — структурированный JSONL с полной трассировкой каждой сделки: вход, сигналы, PnL, выход
- `FeatureRecorder` (опционально) — пишет FeatureFrame в parquet/CSV для оффлайн-анализа

### 2.10. Config (`src/config/`)

TOML (через `toml++` или `cpptoml`). Пример структуры см. в `TASK_SPECS.md § T0-CONFIG`.

Все пороги из `SIGNAL_DETECTION.md` и лимиты из `RISK_MANAGEMENT.md` задаются в конфиге.

### 2.11. Ticker Universe (`src/universe/`)

Модуль, который **сам подбирает монеты под стратегии**. На бирже сотни перпов; вручную
прописывать whitelist бессмысленно. TickerUniverse строит две вложенные совокупности:

1. **Pool** — широкий пул кандидатов (десятки), прошедших базовые фильтры
   ликвидность/спред/торгуемость. Это то, на что мы вообще согласны
   подписать MarketDataFeed.
2. **Strategy affinity** — для каждого тикера из pool → набор стратегий,
   которые **подходят именно ему** по их собственным критериям
   (см. `STRATEGIES.md § предусловия` каждой стратегии).

Стратегия активируется на тикере только если он прошёл и pool-фильтр, и её
персональный affinity-фильтр. Это и есть «бот сам выбирает, что и чем торговать».

#### Источники данных (всё API-эндпоинты MetaScalp, использованные модулем)

| Источник | Эндпоинт / поток | Что даёт |
|----------|------------------|----------|
| Каталог бирж | `GET /api/connections` | список активных подключений, `State==Connected`, `ViewMode==false`, выбор торгового коннекшена + опционально demo |
| Каталог пар | `GET /api/connections/{id}/tickers` (refresh раз в `pool_refresh_min`) | `Name`, `BaseAsset`, `QuoteAsset`, `IsTradingAllowed`, `PriceIncrement`, `SizeIncrement`, `MinSize`, `MaxSize` |
| Pool warmup | `trade_subscribe` коротко (5–10 мин рекординг через MarketDataFeed) | rolling `volume_window_usd` → экстраполяция в 24h, avg_spread, `prints_per_sec`, оценочная волатильность |
| Volume profile | `GET /api/connections/{id}/cluster-snapshot?Ticker&TimeFrame=M5/M15/H1&ZoomIndex=1` (poll по таймеру для каждого pool-тикера) | footprint M5/M15/H1 → точки концентрации объёма (для LevelDetector + классификация approach) и грубая оценка «торгуется ли тикер активно сейчас» |
| Screener | WS `notification_subscribe` → `ScreenerNewCoin` | динамическое расширение pool — кандидат проходит pool-фильтр и допускается на warmup; поток app-wide, фильтр только по документированным полям (см. README OQ-3) |
| Подсказки активности | WS `notification_subscribe` → `BigTick`, `BigOrderBookAmount`, `BigOrderBookAmount2` | временный boost priority тикера в StrategyEngine на `boost_ttl_sec`; `ConnectionId` в payload не документирован (см. README OQ-3) |
| Настройки оператора | `GET /api/connections/{id}/orderbook-settings?Ticker=X` | `LargeAmountUsd` / `LargeAmountUsd2` — референс «крупного объёма» для конкретного тикера. Используется как авто-калибровка `signals.density.min_size_usd`. |
| Конфиг | `[universe.pool]`, `[universe.affinity.<strategy>]` | ручные allow/deny, пороги |

#### Двухуровневая модель

```
[ Все тикеры биржи ]
        │  static-фильтр (quote/glob/leveraged-deny/IsTradingAllowed)
        ▼
[ Pool кандидаты ]
        │  warmup на trade-stream + cluster-snapshot M5
        │  pool-фильтр: liquidity / spread / volatility / prints
        ▼
[ Active pool ]   ── публикуется как universe.active(), на это подписан MarketDataFeed
        │
        │  для каждого тикера из active pool:
        │  для каждой стратегии S из конфига:
        │    score = compute_affinity(t, S, current features)
        │    if score >= S.affinity_threshold: enable S on t
        ▼
[ Strategy affinity map ]   ── publishes (ticker, [strategy]) пары
        │
        ▼
StrategyEngine активирует стратегию S на тикере t (создаёт IStrategy-инстанс с конфигом S)
```

Affinity пересчитывается раз в `affinity_refresh_sec` (default **60 сек**) и при
каждом крупном событии (BigTick / BigOrderBookAmount по тикеру).

#### Pool-фильтры (`[universe.pool]`)

```toml
[universe.pool]
# статические
quote_assets = ["USDT"]
allow_patterns = ["*USDT"]                  # все USDT-перпы
deny_patterns  = ["*UP*", "*DOWN*", "*BULL*", "*BEAR*", "*1000*"]  # leveraged-токены
require_trading_allowed = true
manual_allow = []
manual_deny  = []

# динамические
warmup_sec = 600                  # сбор статистики
min_volume_24h_usd = 50_000_000   # минимально-приемлемый оборот для входа в pool
max_avg_spread_bps = 8            # средний спред в warmup
min_prints_per_sec_30s = 2        # активность ленты
min_volatility_1min_bps = 5       # «есть ли вообще движение»
max_pool_size = 30                # подписаться больше чем на 30 — расход CPU
pool_refresh_min = 30             # полный пересмотр каждые 30 мин
use_screener_notifications = true # ScreenerNewCoin → новый кандидат
boost_ttl_sec = 120               # как долго BigTick «греет» тикер
```

#### Strategy affinity — что считаем под каждую стратегию

Для каждой стратегии в `STRATEGIES.md` есть свой профиль кандидата. Все
числа — из конфига `[universe.affinity.<strategy>]`. Защита от ложных
срабатываний — у каждой стратегии минимальный affinity score `S.affinity_threshold`.

```toml
[universe.affinity.bounce]                # BounceFromDensity (STRATEGIES § 1)
min_volume_24h_usd = 100_000_000
max_avg_spread_bps = 3
min_density_events_per_hour = 4           # из DensityDetector за последний час pool-окна
min_levels_in_60min = 3                   # LevelDetector нашёл хотя бы 3 уровня
require_low_news_density = true           # в ближайший час нет high-importance новостей
affinity_threshold = 0.6

[universe.affinity.breakout]              # BreakoutEatThrough (STRATEGIES § 2)
min_volume_24h_usd = 100_000_000
max_avg_spread_bps = 4
min_density_events_per_hour = 3
min_eating_events_per_hour = 1            # DensityEating (см. SIGNAL_DETECTION § 1.1)
min_volatility_1min_bps = 8               # без волатильности пробои не работают
affinity_threshold = 0.55

[universe.affinity.leaderlag]             # LeaderLag (STRATEGIES § 3)
require_leader = "BTCUSDT"
min_correlation_60s = 0.6                 # rolling корреляция с BTC
min_leader_lag_events_per_hour = 2
max_avg_spread_bps = 3
min_volume_24h_usd = 50_000_000           # альты с меньшим оборотом тоже подходят
exclude_self_for_leader = true            # сам BTCUSDT не считается кандидатом для LeaderLag
affinity_threshold = 0.5

[universe.affinity.flush]                 # FlushReversal (STRATEGIES § 4) — Фаза 5
min_flush_events_per_day = 3              # хотя бы 3 прострела/день в истории
min_volume_24h_usd = 50_000_000
max_avg_spread_bps = 6
affinity_threshold = 0.4
```

#### Auto-калибровка порогов плотности (опционально)

`GET /api/connections/{id}/orderbook-settings?Ticker=X` возвращает `LargeAmountUsd` —
порог, который оператор MetaScalp выставил «как крупный объём» для UI этого
тикера. Если включено `[universe.calibration].use_orderbook_settings = true`,
TickerUniverse подменяет `signals.density.min_size_usd` для тикера на
`max(config.signals.density.min_size_usd, ob_settings.LargeAmountUsd)` —
бот не считает плотностью то, что меньше операторского порога.

#### Интерфейс

```cpp
class TickerUniverse {
public:
    // Пул-уровень
    bool   in_pool(const Ticker&) const;
    std::vector<Ticker> active() const;
    std::optional<TickerMeta> meta(const Ticker&) const;

    // Affinity-уровень
    bool is_tradable_by(const Ticker&, std::string_view strategy_name) const;
    std::vector<std::string> strategies_for(const Ticker&) const;
    double affinity_score(const Ticker&, std::string_view strategy_name) const;

    // Подписки
    using PoolChangeCb = std::function<void(const std::vector<Ticker>& added,
                                            const std::vector<Ticker>& removed)>;
    using AffinityChangeCb =
        std::function<void(const Ticker&, std::string_view strategy, bool enabled)>;
    void on_pool_change(PoolChangeCb);
    void on_affinity_change(AffinityChangeCb);

    // Подсказки live-активности (notification_subscribe)
    void on_big_tick(const Ticker&, double size_usd);
    void on_big_orderbook_amount(const Ticker&, double size_usd);
    bool is_boosted(const Ticker&) const;        // активен ли BigTick-boost сейчас
};
```

`RiskManager.R5` теперь спрашивает `is_tradable_by(plan.ticker, plan.strategy_name)` —
не просто «торгуем ли вообще тикер», а «торгуем ли его именно этой стратегией». Это
автоматически отбрасывает планы стратегии в неподходящем тикере.

`StrategyEngine` подписывается на `on_affinity_change` и динамически
включает/выключает стратегии: если у тикера `bounce` ушёл из affinity
(например, испарилась ликвидность) — соответствующий `IStrategy` instance гасится,
открытые позиции не трогаем (managed Executor-ом до закрытия).

#### Что НЕ делает TickerUniverse

- Не сигналит сделки — это детекторы.
- Не выбирает направление — это стратегии.
- Не размещает ордера — это Executor.
- Не торгует на `ViewMode`/`State!=Connected` коннекшенах — такие отбрасываются на старте.

### 2.12. External Feeds (`src/transport/external/`)

MetaScalp не даёт ряд данных, критичных для скальпинга крипто-фьючерсов.
Для них заводится **изолированный подслой** в `transport/external/` — те
же принципы graceful degradation и «нестабильный API изолирован в
транспортном слое».

| Фид | Endpoint | Где используется | Тикет |
|-----|----------|------------------|-------|
| Funding rate (perp) | Binance `GET /fapi/v1/premiumIndex`, Bybit `GET /v5/market/funding/history` | R13 funding blackout (RISK § 2) | T4-FUNDING |
| Liquidations feed | Binance WS `<symbol>@forceOrder`, Coinglass aggregator | `LiquidationDetector` для FlushReversal — отличает капитуляцию от шумного принта | T5-FLUSH |
| Open Interest | Binance `GET /fapi/v1/openInterest`, Bybit `GET /v5/market/open-interest` | Подтверждение FlushReversal-сетапов | T5-FLUSH |
| Historical klines (OHLCV) | Binance/Bybit `GET /klines` | T2-BACKTEST warmup LevelDetector, Фаза 5 | T5-BACKTEST |
| News calendar | ForexFactory ICS / TradingEconomics JSON export (кладёт оператор в `config/news_calendar.json`) | R12 news blackout | T0-NEWS |

#### Общие принципы

1. **ExternalFeed не блокирует торговлю при недоступности.** Если
   funding-feed упал — R13 **не блокирует** сделки, но пишет WARN и
   помечает `last_funding_update_age_sec` в метриках. Решение о
   kill-switch по external-feed отсутствию — отдельное per-feed правило
   (`external_feeds.<name>.staleness_kill_sec`, default 900 = 15 мин, с
   `external_feeds.<name>.kill_on_staleness=true`).
2. **Rate-limits соблюдаются.** Все клиенты имеют персональные лимиты
   (Binance = 1200 req/min weighted), конфиг `[external_feeds.<name>]`.
3. **Единый `IExternalFeed` интерфейс** — `last_value()`, `last_update_ts()`,
   `is_stale(max_age_sec)`.
4. **Никаких «умных» автоподписок.** Фид включается явно через
   `[external_feeds.<name>].enabled = true`.
5. **Маппинг тикеров.** MetaScalp-тикер → Binance-символ через
   `external_ticker_map` в конфиге (обычно 1:1 для USDT-перпов,
   но явный маппинг обязателен для защиты от путаницы spot/perp).

#### Модуль

```
src/transport/external/
├── IExternalFeed.hpp
├── BinanceFundingClient.{hpp,cpp}       # REST premiumIndex polling
├── BinanceLiquidationsFeed.{hpp,cpp}    # WS forceOrder
├── BinanceOpenInterestClient.{hpp,cpp}  # REST openInterest polling
├── BinanceKlinesClient.{hpp,cpp}        # REST klines (offline/warmup)
├── ExternalFeedRegistry.{hpp,cpp}       # регистрирует все активные feed'ы,
│                                        # выдаёт их потребителям по enum FeedKind
└── FeedStalenessMonitor.{hpp,cpp}       # периодический чек is_stale → WARN/kill
```

#### Конфиг

```toml
[external_feeds.funding]
enabled = true
provider = "binance"                # binance | bybit | disabled
poll_interval_sec = 60              # premiumIndex обновляется ~раз в 8 часов + fundingTime, нам нужен ~минутный polling для R13
staleness_warn_sec = 120
staleness_kill_sec = 900
kill_on_staleness = false
external_ticker_map = { "BTCUSDT" = "BTCUSDT", "ETHUSDT" = "ETHUSDT", "SOLUSDT" = "SOLUSDT" }

[external_feeds.liquidations]
enabled = false                     # включается в Фазе 5 вместе с FlushReversal
provider = "binance"
ws_url = "wss://fstream.binance.com/ws/!forceOrder@arr"
min_size_usd = 50_000

[external_feeds.open_interest]
enabled = false
provider = "binance"
poll_interval_sec = 60

[external_feeds.klines]
enabled = false                     # только для оффлайн-warmup и backtest
provider = "binance"
```

---

## 3. Модель потоков

- **Main thread** — конфиг, оркестрация, сигналы ОС, graceful shutdown.
- **IO thread (asio)** — HTTP + WebSocket ввод-вывод (MetaScalp). Глобальный
  rate-limiter `metascalp_max_rps` (default **20**, конфиг) с приоритетной
  очередью: kill-switch > order placement > polling.
- **External-IO thread (asio)** — HTTP + WS для ExternalFeeds (Binance / Bybit). Отдельный io_context, чтобы медленные/заблокированные внешние соединения не мешали MetaScalp.
- **Processing thread** — обработка WS-сообщений: десериализация, обновление OrderBook/TradeStream, генерация FeatureFrame, прогон детекторов, маршрутизация сигналов стратегиям, формирование TradePlan, RiskManager, Executor.
  **Мониторинг queue depth per ticker:** при превышении `max_queue_depth`
  (default **500** сообщений) — дроп старых событий с WARN (а не зависание
  всей системы). Один тикер с аномальным потоком не должен блокировать
  processing для остальных.
- **Journal thread** — запись журнала сделок (лок-фри очередь → файл).
- **State-persist thread** — раз в `state_persist_interval_sec` (default 5 сек) атомарно сериализует `AccountState` + снапшот `ActiveTrade`s в `journal/account_state.json` (см. RISK § 5).

Одно ядро на processing — этого более чем достаточно для 10–20 Гц FeatureFrame и 100–1000 событий в секунду от MetaScalp.

**Никаких блокировок внутри processing потока.** Все очереди — lock-free (`boost::lockfree::spsc_queue` или `moodycamel::ConcurrentQueue`).

### 3.0. Graceful shutdown

Два режима остановки:
- **SIGTERM** = graceful: `StrategyEngine.pause()`, не открывать новые
  позиции, дождаться закрытия текущих по TP/stop (до
  `graceful_shutdown_timeout_sec`, default **300**), затем exit 0.
- **SIGINT / kill-switch** = экстренный: каноническая sequence из RISK § 4.

### 3.1. Латентные бюджеты (SLO, измеряется в T1-GATE и T2-GATE)

| Путь | p50 | p99 |
|------|-----|-----|
| WS orderbook frame → OrderBook applied | 0.5 мс | 2 мс |
| book update → FeatureFrame ready | 1 мс | 5 мс |
| FeatureFrame → Signal emitted (все детекторы) | 2 мс | 10 мс |
| Signal → TradePlan (StrategyEngine) | 1 мс | 5 мс |
| TradePlan → RiskDecision | 0.5 мс | 3 мс |
| **Total book→submit order (excl. network)** | **5 мс** | **25 мс** |

Измерения через `google/benchmark` (микро) + production-histogram
(`prometheus-cpp`). Нарушение p99 2 раза подряд — WARN, 5 раз подряд — алерт.

---

## 4. Модель времени

- `std::chrono::steady_clock` — для измерения интервалов
- `std::chrono::system_clock` — для отметок сделок в журнале (UTC)
- MetaScalp присылает timestamps как ISO-8601 строки — парсим в `system_clock::time_point`
- **В replay-режиме часы виртуализируются** (`ClockAbstraction`) — это нужно для детерминированных тестов

---

## 5. Конфигурация

Пример `config.toml`:

```toml
[connection]
primary_ticker = "BTCUSDT"
connection_id = 1
leaders = ["BTCUSDT"]  # для альтов: BTC как поводырь

[universe.pool]
quote_assets = ["USDT"]
allow_patterns = ["*USDT"]
deny_patterns = ["*UP*", "*DOWN*", "*BULL*", "*BEAR*", "*1000*"]
require_trading_allowed = true
warmup_sec = 600
min_volume_24h_usd = 50_000_000
max_avg_spread_bps = 8
min_prints_per_sec_30s = 2
min_volatility_1min_bps = 5
max_pool_size = 30
pool_refresh_min = 30
use_screener_notifications = true
boost_ttl_sec = 120

[universe.affinity.bounce]
min_volume_24h_usd = 100_000_000
max_avg_spread_bps = 3
min_density_events_per_hour = 4
min_levels_in_60min = 3
affinity_threshold = 0.6

[universe.affinity.breakout]
min_volume_24h_usd = 100_000_000
max_avg_spread_bps = 4
min_density_events_per_hour = 3
min_eating_events_per_hour = 1
min_volatility_1min_bps = 8
affinity_threshold = 0.55

[universe.affinity.leaderlag]
require_leader = "BTCUSDT"
min_correlation_60s = 0.6
min_leader_lag_events_per_hour = 2
max_avg_spread_bps = 3
min_volume_24h_usd = 50_000_000
exclude_self_for_leader = true
affinity_threshold = 0.5

[universe.calibration]
use_orderbook_settings = true   # авто-калибровка density.min_size_usd по LargeAmountUsd

[notifications]
subscribe = true                         # WS notification_subscribe app-wide, см. README OQ-3
forward_big_tick_to_universe = true      # BigTick → boost priority в universe
forward_big_amount_to_signals = false    # BigOrderBookAmount → SignalBus (по умолчанию выкл, чтобы не дублировать DensityDetector)

[features]
rate_hz = 10
trade_window_sec = [1, 5, 30]  # окна агрегации ленты
price_window_sec = [1, 5, 30]
orderbook_depth = 10

[signals.density]
min_size_vs_avg = 10.0         # плотность ≥ 10× среднего размера заявки в книге
min_distance_bps = 5           # не ближе 5 bps от mid (иначе это просто best)
sticky_duration_ms = 2000      # должна "стоять" ≥ 2 сек, чтобы считаться плотностью
fake_threshold_ms = 300        # если снят раньше 300 мс — "ложная" плотность

[signals.tape]
burst_ratio = 3.0              # buy_vol / sell_vol ≥ 3 (или наоборот)
burst_min_volume_usd = 50000   # минимум $50k за окно
fade_ratio = 0.2               # скорость упала до 20% от пика

[signals.level]
touches_min = 2                # минимум 2 касания для валидного уровня
lookback_min = 60

[risk]
max_daily_loss_pct = 3.0       # "морж"
max_per_trade_risk_pct = 0.5   # 0.5% от капитала на сделку
max_concurrent_positions = 3
whitelist_tickers = ["BTCUSDT", "ETHUSDT", "SOLUSDT"]

[executor]
use_market_for_entry = false   # Limit по умолчанию
entry_timeout_sec = 10         # если не исполнилось за 10 сек — отменить (см. STRATEGIES.md § 0.4)
stop_slippage_bps = 10         # защита от slippage
max_recovery_stop_bps = 30     # emergency-stop для recovered позиции без стопа (T4-RECOVERY)
ambiguous_submit_reconcile_window_sec = 2  # см. README OQ-1: окно local recent order_update перед SubmitUnknown

[state_persist]
enabled = true
path = "journal/account_state.json"
interval_sec = 5

[clusters]
poll_timeframes = ["M5", "M15", "H1"]
poll_interval_sec = 300
poll_jitter_sec = 30             # см. README OQ-6: jitter против REST burst
max_concurrent_requests = 4      # см. README OQ-6: лимит параллельных cluster-snapshot

[external_feeds.funding]
enabled = true
provider = "binance"
poll_interval_sec = 60
staleness_warn_sec = 120
staleness_kill_sec = 900
kill_on_staleness = false
```

---

## 6. Кодовая структура

```
trade_bot/
├── CMakeLists.txt
├── conanfile.txt                 # libcurl, boost, spdlog, nlohmann_json, toml++, gtest
├── src/
│   ├── main.cpp
│   ├── transport/
│   │   ├── IHttpClient.hpp
│   │   ├── CurlHttpClient.{hpp,cpp}
│   │   ├── IWsClient.hpp
│   │   ├── BeastWsClient.{hpp,cpp}
│   │   ├── MetaScalpDiscovery.{hpp,cpp}
│   │   ├── OrderGateway.{hpp,cpp}       # обёртка над REST endpoint-ами
│   │   └── MarketDataFeed.{hpp,cpp}     # обёртка над WS
│   ├── domain/
│   │   └── Types.hpp
│   ├── marketdata/
│   │   ├── OrderBook.{hpp,cpp}
│   │   ├── TradeStream.{hpp,cpp}
│   │   └── LeaderTracker.{hpp,cpp}
│   ├── features/
│   │   ├── FeatureFrame.hpp
│   │   └── FeatureExtractor.{hpp,cpp}
│   ├── signals/
│   │   ├── Signal.hpp
│   │   ├── SignalBus.{hpp,cpp}
│   │   ├── DensityDetector.{hpp,cpp}
│   │   ├── IcebergDetector.{hpp,cpp}
│   │   ├── TapeAnalyzer.{hpp,cpp}
│   │   ├── LevelDetector.{hpp,cpp}
│   │   ├── ApproachAnalyzer.{hpp,cpp}
│   │   └── LeaderSignal.{hpp,cpp}
│   ├── strategy/
│   │   ├── IStrategy.hpp
│   │   ├── TradePlan.hpp
│   │   ├── StrategyEngine.{hpp,cpp}
│   │   ├── BounceFromDensity.{hpp,cpp}
│   │   ├── BreakoutEatThrough.{hpp,cpp}
│   │   └── LeaderLag.{hpp,cpp}
│   ├── universe/
│   │   ├── TickerUniverse.{hpp,cpp}
│   │   └── UniverseFilters.{hpp,cpp}
│   ├── risk/
│   │   └── RiskManager.{hpp,cpp}
│   ├── executor/
│   │   ├── Executor.{hpp,cpp}
│   │   └── ActiveTrade.{hpp,cpp}
│   ├── logger/
│   │   ├── Logger.{hpp,cpp}
│   │   └── TradeJournal.{hpp,cpp}
│   └── config/
│       └── Config.{hpp,cpp}
├── test/
│   ├── unit/
│   └── integration/
└── replay/
    └── dumps/                      # сохранённые WS-сессии для regression-тестов
```

---

## 7. Зависимости (3rd-party)

Через Conan / vcpkg:

| Библиотека | Назначение |
|------------|-----------|
| `libcurl` или `cpr` | HTTP клиент |
| `boost` (beast + asio) | WebSocket |
| `nlohmann_json` | JSON |
| `spdlog` + `fmt` | логирование |
| `toml++` | конфиг |
| `abseil-cpp` (`absl`) | `btree_map` для OrderBook, `flat_hash_map` для кешей, `Time` утилиты |
| `gtest` | unit-тесты |
| `benchmark` | микробенчмарки критичных горячих путей |

---

## 8. Транспорт: принципы работы с API MetaScalp

Потому что API нестабилен:

1. **Фактический контракт API** — `METASCALP_API_CONTRACT.md`, сверенный с
   `../metascalp-sdk/docs/MetaScalp-Api.md`. Если поля/endpoint нет там, код
   не должен на него опираться без demo contract test.
2. **Единственное место парсинга JSON** — `transport/` слой. Выше — только доменные типы.
3. **Имена полей в JSON — в одной таблице констант**, не разбросаны по коду:
   ```cpp
   namespace api::fields {
       inline constexpr auto kConnectionId = "ConnectionId";
       inline constexpr auto kTicker = "Ticker";
       // ...
   }
   ```
4. **Graceful degradation:** если в ответе появилось новое неизвестное поле — игнорируем; если исчезло известное — пишем WARNING, но не падаем.
5. **Ретраи** с экспоненциальным backoff только на сетевые ошибки и 5xx; на 4xx — сразу failure.
6. **Версия MetaScalp** в `/ping` логируется при старте; если major отличается от ожидаемого — большое жёлтое предупреждение в лог.

---

## 9. Что НЕ делаем

- Никаких REST-poll для данных, которые есть по WS (orders/positions/balances) — с **единственным исключением**: ambiguous-submit reconciliation после `POST /api/connections/{id}/orders` (см. § 2.8 + README OQ-1) и startup-recovery (§ 2.8 T4-RECOVERY).
- Никаких собственных "умных" ордеров поверх — стоп/TP ставим отдельными ордерами у биржи, не храним stop-loss-в-памяти. **Исключение:** если demo contract test покажет, что документированный `Type=Stop/StopLoss/TakeProfit` нельзя безопасно использовать, live не стартует; soft-stop разрешён только на paper/test до исправления API-контракта.
- Никакой автоматической подстройки параметров в рантайме без явного разрешения в конфиге — все оптимизации — оффлайн.
- Никакого ML в фазах 0-4. Только формальные правила. ML — отдельный трек.
- Никакого торговли на внешних фидах (Binance/Bybit напрямую) — ExternalFeeds только для справочных данных (funding, liquidations, OI), ордера всегда через MetaScalp.

---

## 10. Performance Engineering

Бот целит в **p99 book→signal < 5 мс** при 30 тикерах × 1000 events/сек.
Достигается комбинацией: hot-path discipline, cache-aware структуры,
SIMD, kernel-bypass IO. Реализация — тикеты `T0-PERF`, `T1-ORDERBOOK`
(бенчмарки), `T2-GATE` (нагрузочный тест).

### 10.1. Hot-path discipline

Processing thread обязан соблюдать (enforce'ится `clang-tidy`-чеками
+ runtime-tracer'ом в Debug):

- **Zero allocations** — никаких `new`/`std::string`/`shared_ptr`-make
  в hot path. Все аллокации — через `std::pmr::monotonic_buffer_resource`
  с pre-allocated arena (256 KiB на тикер, sized в config).
- **`noexcept`** на всех hot-path методах (компилятор устраняет EH-frames).
- **No virtual dispatch in inner loop** — детекторы вызываются через
  `std::variant<Detector...>` + `std::visit` (devirtualization)
  либо CRTP, не через указатель на `IDetector`.
- **No mutex** — только `boost::lockfree::spsc_queue` (transport→processing)
  и `moodycamel::ConcurrentQueue` (processing→journal/metrics).
- **No syscall** — лог в hot path только через ring-buffer (читается
  отдельным потоком и сбрасывается в spdlog batch'ами).
- **Branch prediction hints** — `[[likely]]` / `[[unlikely]]` (C++20)
  на ветках критичных детекторов (нормальная ветка update vs delete level).
- **Inline где надо** — `[[gnu::always_inline]]` на 1–3-инструкционных
  helper'ах (`bps_to_price_tick`, `tick_to_bps`).

### 10.2. CPU pinning + NUMA-awareness

- Processing thread → pinned на одно физическое ядро (`pthread_setaffinity_np`),
  изолированное через ядерный параметр `isolcpus=N` (или cgroup-level
  `cpuset`), HT-sibling этого ядра не используется (или тоже изолирован).
- IO threads (MetaScalp + External) — на других ядрах того же NUMA-node
  что и processing. Все аллокации hot-path arena — через `numa_alloc_onnode`
  для нашего NUMA-node.
- Конфиг `[performance.cpu]`:
  ```toml
  processing_cpu = 2          # CPU id под processing thread
  io_cpu = 3                  # MetaScalp IO
  external_io_cpu = 4         # ExternalFeeds IO
  journal_cpu = 5
  numa_node = 0
  enable_cpu_pinning = true   # off для dev-окружения
  ```
- `T0-PERF` обязан проверить, что система поддерживает `cpuset`/`numactl`,
  иначе fallback с WARN.

### 10.3. Memory layout & cache-friendly structures

- **`absl::flat_hash_map`** вместо `std::unordered_map` для всех hot-path
  словарей (`TickerId → OrderBook`, `OrderId → ActiveTrade`): 2–4× быстрее
  на lookup, swiss-table layout.
- **`absl::btree_map`** для OrderBook (см. § 2.3) — B-tree вместо RB-tree.
- **Struct-of-Arrays** для FeatureFrame полей, по которым итерируем
  батч'ами (детекторы): SoA вместо AoS даёт vectorization-friendly layout.
- **Padding до 64 байт** (cacheline) на структурах, которые шарятся
  между потоками (`CachelinePadded<T>`), чтобы избежать false sharing.
- **Pre-allocate** все ring-buffer'ы и lock-free queue'и фикс. capacity
  на старте (`spsc_queue<Trade, 65536>`).

### 10.4. Lock-free очереди и pipelining

```
Transport thread ─[spsc_queue]─► Processing thread ─[mpmc]─► Journal
                                                         └─► Metrics
                                                         └─► Dashboard WS
```

- Transport→Processing: `boost::lockfree::spsc_queue` (single-producer
  single-consumer, wait-free, no CAS).
- Processing→Journal/Metrics: `moodycamel::ConcurrentQueue` (MPMC, sub-µs).
- Backpressure: при заполнении очереди >`max_queue_depth` (см. § 3) —
  drop oldest events с WARN-метрикой, **никогда не блокируем** producer.

### 10.5. SIMD optimizations

Везде, где есть итерация по массивам doubles длиной ≥ 8 — используем
SIMD (AVX2 baseline). AVX-512 не используется.

| Операция | Где | Speedup |
|----------|-----|---------|
| sum/prefix-sum по `bid_depth_10` | `OrderBook::depth(N)` | 4× (AVX2, AVX-512 disabled) |
| Pearson correlation (Welford-vectorized) | `LeaderTracker::corr()` | 3× |
| Apply batched orderbook update (8+ levels) | `OrderBook::apply_update_batch` | 2× |
| KDE для cluster-snapshot уровней | `LevelDetector` | 5× |
| FFT-based cross-correlation (cufft если есть) | `LeaderTracker` (offline-warmup) | 10× |

Реализация — через `xsimd` или `Highway` (Google) — портируемо
(AVX2/NEON (AVX-512 disabled)), без ручного `_mm256_*`-кода.

### 10.6. io_uring для localhost REST

- Linux 5.6+ — io_uring для всех HTTP-вызовов к MetaScalp (REST-pol-ling
  cluster-snapshot, polling positions). p99 latency на localhost: ~30 µs
  (vs ~150 µs на blocking libcurl). Конфиг `[performance.io].use_io_uring=true`,
  fallback — обычный libcurl.
- WS остаётся на Boost.Beast — io_uring для WS-фреймов того не стоит,
  выигрыш < 5%.

### 10.7. Fixed-point arithmetic (см. § 11.3)

Цены и размеры в hot path — `int64_t` ticks. Преобразование в `double`
только на границах: вход (codec) и выход (TradePlan, Journal). Внутри —
никаких float compare, никаких `std::round`, никаких NaN-проверок.

### 10.8. Profiling & continuous benchmarking

- **`google/benchmark`** микро-бенчмарки на каждый hot-path метод
  (`apply_update`, `compute_features`, `correlate`).
- **`perf record` + FlameGraph** профилирование под нагрузкой 60 мин
  (часть T2-GATE).
- **HDR Histogram** (см. § 11.5) для production latency tracking
  (метрики Prometheus `*_latency_ms` — histogram, не gauge).
- **Continuous regression** — CI-job замеряет p50/p99 на эталонном
  replay-дампе, alert при росте >5% относительно main.

---

## 11. Numerical correctness & determinism

«Корректность чисел» = одинаковый PnL на одинаковом replay-дампе,
независимо от ОС/компилятора/часов системы.

### 11.1. Welford's online algorithm

Все rolling mean / variance / stdev — через **Welford's recurrence**
(numerically stable, O(1) update):

```
n   = n + 1
δ   = x − M
M   = M + δ/n                # running mean
M2  = M2 + δ·(x − M)          # running M₂ for variance
var = M2 / (n − 1)
```

Применяется в:
- `TradeStream` — avg/stdev print size
- `LeaderTracker` — Pearson correlation (extended Welford для co-variance)
- `FeatureExtractor` — `volatility_1min` (через rolling stdev лог-доходностей)
- `RiskManager` — average win/loss tracking для R10/R11 anti-tilt

**Не использовать** наивную сумму `Σx²/n − (Σx/n)²` — катастрофически
теряет точность при near-equal values (классическая float-катастрофа).

### 11.2. Kahan summation

Все **аккумуляторы PnL и комиссий** используют Kahan compensated summation:

```
sum = 0; comp = 0
for x in stream:
    y = x − comp
    t = sum + y
    comp = (t − sum) − y
    sum = t
```

Снижает накопленную ошибку с O(n·ε) до O(ε), при 100k сделок разница
PnL: ~$5 (Kahan) vs ~$500 (naive) на $1M depo. Применяется в:
- `AccountState.realized_pnl_today_usd` accumulator (когда finres_update
  недоступен и работает self-tracking fallback)
- `TradeJournal` summary stats
- `TickerUniverse` warmup-volume aggregation

### 11.3. Fixed-point prices & sizes

```cpp
struct PriceTick { int64_t v; };           // price / PriceIncrement
struct SizeFix   { int64_t v; };           // size  / SizeIncrement
```

- **Точное равенство** уровней в OrderBook (нет epsilon-сравнений).
- **Детерминированный hash** для `flat_hash_map<PriceTick, ...>`.
- **Точная арифметика** в bps: `bps = (ask.v - bid.v) * 10000 / mid.v`
  без округления. Преобразования с `double` — только в codec
  (input) и Journal (output).
- TickerMeta хранит `PriceIncrement` / `SizeIncrement` (см. § 2.11
  TickerMeta), при работе с тикером бот пользуется значениями из meta.
- **Альтернатива** — `boost::multiprecision::cpp_dec_float_50` для
  PnL-PnL агрегатов (decimal128), исключает float-drift при умножении
  цены на размер. Но в hot-path `int64_t` достаточно.

### 11.4. Deterministic replay

- **Stable iteration order** — все хеш-таблицы с явным `std::map`/`btree_map`
  если порядок важен (Journal records, debug dump). `flat_hash_map` —
  только в hot-path, где порядок не важен.
- **Fixed-seed RNG** — `std::mt19937 rng(42)` для всего недетерминизма
  (slippage в paper, jitter в polling). Seed конфигурируется
  `[performance.deterministic].seed = 42`.
- **No system_clock в логике** — только `steady_clock` (monotonic) +
  явный offset до wall-time. ClockDriftMonitor (T0-CLOCK) проверяет дрифт.
- **Replay invariants test** — replay-роуни-тест прогоняет тот же дамп
  10 раз, проверяет PnL bit-identical (T2-GATE).

### 11.5. HDR Histogram для latency

Prometheus `*_latency_ms` метрики — `HdrHistogram_c`-based (не Prom
default linear buckets) для точных p50/p99/p99.9 на динамическом диапазоне.

- 3 significant digits — точность 0.1% по всему диапазону [0.001 мс, 60 сек]
- O(1) update, O(log) percentile query
- Метрики: `book_to_feature_us`, `feature_to_signal_us`, `signal_to_plan_us`,
  `plan_to_submit_us`, `order_rtt_us`.

### 11.6. EMA / DEMA для smoothing

Где требуется smoothing — **DEMA (Double EMA)**, не SMA:

```
EMA(x, α) = α·x + (1−α)·EMA_prev
DEMA(x)   = 2·EMA(x) − EMA(EMA(x))
```

DEMA реагирует в ~2× быстрее SMA того же окна при той же noise-rejection.
Применяется:
- `TickerUniverse.score smoothing` (избегает дрожания на границе threshold)
- `LeaderTracker.lag_ms` (между Kalman-обновлениями)
- `tape_aggression` smoothing для StrategyContext

---

## 12. Algorithm catalog (cross-reference)

Где какой топовый алгоритм используется. Полная мотивация — в `SIGNAL_DETECTION.md`
(детекторы) и `STRATEGIES.md § 6` (regime classifier).

| Алгоритм | Где используется | Почему |
|----------|------------------|--------|
| **Welford's online** (§ 11.1) | `TradeStream`, `LeaderTracker`, `FeatureExtractor` | numerically stable rolling stats |
| **Kahan summation** (§ 11.2) | PnL accumulator, fee tracker | low error при больших N |
| **Kalman filter** | `LeaderTracker.lag_ms` оценка | smooth tracking + uncertainty |
| **Hawkes process** | `TapeBurst` intensity | exponentially-weighted burst detection |
| **CUSUM (Page 1954)** | `TapeFade` change-point | раннее детектирование на 200–500 мс |
| **DBSCAN** | `LevelDetector` clustering экстремумов | стабильнее grid-кластеризации, не требует known-N |
| **KDE** (Kernel Density Estimation) | `LevelDetector` cluster-snapshot уровни | плавная оценка density по volume profile |
| **HMM** (3-state) | `StrategyEngine.classify_regime()` | trend / range / news state inference |
| **HDR Histogram** | latency metrics | accurate p99/p99.9 |
| **t-digest** | online quantiles trade-size distribution | O(log) merge, accurate tails |
| **Wilson score interval** | precision/recall CI в T2-LABELING | корректный CI на малой выборке |
| **DEMA** (§ 11.6) | smoothing affinity score, lag | 2× быстрее SMA при той же noise rejection |
| **Bayesian update** | `IcebergDetector` evidence | накопление uncertainty, не binary count |
| **xsimd / Highway SIMD** | OrderBook depth, KDE, correlation | AVX2 portable (AVX-512 disabled) |
| **io_uring** (Linux 5.6+) | localhost REST к MetaScalp | ~5× snizhenie p99 latency |
| **absl::flat_hash_map / btree_map** | hot-path containers | swiss-table & B-tree cache-friendly |

Все алгоритмы — **deterministic** (kernel widths, seeds — фиксированные
константы из конфига). Никакого недетерминированного ML в фазах 0–4.
