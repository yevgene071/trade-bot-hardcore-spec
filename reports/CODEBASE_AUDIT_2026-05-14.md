# Аудит кодовой базы trade-bot
**Дата:** 2026-05-14  
**Охват:** весь `core/src/` (~30 модулей), MetaScalp API сравнение

> **Вывод:** Зрелая production-grade кодовая база. 0 TODO/FIXME/HACK/stub/placeholder в `core/src/`. Из ~30 модулей — 27 полные, 1 частичный, 2 endpoint-а в DashboardServer функционально некорректны.

---

## Итоговая таблица статусов

| Модуль | Статус | Проблемы |
|---|---|---|
| BeastWsClient | ✅ ПОЛНЫЙ | — |
| MarketDataFeed | ✅ ПОЛНЫЙ | — |
| OrderGateway | ✅ ПОЛНЫЙ | — |
| SignalLevelGateway | ✅ ПОЛНЫЙ | `cleanup_triggered` не проверяет HTTP статус |
| ClusterSnapshotClient | ✅ ПОЛНЫЙ | — |
| NotificationFeed | ✅ ПОЛНЫЙ | Trade/SignalLevel kinds не роутятся (by design, но требует проверки) |
| **FinresHandler** | ⚠️ ЧАСТИЧНЫЙ | `get_snapshot` возвращает `result` и как balance, и как equity — задокументировано, но влияет на R9 |
| MetaScalpDiscovery | ✅ ПОЛНЫЙ | — |
| MetaScalpCodec | ✅ ПОЛНЫЙ | — |
| DumpRecorder | ✅ ПОЛНЫЙ | — |
| BounceFromDensity | ✅ ПОЛНЫЙ | — |
| BreakoutEatThrough | ✅ ПОЛНЫЙ | — |
| LeaderLag | ✅ ПОЛНЫЙ | — |
| TradePlan | ✅ ПОЛНЫЙ | — |
| LiveExecutor | ✅ ПОЛНЫЙ | — |
| PaperExecutor | ✅ ПОЛНЫЙ | — |
| RiskManager | ✅ ПОЛНЫЙ | R1–R13 + R14/R15 в Executor |
| SignalBus | ✅ ПОЛНЫЙ | — |
| SignalLevelBridge | ✅ ПОЛНЫЙ | — |
| OrderReconciliator | ✅ ПОЛНЫЙ | — |
| TradeJournal | ✅ ПОЛНЫЙ | — |
| **main.cpp** | ✅ ПОЛНЫЙ | 996 строк > лимита 500 — техдолг |
| **DashboardServer** | ❌ 2 ЗАГЛУШКИ | `POST /api/command` — no-op; `/api/killswitch/toggle` не дёргает реальный singleton |
| TickerController | ✅ ПОЛНЫЙ | — |
| TickerUniverse | ✅ ПОЛНЫЙ | — |
| OrderbookSettingsLoader | ✅ ПОЛНЫЙ | — |

---

## Критические баги (требуют немедленного решения)

### 🔴 1. `/api/killswitch/toggle` не работает (DashboardServer.cpp:466-478)

**Проблема:** Переключает только локальное поле `current_state_.kill_switch_active` (UI-состояние дашборда), но **не вызывает** `KillSwitch::instance()`. Реальный kill-switch с UI дашборда **не сработает** — бот продолжит торговать.

**Файл:** `core/src/control/DashboardServer.cpp`, строки 466–478

**Исправление:** Добавить вызов `KillSwitch::instance().activate()` / `deactivate()` при toggle.

---

### 🔴 2. `/api/command` — полная заглушка (DashboardServer.cpp:480-483)

**Проблема:** Endpoint принимает POST, возвращает `{"ok":true}` без чтения тела запроса и без какой-либо обработки. Все команды из UI (стратегии, ручные действия) тихо теряются.

**Файл:** `core/src/control/DashboardServer.cpp`, строки 480–483

**Исправление:** Распарсить `body["command"]`, маршрутизировать по команде (enable/disable strategy, force close, и т.д.).

---

## Средние проблемы

### 🟡 3. FinresHandler: balance == equity (FinresHandler.cpp:43-55)

**Проблема:** `get_snapshot()` возвращает один и тот же `result` и как `equity_usd`, и как `free_balance_usd`. Это задокументировано в комментарии:  
> *"For simplicity, we use it for both equity and free balance until separate BalanceUpdate/PositionUpdate logic is fully integrated."*

**Влияние:** R9 (margin check) в RiskManager сравнивает `free_balance_usd` с `equity_usd` — они всегда равны, проверка маржи некорректна для live margin-торговли.

**Файл:** `core/src/transport/FinresHandler.cpp`, строки 43–55

