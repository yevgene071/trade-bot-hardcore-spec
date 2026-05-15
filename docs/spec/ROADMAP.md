# ROADMAP

Дорожная карта разработки торгового бота для MetaScalp.

**Язык:** C++17/20, CMake, Linux/Windows
**Runtime:** headless-демон, подключается к MetaScalp по localhost (REST + WebSocket)
**Целевой рынок:** крипто-фьючерсы (BTC/ETH и альткоины), таймфрейм — скальпинг (секунды–минуты)

> API MetaScalp нестабилен (регулярно обновляется). До фазы 3 SDK не пишем —
> работаем напрямую с HTTP/WS через тонкий абстрактный слой, который легко
> перегенерировать под новую версию API. См. `ARCHITECTURE.md § 2.1`.

---

## Фазы

### Фаза 0. Фундамент (3 недели)
Цель: собранный пустой скелет, логи, конфиг, CI, mock-поток данных, **базовая perf/numeric инфраструктура**, safety-primitives.

- **M0.1** — CMake-проект, структура каталогов, `.clang-format`, `.clang-tidy` (`T0-SETUP`)
- **M0.2** — логгер (`spdlog`), конфиг (TOML), строгие флаги компилятора (`T0-LOG`, `T0-CONFIG`)
- **M0.3** — **`T0-NUMERIC`**: numerical primitives — fixed-point `PriceTick`/`SizeFix`, `WelfordAccumulator`, `KahanAccumulator`, DEMA, HdrHistogram (см. `ARCHITECTURE.md § 11`)
- **M0.4** — **`T0-PERF`**: performance infrastructure — CPU pinning, arena allocators, lock-free queues, SIMD wrappers (xsimd), LatencyTracer, benchmark suite (см. `ARCHITECTURE.md § 10`)
- **M0.5** — транспортный слой: HTTP (`T0-HTTP`) + WebSocket (`T0-WS`) + доменные типы (`T0-DOMAIN`) + JSON codec (`T0-CODEC`)
- **M0.6** — `IMarketDataSource` (`T0-FEED`) и `IOrderGateway` (`T0-GATEWAY`) — чтобы можно было подменить реальный MetaScalp на replay из файла
- **M0.7** — replay-источник (`T0-REPLAY`): читает сохранённый дамп WS-сообщений и проигрывает их с настраиваемой скоростью
- **M0.8** — **ClockDriftMonitor** (T0-CLOCK) — детект дрифта системных часов от NTP, триггер kill-switch
- **M0.9** — **NewsCalendar** (T0-NEWS, перенесён из Фазы 4) — нужен уже в paper-трейдинге, чтобы не обучаться на сделках, которые в live блокировались бы блэкаутом
- **M0.10** — kill-switch (`T0-KILLSWITCH`) + integration gate (`T0-GATE`): подключение → `discover` → `/ping` → `/api/connections` → подписка на trades/orderbook одного тикера → корректный приём N сообщений

**Критерий готовности фазы:** бот подключается к живому MetaScalp, подписывается на BTCUSDT, 60 секунд пишет в лог все события, ClockDriftMonitor активен, news calendar загружен (если предоставлен), benchmark suite зелёный (perf SLO baseline установлен), завершается без утечек.

---

### Фаза 1. Market Data & Feature Extractor (3 недели)
Цель: локальная реплика стакана + поток агрегированных фич.

