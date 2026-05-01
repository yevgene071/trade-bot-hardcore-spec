# RISK_MANAGEMENT

Правила риск-менеджмента торгового бота. Все числа — **дефолты** из
`config.toml` секция `[risk]` и `[external_feeds.*]`. Меняются только в
конфиге, не в коде. Сайзинг встроен в `[risk]` (поля `max_per_trade_risk_pct`,
`max_position_value_pct` и т.д.) — отдельной `[sizing]` секции **нет**.

Принцип: **у трейдера всегда должен быть завтрашний день/час/минута**.
Любой риск ограничен жёстко, любое превышение — kill-switch.

---

## 1. Входные данные RiskManager

`RiskManager.evaluate(plan: TradePlan, state: AccountState) -> RiskDecision`

`AccountState` собирается из WS-событий `balance_update`, `position_update`,
`finres_update` и внутреннего учёта открытых `ActiveTrade`. **Источник
истины для `realized_pnl_today_usd` — биржевой `finres_update`**, не
self-tracking (устраняет накопление расхождения при длительной сессии).

```cpp
struct AccountState {
    double equity_usd;            // текущий капитал (free + value of open positions)
    double starting_equity_usd;   // равен equity на момент открытия торгового дня (UTC 00:00)
    double realized_pnl_today_usd;    // finres.Result - finres_day_start_result_usd
    double finres_day_start_result_usd; // baseline Result из finres_update на UTC reset
    double unrealized_pnl_usd;
    int    active_positions;
    std::vector<Ticker> active_tickers;
    std::chrono::system_clock::time_point trading_day_start;
    bool   kill_switch_triggered;
};
```

---

## 2. Правила проверки (порядок применения)

Проверки выполняются в этом порядке, первая проваленная — отказ.

### R1. Kill-switch

Если `state.kill_switch_triggered == true` ⇒ **reject**, причина `KillSwitchActive`.

### R2. Дневной лимит убытка (морж)

```
daily_pnl_pct = (realized_pnl_today_usd + unrealized_pnl_usd) / starting_equity_usd * 100
```

Если `daily_pnl_pct <= -max_daily_loss_pct` (default **-3%**) ⇒ **reject**,
причина `DailyLossLimitHit`, и на весь оставшийся торговый день все
новые TradePlan отбрасываются (морж). Открытые позиции не трогаем,
но новых не открываем.

> **Правило из мануала:** как только трейдер схватил морж — стоп, никаких
> «отыгрываний». Завтра будет возможность.

### R3. Лимит одновременных позиций

Если `active_positions >= max_concurrent_positions` (default **3**) ⇒
**reject**, причина `TooManyPositions`.

### R4. Уникальность тикера

Если тикер из плана уже есть в `active_tickers` и новый план в **то же
направление** ⇒ **reject**, причина `DuplicatePosition`.
Если план в **противоположное** направление — допускается только в режиме
`allow_hedge = false` (default) даёт reject, при `allow_hedge = true` —
проходит.

### R5. Допуск тикера (universe)

Источник правды — `TickerUniverse::is_tradable(plan.ticker)` (см.
`ARCHITECTURE.md § 2.11`). Если возвращает `false` ⇒ **reject**, причина
`NotInUniverse`.

`TickerUniverse` строит список из каталога биржи (`GET /api/connections/{id}/tickers`) с
фильтрами по ликвидности, спреду и активности; дополнительно слушает
`notification_subscribe` для событий `ScreenerNewCoin` (динамическое
расширение).

**Fallback:** если `TickerUniverse` не инициализирован (например, в
интеграционных тестах), используется статический `whitelist_tickers` из
`[risk]`. Default: `["BTCUSDT", "ETHUSDT", "SOLUSDT"]`.

### R6. Валидация стопа

- `stop_price` должен быть на **противоположной** стороне от `entry_price` относительно `side`
- Дистанция стопа `stop_distance_bps = |entry - stop| / entry * 10000` должна быть в диапазоне `[min_stop_bps, max_stop_bps]` (default **[3, 20]**, см. § 6)
- Если `stop_distance_bps > warn_stop_bps` (default **15**) — WARN в логе (стоп «широковат для скальпа», но не reject)
- Если `stop_distance_bps < min_stop_bps` ⇒ **reject**, `StopTooTight`
  (бот получит стоп сразу на флуктуации рынка)