**Исправление:** Раздельный учёт equity и free balance из `BalanceUpdate` (уже приходит через MarketDataFeed как `balance_update`).

---

### 🟡 4. SignalLevel не роутится через NotificationFeed (NotificationFeed.cpp:109)

**Проблема:** Комментарий говорит «Trade and SignalLevel are ignored or routed elsewhere» — но explicit вызов `SignalLevelBridge::on_server_trigger` в NotificationFeed отсутствует. Требуется проверить, что signal_level_triggered реально обрабатывается через отдельный путь.

**Файл:** `core/src/transport/NotificationFeed.cpp`, строка 109

---

## Мелкие замечания (техдолг)

### 🟢 5. main.cpp — 996 строк (CLAUDE.md лимит 500)
**Файл:** `core/src/main.cpp`  
Класс `BotApp` со всей инициализацией. Кандидат на разбивку по TU: `BotApp`, `ComponentFactory`, `StrategyRegistry`.

### 🟢 6. cleanup_triggered без проверки HTTP статуса
**Файл:** `core/src/transport/SignalLevelGateway.cpp`, строки 65–69  
`http_.del()` fire-and-forget. Ошибки сервера проглатываются молча.

### 🟢 7. Дублирование to_ms / from_ms в MarketDataFeed
**Файл:** `core/src/transport/MarketDataFeed.cpp`, строки 12–30  
Функционально идентичны. Техдолг, не баг.

---

## MetaScalp API: что реализовано / что нет

### ✅ REST — реализовано

| Эндпоинт | Файл |
|---|---|
| `GET /ping` (порт-скан 17845–17855) | MetaScalpDiscovery |
| `GET /api/connections/{id}/orders?Ticker=` | OrderGateway |
| `GET /api/connections/{id}/positions` | OrderGateway |
| `GET /api/connections/{id}/balance` | OrderGateway |
| `POST /api/connections/{id}/orders` | OrderGateway |
| `POST /api/connections/{id}/orders/cancel` | OrderGateway |
| `POST /api/connections/{id}/orders/cancel-all` | OrderGateway |
| `GET /api/connections/{id}/cluster-snapshot` | ClusterSnapshotClient |
| `GET /api/connections/{id}/signal-levels?Ticker=` | SignalLevelGateway |
| `POST /api/connections/{id}/signal-levels` | SignalLevelGateway |
| `DELETE /api/connections/{id}/signal-levels/{id}` | SignalLevelGateway |
| `DELETE /api/connections/{id}/signal-levels?Ticker=` | SignalLevelGateway |
| `DELETE /api/signal-levels/triggered` | SignalLevelGateway |
| `GET /api/connections/{id}/orderbook-settings?Ticker=` | OrderbookSettingsLoader |

### ❌ REST — не реализовано

| Эндпоинт | Замечание |
|---|---|
| `GET /api/connections` | `connection_id` хардкодится из `config.toml` |
| `GET /api/connections/{id}` | не используется |
| `GET /api/connections/{id}/tickers?Refresh=` | universe захардкожен |
| `POST /api/change-ticker` | смена тикера не передаётся в MetaScalp UI |
| `POST /api/combo` | не используется |
| `PUT /api/connections/{id}/orderbook-settings` | только чтение, запись не реализована |

### ✅ WebSocket — реализовано

| Подписка / Event | Файл |
|---|---|
| `subscribe` (orders, positions, balance, finres) | MarketDataFeed |
| `trade_subscribe` / `trade_update` | MarketDataFeed |
| `orderbook_subscribe` / snapshot + incremental | MarketDataFeed |
| `mark_price_subscribe` / `mark_price_update` | MarketDataFeed |
| `funding_subscribe` / `funding_update` | MarketDataFeed |
| `notification_subscribe` / snapshot + update | NotificationFeed |
| `order_update`, `position_update`, `balance_update` | MarketDataFeed |
| `finres_update` | MarketDataFeed + FinresHandler |

### ❌ WebSocket — не реализовано

| Подписка | Замечание |
|---|---|
| `signal_level_subscribe` | управление через REST-опрос, WS-события `signal_level_placed/triggered/removed` не подписаны |

---

## Детальный разбор по модулям

### Транспорт

**BeastWsClient** — WebSocket клиент на Boost.Beast с SSL/TLS. Реконнект с экспоненциальным backoff (1→30s), ping каждые 20s, очередь записи с overflow protection (max 1000). Корректная фиксация mutex ordering (B8-FIX). Метрики: `trade_bot_ws_reconnects_total`.

**MarketDataFeed** — Диспетчер 10 типов WS-сообщений MetaScalp. Кеши funding/mark_price thread-safe. Resubscribe-on-reconnect. Record tap для DumpRecorder. Merged-listener cache с инвалидацией.

