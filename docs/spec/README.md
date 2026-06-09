# Spec — документы проекта `trade_bot`

Это каталог со всей проектной документацией: дорожная карта, архитектура,
формализованные стратегии, правила риска и тикеты для ИИ-исполнителя.

**Аудитория:** разработчик (ИИ или человек), реализующий торгового бота
для [MetaScalp](../../metascalp-sdk/docs/MetaScalp-Api.md).

**Язык реализации:** C++23, CMake, Conan.
**Runtime:** headless-демон, подключается к MetaScalp на localhost (REST + WebSocket).
**Рынок:** крипто-фьючерсы (BTC/ETH/SOL-USDT perpetual), скальпинг (секунды–минуты).

> ⚠️ **Production.** Любой сценарий, влияющий на живые ордера, обязан
> соответствовать `RISK_MANAGEMENT.md`. Отсебятины быть не должно.

---

## Порядок чтения

Читать документы строго в этом порядке — каждый следующий опирается на
предыдущий.

| # | Файл | О чём | Для кого |
|---|------|-------|----------|
| 1 | [`ROADMAP.md`](./ROADMAP.md) | Фазы разработки, milestones, критерии готовности каждой фазы | Менеджер + исполнитель |
| 2 | [`ARCHITECTURE.md`](./ARCHITECTURE.md) | Модули, структуры данных, потоки, кодовая структура, 3rd-party зависимости | Исполнитель |
| 3 | [`METASCALP_API_CONTRACT.md`](./METASCALP_API_CONTRACT.md) | Фактическая поверхность MetaScalp API из SDK-документации: endpoints, WS events, ограничения | Исполнитель |
| 4 | [`STRATEGY_SOURCE_DIGEST.md`](./STRATEGY_SOURCE_DIGEST.md) | Выжимка ручных методических материалов из `docs/*.docx` | Исполнитель |
| 5 | [`LARGE_PARTICIPANTS_AND_INEFFICIENCIES.md`](./LARGE_PARTICIPANTS_AND_INEFFICIENCIES.md) | Traceability для крупных участников и неэффективностей: signal mapping, live-only/ambiguous gates | Исполнитель |
| 6 | [`SIGNAL_DETECTION.md`](./SIGNAL_DETECTION.md) | Формальные детекторы рыночных паттернов: плотности, айсберги, лента, уровни, подход, поводырь | Исполнитель |
| 7 | [`STRATEGIES.md`](./STRATEGIES.md) | Формализация торговых стратегий: BounceFromDensity, BreakoutEatThrough, LeaderLag, FlushReversal | Исполнитель |
| 8 | [`RISK_MANAGEMENT.md`](./RISK_MANAGEMENT.md) | 15 правил риска с числами, kill-switch, сайзинг, ежедневный ресет | Исполнитель + compliance |
| 9 | [`TASK_SPECS.md`](./TASK_SPECS.md) | Тикеты T0–T5 с acceptance criteria для каждой задачи | Исполнитель |

Приложения:

- Исходные ручные `.docx` лежат в `docs/`. Их нормализованная выжимка —
  [`STRATEGY_SOURCE_DIGEST.md`](./STRATEGY_SOURCE_DIGEST.md). Новая
  торговая логика должна быть привязана либо к этому digest, либо к явно
  указанному ручному источнику.

---

## Карта зависимостей

```
ROADMAP.md
   │
   └─► ARCHITECTURE.md
           │
           ├─► METASCALP_API_CONTRACT.md
           ├─► STRATEGY_SOURCE_DIGEST.md
           ├─► SIGNAL_DETECTION.md
           │        │
           │        ▼
           ├─► STRATEGIES.md  ──► RISK_MANAGEMENT.md
           │                              │
           └──────────────────────────────┼──► TASK_SPECS.md
                                          │       (тикеты реализации)
```

- `LARGE_PARTICIPANTS_AND_INEFFICIENCIES.md` уточняет ручные понятия крупных
  участников и неэффективностей перед тем, как они становятся detector/strategy
  requirements.
- `reports/FN-004-leaderlag-flushreversal-source-audit.md` фиксирует итоговые
  status labels для `LeaderLag` (`gated`) и `FlushReversal` (`gated` paper/offline,
  live `phase-later`) и должен сверяться при изменении этих стратегий.