- **M1.1** — класс `OrderBook` (`T1-ORDERBOOK`): bid/ask деревья, применение snapshot + incremental update
- **M1.2** — `TradeStream` (`T1-TRADESTREAM`) — скользящее окно сделок (N секунд), агрегированные метрики (объём buy/sell, скорость, средний размер принта)
- **M1.3** — `LeaderTracker` (`T1-LEADER`) — подписка на "поводырей" (BTC для альтов, ETH для альтов своего сегмента), расчёт корреляции и лагов
- **M1.4** — `FeatureFrame` (`T1-FRAME`) — снапшот всех фич на момент T (см. `ARCHITECTURE.md § 2.4`)
- **M1.5** — сериализация FeatureFrame в parquet/CSV для оффлайн-анализа
- **M1.6** — `TickerUniverse` (`T1-UNIVERSE`, см. `ARCHITECTURE.md § 2.11`): pool + per-strategy affinity
- **M1.7** — `cluster-snapshot` polling (`T1-CLUSTER`): footprint M5/M15/H1 как второй источник уровней
- **M1.8** — `notification_subscribe` (`T1-NOTIF`): screener (`ScreenerNewCoin` → новые кандидаты) + boost priority по `BigTick` / `BigOrderBookAmount`
- **M1.9** — auto-калибровка `density.min_size_usd` по `LargeAmountUsd` из orderbook-settings (`T1-CALIB`)
- **M1.10** — integration gate (`T1-GATE`) + unit-тесты на детерминированные сценарии (заранее подготовленные dump-файлы)

**Критерий готовности:** на replay-дампе бот строит идентичный стакан и генерирует FeatureFrame на 10 Гц без пропусков; `TickerUniverse` строит живой pool (≥ 5 тикеров) и для ≥ 1 тикера активна ≥ 1 стратегия по affinity-критериям.

---

### Фаза 2. Signal Detectors (4 недели; +1 неделя на labeling)
Цель: реализация формальных детекторов паттернов из `SIGNAL_DETECTION.md`.

- **M2.0** — `T2-SIGNAL-BASE` + `T2-LABELING` (параллельно с M2.1-M2.6) — SignalBus и сбор размеченной выборки ≥200 положительных и ≥400 отрицательных на каждый детектор (см. SIGNAL § 8)
- **M2.1** — `DensityDetector` (`T2-DENSITY`) — обнаружение плотностей (больших лимитных заявок) в стакане
- **M2.2** — `IcebergDetector` (`T2-ICEBERG`) — обнаружение айсбергов (реализация объёма сверх видимого)
- **M2.3** — `TapeAnalyzer` (`T2-TAPE`) — классификация ленты (агрессия / затухание / раздача / прострел)
- **M2.4** — `LevelDetector` (`T2-LEVEL`) — поиск горизонтальных уровней по истории (локальные хаи/лои, зоны скопления, cluster-snapshot)
- **M2.5** — `ApproachAnalyzer` (`T2-APPROACH`) — качество подхода к уровню (импульсный / медленный / с проторговкой)
- **M2.6** — `LeaderSignal` (`T2-LEADER`) — сигнал от поводыря (BTC пошёл вверх → альт с лагом X мс должен подтянуться)
- **M2.7** — бэктест детекторов (`T2-BACKTEST`) на размеченной выборке (с Wilson 95%-CI метрик)

**Критерий готовности:** `T2-GATE` — все 6 детекторов работают онлайн на живом потоке, выдают события в шину `SignalBus`, precision ≥ 70% на валидационном наборе с Wilson 95%-CI lower bound ≥ 60%, recall ≥ 50%.

---

### Фаза 3. Strategy Engine (4 недели)
Цель: три рабочие стратегии из `STRATEGIES.md` в режиме paper-trading.

- **M3.1** — `TradePlan` + `StrategyContext` + `StrategyEngine` — инфраструктура стратегий (см. TASK_SPECS `T3-PLAN`)
- **M3.2** — стратегия `BounceFromDensity` (`T3-BOUNCE`)
- **M3.3** — стратегия `BreakoutEatThrough` (`T3-BREAKOUT`)
- **M3.4** — стратегия `LeaderLag` (`T3-LEADERLAG`)
- **M3.5** — paper-executor (`T3-PAPER`): исполняет TradePlan против виртуального счёта на живых данных
- **M3.6** — `TradeJournal` (`T3-JOURNAL`): JSONL с полной трассировкой каждой сделки
- **M3.7** — **T3-SIGLEVEL**: серверные signal-levels от MetaScalp (`POST /api/connections/{id}/signal-levels` + `signal_level_subscribe`) — event-driven замена polling уровней