**OrderGateway** — Полный REST-клиент. Case-insensitive поиск массивов для устойчивости к PascalCase/camelCase ответам MetaScalp. Все HTTP-ошибки бросают `CodecError` с диагностикой.

**MetaScalpCodec** — Парсеры для 15+ структур. Поддерживает PascalCase и camelCase (`get_val` с auto-flip регистра). Парсинг ISO8601 с мс. Нормализация тикеров. 10 типов рынка.

### Стратегии

**BounceFromDensity** — Реальная торговая логика, не заглушка. C1–C9 условия из STRATEGIES.md §1.4. Post-entry monitoring: DensityRemovedPostEntry, BurstContraPostEntry. Стоп за плотностью. get_state() с 9 conditions + readiness_pct для UI.

**BreakoutEatThrough** — 8 условий: DensityEating ratio, TapeBurst alignment, Volume participation, Leader alignment, Resistance clusters, Support behind. Post-entry: FadeOnOurSidePostEntry, LeaderContraPostEntry.

**LeaderLag** — C1–C6 с LeaderMove signal, correlation check, Density on path. Реальный swing-low/high search для стопа (ring buffer 120 ticks). Post-entry: CorrelationBreakdown, LeaderReversal.

### Исполнение

**LiveExecutor (745 строк)** — Реальное выставление ордеров через `gateway_.place_order`. Partial fills с обновлением avg_entry_price. R15 Entry Slippage Control (emergency close > 5bps). BE-stop после TP1 (AvgPriceFix). Balance reservation. OrderReconciliator интеграция. Post-entry invalidation: no_progress_timeout, min_follow_through, R14 Single Position Loss Kill. Two-phase tick (collect под mutex, HTTP вне mutex).

**PaperExecutor** — Симуляция fill с gap protection. Slippage на entry и exit. SL/TP с gap protection. Post-entry зеркалирует LiveExecutor. Unrealized PnL по mark price.

### Риск

**RiskManager** — R1 Kill-switch, R2 Daily loss limit, R3 Concurrent positions, R4 Unique ticker/hedge, R5 Universe + strategy affinity, R6 Stop validation (per-ticker BPS-scaling через `inv_sqrt(volume_scale_factor)`), R7 TP validation (R:R ratio), R8 Sizing с size_increment normalization, R9 Margin, R10 Rate limit (B7-FIX, hard-cap deque), R11 Loss streak circuit breaker, R12 News blackout, R13 Funding blackout (T1-BUGFIX с prev_funding_times_).

### Торговля

**OrderReconciliator** — T0-ORDER-RECONCILIATION. Exp backoff (500ms→8s). Snapshot intents под lock, HTTP вне lock (B5-FIX). Matching с price tolerance 2bps + size tolerance 0.5%. Market orders price skip (T4-MATCHING).

---

## Следы исправлений в коде

Явно видны post-mortem fix-ы — признак системной работы над качеством:
`T0-BUGFIX`, `T1-BUGFIX`, `T4-RISK`, `T4-MATCHING`, `B5-FIX`, `B7-FIX`, `B8-FIX`, `B9-FIX`, `FIX #125`, `FIX #153`, `FIX #204`

---

## Приоритизированный план доработки

| # | Задача | Приоритет | Файл |
|---|---|---|---|
| 1 | Подключить `KillSwitch::instance()` в `/api/killswitch/toggle` | 🔴 КРИТ | DashboardServer.cpp:466 |
| 2 | Реализовать `/api/command` — маршрутизация команд | 🔴 КРИТ | DashboardServer.cpp:480 |
| 3 | Разделить balance vs equity в FinresHandler | 🟡 СРЕДНИЙ | FinresHandler.cpp:43 |
| 4 | Проверить/добавить signal_level_triggered routing | 🟡 СРЕДНИЙ | NotificationFeed.cpp:109 |
| 5 | `GET /api/connections` — авто-обнаружение connection_id | 🟡 СРЕДНИЙ | OrderGateway / main.cpp |
| 6 | `POST /api/change-ticker` — синхронизация с MetaScalp UI | 🟡 СРЕДНИЙ | TickerController |
| 7 | `signal_level_subscribe` WebSocket | 🟢 НИЗКИЙ | MarketDataFeed |
| 8 | `GET /api/connections/{id}/tickers` — динамический universe | 🟢 НИЗКИЙ | TickerUniverse |
| 9 | Разбить main.cpp (996 строк) на модули | 🟢 НИЗКИЙ | main.cpp |
| 10 | HTTP статус в `cleanup_triggered` | 🟢 НИЗКИЙ | SignalLevelGateway.cpp:65 |