- Если `stop_distance_bps > max_stop_bps` ⇒ **reject**, `StopTooWide`
  (нарушаем принцип «короткий стоп»)

### R7. Валидация TP

- `tp1_price` должен быть по направлению сделки от `entry_price`
- `R` = `stop_distance_bps`, `T1` = `|tp1 - entry| / entry * 10000`
- Если `T1 < min_rr_ratio * R` (default **1.0**) ⇒ **reject**, `PoorRewardRisk`
- Если `tp2` задан: `T2 >= min_tp2_rr * R` (default **2.0**)

### R8. Сайзинг (вычисляем size, не только проверяем)

Формула fixed-risk:

```
risk_usd_target = equity_usd * max_per_trade_risk_pct / 100    # default 0.5%
size_coin = risk_usd_target / (entry_price * stop_distance_bps / 10000)
```

Затем клипируем:
- по `max_position_value_pct` (default 10% equity)
- по `min_order_size` и `max_order_size` инструмента (из `/api/connections/{id}/tickers`)
- округляем вниз до `size_increment`

Если после клипирования `size < min_order_size` ⇒ **reject**, `SizeBelowMinimum`.

Итоговый `risk_usd = size_coin * entry_price * stop_distance_bps / 10000`.

### R9. Проверка доступного баланса

Требуемая маржа: `required_margin_usd = size_coin * entry_price / leverage`
где `leverage` = min(`max_leverage` из конфига, фактическое плечо биржи).

Default `max_leverage = 5`.

Если `required_margin_usd > free_balance_usd * margin_safety_ratio` (default
**0.8**, то есть используем не более 80% свободного баланса) ⇒ **reject**,
`InsufficientMargin`.

### R10. Лимит сделок в единицу времени (anti-tilt)

Если за последние `trades_window_min` минут было `>= max_trades_per_window`
сделок (любого исхода) ⇒ **reject**, `TradeRateLimitHit`.

Defaults: `trades_window_min = 5`, `max_trades_per_window = 6`.

### R11. Лимит последовательных убытков

Если за последние `loss_streak_window_min` минут (**15** default) было
`>= max_consecutive_losses` (**3** default) убыточных сделок подряд ⇒
**reject**, `LossStreakCircuitBreaker`. Блок действует `loss_streak_cooloff_min`
(**10** default) минут.

### R12. Новостной блэкаут

Если до/после плановой новости (из внешнего календаря `news_calendar.json`)
меньше `news_blackout_min` (**5** default) минут ⇒ **reject**, `NewsBlackout`.

> **Источник news_calendar.json** — внешний, поставляется оператором или
> отдельным сервисом (например, экспорт из экономического календаря).
> В проект коммитится только пример `config/news_calendar.example.json` и
> schema. Загрузка/обновление календаря — задача оператора, не ботa.
> Реализация в тикете T0-NEWS (перенесён из Фазы 4, т.к. R12 нужен уже
> на paper-стадии Фазы 3).

**Валидация свежести календаря:** при старте и каждые `news_calendar_check_min`
(**60** default) проверяется `max(event.ts_utc)` в загруженном файле. Если
самое позднее событие в прошлом (< `now - 24h`) — **WARN** `NewsCalendarStale`
в лог. Если `news_calendar_require_fresh = true` (default **false**) и календарь
устарел — **reject** всех планов до обновления файла. Дополнительно: встроенный
фоллбэк-список recurring events (FOMC, NFP, CPI) с известным расписанием —
используется как baseline, если оператор не предоставил файл.

### R13. Funding blackout (крипто-перпы)

За `funding_blackout_pre_sec` (**30** default) секунд до и
`funding_blackout_post_sec` (**30** default) секунд после funding-момента
перпа — ⇒ **reject**, `FundingBlackout`.