**Критерий готовности:** `T3-GATE` — минимум 48 часов paper-trading без крешей, все сделки логируются с полной трассировкой (какие сигналы, какие пороги, почему вошли), T3-SIGLEVEL эмитит `LevelTriggered` от сервера.

---

### Фаза 4. Risk Manager & Executor (4 недели)
Цель: живое исполнение с жёсткими лимитами.

- **M4.0** — **T4-EXTERNAL** + **T4-FUNDING**: ExternalFeedRegistry + Binance funding rate client (для R13)
- **M4.1** — `RiskManager` — проверки перед каждой сделкой (R1..R13 из `RISK_MANAGEMENT.md`), persistent state через T4-RISK
- **M4.2** — дневной лимит убытка (daily stop-out, R2 «морж»), идемпотентный UTC-ресет
- **M4.3** — размер позиции через формулу fixed-risk (% от депозита / дистанция до стопа)
- **M4.4** — **T4-FINRES**: финрезультаты от биржи как источник истины для realized_pnl
- **M4.5** — **T4-RECOVERY**: startup recovery — при рестарте поднимает открытые позиции/ордера, сверяет с persisted state, выставляет emergency-stop при необходимости
- **M4.6** — `LiveExecutor` (T4-EXECUTOR) — реальные ордера через REST `placeOrder` с ambiguous-submit handling без blind retry (см. README OQ-1), state-machine для race'ов, demo contract test для server-side stop/TP (OQ-2), aggregate balance reservation (OQ-4)
- **M4.7** — обработка отказов биржи (retries, circuit breaker после N ошибок подряд)
- **M4.8** — kill-switch с **каноническим sequence** (RISK § 4): `orders/cancel-all` по каждому ticker → `GET /api/connections/{id}/positions` → market-close → poll → persist → exit 42
- **M4.9** — WS-loss recovery sub-procedure (RISK § 4): REST-heartbeat, мониторинг PnL, принудительный market-close при ws_loss_soft_stop_bps
- **M4.10** — прод-дашборд (`T4-DASHBOARD`): текущий PnL, открытые позиции, последние 20 сделок, external feed staleness

**Критерий готовности:** `T4-GATE` — неделя непрерывной работы на демо-счёте с корректным соблюдением лимитов, zero over-risk events, успешное recovery после минимум 2 плановых рестартов с открытой позицией.

---

### Фаза 5. Оптимизация и бэктест (continuous)
- **M5.1** — бэктест-движок (`T5-BACKTEST`) на исторических дампах с той же логикой strategy engine
- **M5.2** — параметризация стратегий (thresholds в конфиге) + grid search (`T5-GRID`) по историческим данным
- **M5.3** — FlushReversal + LiquidationDetector (`T5-FLUSH`), метрики: win rate, profit factor, max DD, Sharpe, avg hold time
- **M5.4** — walk-forward validation (`T5-WALKFORWARD`)

---

## Общая таблица milestones

| Фаза | Длительность | Ключевой артефакт |
|------|--------------|-------------------|
| 0 | 2.5 нед | transport + replay + clock/news |
| 1 | 3 нед | `OrderBook` + `FeatureFrame` + universe |
| 2 | 4 нед | 6 детекторов сигналов + labeled dataset ≥200 |
| 3 | 4 нед | 3 стратегии в paper + signal-levels |
| 4 | 4 нед | live с риск-менеджером + recovery + external feeds |
| 5 | ∞ | бэктест, FlushReversal + LiquidationDetector, оптимизация |

**Итого до live:** ~17.5 недель (≈4.5 месяца) при одном разработчике full-time.

---

## Принципы, которых не отступаем

1. **Никаких стратегий без детекторов, никаких детекторов без данных** — строгая последовательность фаз.
2. **Ни одного реального ордера до фазы 4** — всё paper.
3. **Каждый сигнал и каждая сделка должны логироваться с полным контекстом** — чтобы разбирать пост-мортем (основа основ для скальпинга — разбор своих сделок).
4. **Kill-switch важнее фич** — делается в фазе 0 (файл-флаг), ужесточается в фазе 4.
5. **Replay-тесты — перед любым мерджем в main.**