- `STRATEGIES.md` строится поверх сигналов из `SIGNAL_DETECTION.md` и правил
  `RISK_MANAGEMENT.md`.
- Всё, что касается MetaScalp endpoints, WS events и JSON fields, сверяется с
  `METASCALP_API_CONTRACT.md`, который привязан к `metascalp-sdk` v1.0.7.
  Новые догадки о неописанных полях туда не добавляются без demo contract test.
- Числовые дефолты рисков и глобальных фильтров стратегий — **только** в
  `RISK_MANAGEMENT.md § 6`. `STRATEGIES.md § 5` ссылается на них по именам
  конфиг-параметров.

---

## Принципы работы с документами

1. **Один источник истины на один аспект.** Архитектура — только в
   `ARCHITECTURE.md`. MetaScalp API — только в `METASCALP_API_CONTRACT.md`.
   Числа рисков — только в `RISK_MANAGEMENT.md`. Детекторы —
   только в `SIGNAL_DETECTION.md`. Нельзя дублировать — иначе рано или
   поздно рассинхронизируется.
2. **Все пороги — из конфига.** Если в документе написано «default 10 bps»,
   значит в `config.toml` есть поле с этим значением и оно читается через
   `Config::get<T>`. Никаких хардкодов в коде.
3. **Реализовывать строго по тикетам.** Исполнитель берёт тикет из
   `TASK_SPECS.md`, делает строго то, что в нём описано. Выход за scope —
   новый тикет.
4. **Переход между фазами — через gate.** В каждой фазе `ROADMAP.md`
   определён критерий готовности. Не проходим gate → в следующую фазу не
   идём.
5. **Живые ордера — только после Фазы 4.** Фазы 0–3 — paper-trading,
   фаза 4 — risk-manager + live executor.

---

## Быстрая навигация по темам