Расчёт: для каждого тикера из `universe.active()` читается ближайший
funding time из `ExternalFeeds.funding.last_value(ticker)` (см.
ARCHITECTURE § 2.12). Если `abs(now_utc - funding_time) <= blackout_window` —
план отбрасывается.

**Fallback при недоступности funding-фида:**
- если `is_stale(external_feeds.funding.staleness_warn_sec)` — WARN в лог, R13 **не блокирует** (fail-open)
- если `is_stale(external_feeds.funding.staleness_kill_sec)` и
  `external_feeds.funding.kill_on_staleness=true` — R13 блокирует всё
  (fail-closed) + WARN в лог; после восстановления фида — автоматическое
  снятие блока

> **Зачем:** при funding перпа часто случаются "стеклянные" движения
> (manipulative spikes) за секунды до и после — скальперские стопы
> срабатывают в худшем slippage. Лучше пересидеть 60 сек.

### R14. Максимальный убыток на одну позицию (hard kill)

Если unrealized loss одной позиции превышает `max_single_position_loss_pct`
(default **1.5%** от `equity_usd`) ⇒ немедленный **market-close через REST**,
причина `SinglePositionLossExceeded`.

Это правило не ждёт WS — проверяется в processing-потоке по каждому
`position_update` и параллельно в WS-loss recovery poll. Защищает от
сценариев: стоп на бирже не сработал (OQ-2), WS упал, API вернул ошибку
на cancel.

```
for each active_trade in executor.active_trades():
    unrealized_pnl = compute_unrealized(trade, current_mid)
    loss_pct = -unrealized_pnl / equity_usd * 100
    if loss_pct >= max_single_position_loss_pct:
        market_close_via_rest(trade)  // минуя WS
        log::critical("R14: position {} exceeded max loss {:.2f}%", trade.ticker, loss_pct)
```

### R15. Контроль slippage на market-ордерах

После fill market-ордера (entry_type=Market) проверяется фактический
slippage: `actual_slippage_bps = |avg_fill_price - expected_entry_price| / expected_entry_price * 10000`.

Если `actual_slippage_bps > max_entry_slippage_bps` (default **5 bps**) ⇒
немедленный market-close, причина `EntrySlippageExceeded`. R:R
испорчен — дешевле закрыть с маленьким убытком, чем ждать TP с плохим входом.

Не применяется к limit-ордерам (fill по заявленной цене).

---

## 3. Результат проверки

```cpp
struct RiskDecision {
    bool accepted;
    RejectReason reason;          // enum из R1..R15
    double adjusted_size_coin;    // итоговый размер (после R8)
    double risk_usd;
    std::string details;          // human-readable почему
};
```

Все отказы логируются с полной детализацией (evidence из плана + state).

---

## 4. Триггеры kill-switch

Kill-switch = полная остановка торговли + отмена всех ордеров + флаг
`killswitch` на диске, чтобы после рестарта бот не стартовал автоматически.

Kill-switch срабатывает при любом из:

| Триггер | Условие |
|---------|---------|
| Ручной | файл `./killswitch` создан (watchdog проверяет каждые 500 мс) |
| Дневной лимит превышен на X% | `daily_pnl_pct <= -max_daily_loss_pct * 1.5` (резкое превышение, например из-за слиппеджа) |
| R14 single position loss | unrealized loss одной позиции > `max_single_position_loss_pct` — market-close этой позиции (не full kill, но алерт) |
| Серия ошибок биржи | `>= exchange_error_streak` (**5** default) подряд ошибок 5xx или сетевых таймаутов на `POST /api/connections/{id}/orders` |
| Drift времени | системные часы отошли от NTP > `max_clock_drift_ms` (**500** default) — торговля на замусоренном времени опасна (см. T0-CLOCK) |
| Потеря WS-соединения | WS не восстановилось за `ws_reconnect_timeout_sec` (**15** default) — без live данных не торгуем |
| Расхождение позиций | расхождение между внутренней моделью ActiveTrade и фактической позицией с биржи > `max(position_drift_coin, ticker.size_increment * 2)` на 2 подряд сверки (раз в 30 сек). Порог динамический — зависит от инструмента, чтобы избежать ложных срабатываний на округлении. |
| MetaScalp connection down | `GET /api/connections/{id}` вернул `State != 2 (Connected)` или HTTP error на 2 подряд health-poll (интервал `connection_health_poll_sec`, default **30**) |
| External feed stale | funding-feed `is_stale(external_feeds.funding.staleness_kill_sec)` И включён `external_feeds.funding.kill_on_staleness=true` (default false) |
| FinRes stale | нет `finres_update` дольше `finres_staleness_sec` (**60** default) при открытых позициях → переключение на self-tracking + WARN; при `finres_staleness_kill_sec` (**300**) и `finres_kill_on_staleness=true` (default false) → full kill |

### Действие kill-switch (каноническая sequence)

Выполняется **в этом порядке**, каждый шаг защищён таймаутом
`killswitch_step_timeout_sec` (default **5 сек**). Если шаг не успел —
логируется ERROR и переходим к следующему. **Общий дедлайн:**
`killswitch_total_timeout_sec` (default **45 сек**) — если kill-switch не
завершился за это время, процесс записывает `kill_switch_report.json` с
пометкой `incomplete=true` и вызывает `_exit(42)` немедленно.

1. **Пометить `kill_switch_triggered = true`** — дальнейшие планы
   отбрасываются RiskManager.R1, даже если этот процесс kill-switch упадёт.
2. **Отменить все открытые лимитные ордера.** `POST /api/connections/{id}/orders/cancel-all`
   **параллельно** по всем активным тикерам (`max_parallel_cancel = 5`,
   конфиг). API отменяет orders по одному `Ticker` за вызов;
   через REST, а **не** через WS — WS мог быть причиной kill. Все запросы
   отправляются одновременно через async io_context, результат собирается
   через futures с per-request таймаутом.
3. **Запросить актуальные позиции через REST.** `GET /api/connections/{id}/positions` — не
   полагаемся на внутреннюю модель ActiveTrade (возможно рассогласование
   из-за WS-разрыва).
4. **Закрыть все открытые позиции по рынку.** По каждой позиции из
   п.3 отправляем `POST /api/connections/{id}/orders` с `Type=Market` в противоположную сторону.
   Управляется `kill_switch_close_positions` (default **true**). Если
   выключено — только cancel-all + алерт.
5. **Poll `GET /api/connections/{id}/positions` каждые 500 мс до `kill_switch_close_timeout_sec`
   (default 30 сек)**, пока все позиции не закрыты. Если осталась
   позиция — повторная отправка market-close с отметкой `_retry_N`.
   После N=3 — ERROR + продолжаем.
6. **Создать файл `./killswitch`** и записать причину в первую строку
   (human-readable).
7. **Сбросить состояние на диск.** `journal/account_state.json` +
   `journal/kill_switch_report.json` (полный контекст: причина, ошибки
   шагов, финальные позиции, `incomplete` flag).
8. **Выйти с exit code 42.**

### WS-loss recovery sub-procedure (до kill-switch)

Если потеря WS < `ws_reconnect_timeout_sec` — бот **не торгует**, но
не убивается. Последовательность при потере WS при открытой позиции:

1. Стоп автоподписок на новые планы (StrategyEngine.pause()).
2. `OrderGateway.heartbeat()` — REST `GET /ping`. Если REST тоже упал —
   kill-switch шаг 3 сразу (закрываем рыночно).
3. REST poll `GET /api/connections/{id}/positions` каждые **500 мс** (не 2 сек —
   localhost REST < 5 мс, нагрузки нет; за 2 сек на BTC цена проходит
   50-100 bps) — отслеживаем PnL пока нет WS.
4. **R14 check на каждом poll:** если unrealized loss позиции >
   `max_single_position_loss_pct` — немедленный market-close, не ждём WS.
5. Если MID-цена ушла против нас больше `ws_loss_soft_stop_bps`
   (default 25 bps) — принудительное market-close через REST, **не ждём
   WS**. Это решает кейс когда soft-stop не мог сработать (OQ-2).