| Тема | Документ | Раздел |
|------|----------|--------|
| Как подключиться к MetaScalp | `METASCALP_API_CONTRACT.md` + `../../metascalp-sdk/docs/MetaScalp-Api.md` | весь |
| Как устроен OrderBook в проекте | `ARCHITECTURE.md` | § 2.3 |
| Где задаётся частота FeatureFrame | `ARCHITECTURE.md` | § 2.4, § 5 |
| Как бот находит и фильтрует тикеры | `ARCHITECTURE.md` | § 2.11 (TickerUniverse) |
| Внешние фиды (funding, liquidations, OI) | `ARCHITECTURE.md` | § 2.12 (ExternalFeeds) |
| Поддержка screener-уведомлений MetaScalp | `TASK_SPECS.md` | T1-NOTIF |
| Серверные signal-levels MetaScalp | `TASK_SPECS.md` | T3-SIGLEVEL |
| Recovery после рестарта | `TASK_SPECS.md` | T4-RECOVERY |
| Funding blackout | `RISK_MANAGEMENT.md` | § 2 R13 |
| **Performance engineering** (CPU pinning, SIMD, lock-free, io_uring) | `ARCHITECTURE.md` | § 10 |
| **Numerical correctness** (Welford, Kahan, fixed-point, HDR histogram) | `ARCHITECTURE.md` | § 11 |
| **Algorithm catalog** (Kalman, DBSCAN, KDE, HMM, Hawkes, CUSUM, t-digest) | `ARCHITECTURE.md` | § 12 |
| Структура проекта (топ-уровень) | `README.md` (корень) | полное дерево |
| C++ ядро (src/) | `README.md` → `├── src/` | 18 поддиректорий |
| React-дашборд (dashboard/) | `dashboard/README.md` | 20+ компонентов |
| Документация (spec/, docs/spec/) | `docs/spec/README.md` | 10 документов |
| Тесты (test/) | `README.md` → `├── test/` | ~70 unit + integration + contract |
| Runtime данные (replay/, journal/) | `README.md` → `├── replay/, ├── journal/` | WS-dumps, JSONL журнал |
| Аудиты и инспекции (reports/) | `README.md` → `├── reports/` | 5 отчётов |
| Latency SLO (p50/p99 budgets) | `ARCHITECTURE.md` | § 3.1 |
| Numerical primitives infrastructure | `TASK_SPECS.md` | T0-NUMERIC |
| Performance infrastructure | `TASK_SPECS.md` | T0-PERF |
| Что такое «плотность» формально | `SIGNAL_DETECTION.md` | § 1 |
| Крупные участники и неэффективности | `LARGE_PARTICIPANTS_AND_INEFFICIENCIES.md` | весь |
| Когда входим на отскок | `STRATEGIES.md` | § 1 |
| Когда входим на пробой | `STRATEGIES.md` | § 2 |
| Размер позиции | `RISK_MANAGEMENT.md` | § 2 R8 |
| Морж | `RISK_MANAGEMENT.md` | § 2 R2 |
| Kill-switch | `RISK_MANAGEMENT.md` | § 4 |
| С чего начинать кодить | `TASK_SPECS.md` | T0-SETUP |
| Открытые вопросы к MetaScalp API | [`docs/spec/README.md § Open Questions`](#open-questions-к-metascalp-api) | ↓ |

---

## Статус документов

| Файл | Статус |
|------|--------|
| `ROADMAP.md` | ✅ v1.1 (уточнены фазы после hard-review) |
| `ARCHITECTURE.md` | ✅ v1.1 (+ ExternalFeeds § 2.12, WS-loss recovery, signal-levels) |
| `METASCALP_API_CONTRACT.md` | ✅ v1.1 (MetaScalp SDK v1.0.7: orderbook-snapshot, mark/funding, FetchSnapshot, stop trigger Price) |
| `STRATEGY_SOURCE_DIGEST.md` | ✅ v1.0 (ручные стратегии из DOCX, дубль `СТРАТЕГИИЯ.docx` удалён) |
| `LARGE_PARTICIPANTS_AND_INEFFICIENCIES.md` | ✅ v1.0 (формализация крупных участников/неэффективностей, signal mapping, live-only/ambiguous gates) |
| `SIGNAL_DETECTION.md` | ✅ v1.1 (минимум 200 положительных примеров, LiquidationDetector) |
| `STRATEGIES.md` | ✅ v1.2 (manual strategy gap-analysis: continuation, large participant, spot/futures dislocation) |
| `RISK_MANAGEMENT.md` | ✅ v1.2 (R1-R15, R14 runtime hard-kill, R15 slippage) |
| `TASK_SPECS.md` | ✅ v1.2 (+ T1-BOOK-SEED, SDK v1.0.7 order body, R1-R15 split) |

Изменения документов — только через PR с описанием, **что именно** меняется
и **почему**. Любая правка чисел в defaults обязана сопровождаться
ссылкой на backtest/эксперимент.

---

## Open Questions к MetaScalp API

Факты ниже сверены с `metascalp-sdk/docs/MetaScalp-Api.md` и сведены в
`METASCALP_API_CONTRACT.md`. Там, где API-документация закрывает вопрос, это
больше не гипотеза; там, где поля/семантика не описаны, live-торговля требует
contract test на demo.

### OQ-1. Идемпотентность `POST /api/connections/{id}/orders` — client-side ID

**Вопрос:** поддерживает ли MetaScalp передачу клиентского ID ордера
(по типу `clientOrderId` у Binance/Bybit), чтобы сетевой retry не
породил дубль ордера?

**Факт по API:** `POST /api/connections/{id}/orders` возвращает auto-generated `ClientId`.
В таблице `GET open orders` `ClientId` назван client-generated, но
документация не показывает request-поле для caller-supplied client id.
Поэтому `OrderGateway` не отправляет `ClientId`, `ClientOrderId` или другие
idempotency-key поля в request body.
`GET /api/connections/{id}/orders?Ticker=...` принимает только обязательный `Ticker` и возвращает open orders;
timestamp/status/history фильтров нет.

**Решение:** полноценный idempotent retry документированным API не решается.
Если HTTP-клиент знает, что request body не был отправлен (connect/write
failure before send), retry разрешён. Если timeout/5xx случился после отправки
body или статус неизвестен, `LiveExecutor` делает reconciliation: local recent
`order_update` + `GET /api/connections/{id}/orders?Ticker=...`. Если совпадение
по `(Ticker, Side, Type, Price, Size)` найдено — используется найденный order.
Если не найдено — **в live второй POST не отправляется**: сделка переходит в
`SubmitUnknown`, тикер ставится на pause, запускается recovery/операторский ack.
В paper можно тестировать повторный POST, но это не live-политика.

### OQ-2. Стоп-ордера: `Price` как trigger price

**Факт по API v1.0.7:** `POST /api/connections/{id}/orders` поддерживает
`Type=1 Stop`, `Type=2 StopLoss`, `Type=3 TakeProfit`. Для этих типов
`Price` документирован как trigger price. Отдельного request-поля
`TriggerPrice` нет. `GET /api/connections/{id}/orders?Ticker=...` показывает
`TriggerPrice`, если ордер уже создан.

**Решение:** до live всё равно выполнить demo contract test: поставить
`Stop`/`StopLoss`/`TakeProfit` с `Price=X` и проверить, что REST open-orders
отражает `TriggerPrice == X`, а WS/fill-семантика соответствует ожиданиям.
Если тест не пройден, live не стартует. Soft-stop разрешён только как
paper/test fallback.

**Импакт:** soft-stop уязвим к WS-разрыву — при потере связи стоп не
сработает. Потому в WS-loss kill-switch sequence (RISK § 4) добавлено
**принудительное закрытие позиций через REST market-close** при
`ws_reconnect_timeout_sec`.

### OQ-3. `notification_subscribe` — app-wide без ConnectionId

**Факт по API:** `notification_subscribe` принимает `{}` и является app-wide.
`ConnectionId` в notification payload не документирован. Доступные поля для
клиентского фильтра: `ExchangeId`, `MarketType` (string), `Ticker`, `Type`.

**Решение:** T1-NOTIF фильтрует только по документированным полям:
`ExchangeId`, `MarketType`, allowlist/pool тикеров. Фильтра по `ConnectionId`
для notifications нет.

Signal-level trading logic не опирается на app-wide notification stream:
`SignalLevelBridge` использует отдельный WS `signal_level_subscribe`, а
`notification_subscribe` остаётся каналом screener/activity/fallback.

### OQ-4. `BalanceUpdate` debounce и race с `POST /api/connections/{id}/orders`

**Вопрос:** есть ли гарантии консистентности `BalanceUpdate` относительно
уже принятых `POST /api/connections/{id}/orders`? Если debounce 500 мс, то между двумя
`place_order` баланс может не обновиться — R9 ('InsufficientMargin') даст
ложный pass.

**Факт по API:** `balance_update` debounced примерно 500 мс и содержит только
`ConnectionId`, `Balances[].{Coin, Total, Free, Locked}`. В нём нет `OrderId`,
`ClientId` или local reservation id.

**Safe-fallback:** `LiveExecutor` ведёт aggregate `balance_reservation`
(резерв маржи немедленно после отправки order) и вычитает его из
`free_balance_usd` перед проверкой R9. Резерв не матчит конкретный
`BalanceUpdate` по id; он снимается/пересчитывается при reject/cancel, при
подтверждённом `order_update`, после следующего `balance_update` по connection
или по `reservation_timeout_ms=1000`, что наступит раньше.

### OQ-5. `AvgPriceFix` vs `AvgPriceDyn` на `position_update`

**Факт по API:** поля есть на `position_update`, не на `order_update`.
`AvgPriceFix` — fixed average price, weighted average of entry orders only.
`AvgPriceDyn` — dynamic average price, adjusted by realized exit profit.

**Решение:** BE-стоп после TP1 считается по `PositionUpdate.AvgPriceFix`.
`AvgPriceDyn` используется для UI/аналитики, не как цена BE.

### OQ-6. Rate-limits REST / cluster-snapshot

**Вопрос:** каковы лимиты на REST-запросы `cluster-snapshot`, `tickers`,
`orders`? В документации лимиты не указаны.

**Safe-fallback:** конфиг `[clusters]` содержит `poll_jitter_sec` и
`max_concurrent_requests` (см. ARCH § 5), calibration — по живым замерам
в T1-CLUSTER.

---

По мере получения ответов от автора MetaScalp API — закрываем тикеты,
убираем fallback-логику, обновляем ARCH/RISK/CODEC.