6. При восстановлении WS — идёт reconciliation через T4-RECOVERY
   процедуру (см. ARCH § 2.8).

---

## 5. Ежедневный ресет и persistence

Торговый день — UTC 00:00 – 23:59. При наступлении нового UTC-дня:
- `starting_equity_usd = current equity`
- `finres_day_start_result_usd = current finres.Result` по quote-currency
- `realized_pnl_today_usd = 0`
- морж-флаг снимается
- loss streak счётчик сбрасывается

Ресет выполняется через cron-колбэк `on_new_trading_day()`.

### Идемпотентность при рестарте

Ресет обязан быть **идемпотентным при рестарте** — если бот был перезапущен
в момент пересечения UTC 00:00, ресет не должен быть ни пропущен, ни
выполнен дважды.

Механизм:
1. Перед каждым применением ресета читается `journal/account_state.json`
   (см. ARCH § 3 — `state-persist thread`).
2. Если `last_reset_day_utc == today_utc` — ресет пропускается.
3. Иначе — ресет выполняется, записывается `last_reset_day_utc = today_utc`
   атомарно (через tmp-файл + rename).
4. `starting_equity_usd` пересчитывается как `realized_equity_at_00_00_utc`
   если он зафиксирован в persisted state (из полночного
   `finres_update`), иначе — как текущий `equity` на момент рестарта.
5. `realized_pnl_today_usd` не суммирует `finres_update`: MetaScalp API
   описывает `Finreses[].Result` как PnL since connection initialized.
   Поэтому дневной PnL = текущий `Result` quote-currency минус
   `finres_day_start_result_usd`.

### Persisted state format (`journal/account_state.json`)

```json
{
  "schema_version": 1,
  "last_persist_ts_utc": "2025-01-15T14:23:05.123Z",
  "last_reset_day_utc": "2025-01-15",
  "account_state": { ... AccountState serialized without persist metadata ... },
  "active_trades": [
    { "plan": {...}, "entry_order_id": 123, "state": "Open", ... }
  ],
  "kill_switch_triggered": false,
  "kill_switch_reason": null
}
```

Запись — атомарная через tmp + rename. Ротация — по суткам
(`account_state.YYYY-MM-DD.json.gz` архивы, 30 дней retention).

---

## 6. Табличка дефолтов

| Параметр | Default | Комментарий |
|----------|---------|-------------|
| `max_daily_loss_pct` | 3.0 | Морж |
| `max_per_trade_risk_pct` | 0.5 | % equity на один стоп |
| `max_single_position_loss_pct` | 1.5 | **R14:** макс. убыток одной позиции (% equity) → market-close через REST |
| `max_entry_slippage_bps` | 5 | **R15:** макс. slippage на market entry → немедленное закрытие |
| `max_position_value_pct` | 10 | Клип на объём |
| `max_concurrent_positions` | 3 | Параллельные сделки |
| `max_leverage` | 5 | Плечо |
| `margin_safety_ratio` | 0.8 | Используем ≤ 80% free |
| `min_stop_bps` | 3 | Минимальный стоп (0.03%) |
| `warn_stop_bps` | 15 | Выше — WARN в логе (стоп «широковат для скальпа») |
| `max_stop_bps` | 20 | Максимальный стоп (0.2%) — сохраняем R:R ≥ 1 при tp1 = 1.5R ≤ 30 bps |
| `min_rr_ratio` | 1.0 | TP1 ≥ 1R |
| `min_tp2_rr` | 2.0 | TP2 ≥ 2R |
| `max_trades_per_window` | 6 | / `trades_window_min` |
| `trades_window_min` | 5 | Окно anti-tilt |
| `max_consecutive_losses` | 3 | Порог loss-streak |
| `loss_streak_window_min` | 15 | Окно loss-streak |
| `loss_streak_cooloff_min` | 10 | Пауза после loss-streak |
| `news_blackout_min` | 5 | Пауза вокруг новостей (R12) |
| `news_calendar_check_min` | 60 | Интервал проверки свежести календаря |
| `news_calendar_require_fresh` | false | Блокировать торговлю при устаревшем календаре |
| `funding_blackout_pre_sec` | 30 | Пауза ПЕРЕД funding (R13) |
| `funding_blackout_post_sec` | 30 | Пауза ПОСЛЕ funding (R13) |
| `external_feeds.funding.staleness_warn_sec` | 120 | WARN при устаревшем funding-feed |
| `external_feeds.funding.staleness_kill_sec` | 900 | Порог устаревания funding-feed для R13 fail-closed |
| `external_feeds.funding.kill_on_staleness` | false | Включает fail-closed при превышении staleness_kill_sec |
| `finres_staleness_sec` | 60 | Нет finres_update дольше → переключение на self-tracking PnL + WARN |
| `finres_staleness_kill_sec` | 300 | Нет finres_update дольше + `finres_kill_on_staleness=true` → kill |
| `finres_kill_on_staleness` | false | Включает kill при полной потере finres |
| `allow_hedge` | false | Нельзя одновременно long+short |
| `whitelist_tickers` | `["BTCUSDT","ETHUSDT","SOLUSDT"]` | Fallback для R5 |
| `exchange_error_streak` | 5 | Порог kill-switch по ошибкам |
| `max_clock_drift_ms` | 500 | Порог kill-switch по дрифту |
| `ws_reconnect_timeout_sec` | 15 | Порог kill-switch по WS |
| `ws_loss_poll_interval_ms` | 500 | Интервал REST poll при WS-loss (localhost < 5 мс) |
| `position_drift_coin` | 0.0001 | Базовое расхождение; реальный порог = `max(position_drift_coin, ticker.size_increment * 2)` |
| `kill_switch_close_positions` | true | Закрывать ли позиции при kill |
| `kill_switch_close_timeout_sec` | 30 | Таймаут на полное закрытие |
| `killswitch_step_timeout_sec` | 5 | Per-step таймаут в sequence § 4 |
| `killswitch_total_timeout_sec` | 45 | Общий дедлайн kill-switch; после — `_exit(42)` |
| `max_parallel_cancel` | 5 | Параллельных cancel-all запросов в kill-switch |
| `ws_loss_soft_stop_bps` | 25 | Принудительный market-close при WS-loss если просадка больше |
| `connection_health_poll_sec` | 30 | Poll `GET /api/connections/{id}` для State==Connected |
| `metascalp_max_rps` | 20 | Глобальный rate-limit на REST-запросы к MetaScalp |
| `max_spread_bps` (strategies.common) | 3 | Макс. спред для торговли |
| `max_vol_bps` (strategies.common) | 50 | Макс. 1-мин волатильность в bps |
| `state_persist.interval_sec` | 5 | Период записи account_state.json |

---

## 7. Тестирование

Для каждого правила R1–R15 должен быть unit-тест в `test/unit/risk_manager_test.cpp`:
- позитивный: план проходит
- негативный: план проваливается, причина совпадает, детали заполнены
- R14 покрывается отдельным `r14_single_position_loss_test.cpp` (mid moves
  против позиции до `max_single_position_loss_pct` → market-close)
- R15 покрывается отдельным `r15_entry_slippage_test.cpp` (market fill
  с avg_price > expected на `max_entry_slippage_bps + 1` → close)

Integration-тест `test/integration/risk_flow_test.cpp`:
- симулируем день: пачку успешных сделок, серию убытков, превышение моржа
- проверяем, что бот корректно останавливается и не генерирует больше TradePlan

---

## 8. Что НЕ является задачей RiskManager

- **Не** выбирает, в какую сторону войти — это задача Strategy
- **Не** управляет стопами после входа — это задача Executor (который двигает стоп в БУ после TP1 и т.д.)
- **Не** рассчитывает комиссию — для планирования используется оценка `maker_fee_bps` / `taker_fee_bps` из конфига; реальная комиссия учитывается на стороне журнала

RiskManager — **только вратарь**: пропустить план в Executor или отбросить.
