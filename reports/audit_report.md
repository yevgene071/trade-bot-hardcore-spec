# Критический аудит кодовой базы `trade_bot`

**Дата:** 2026-05-10  
**Покрытие:** 173 файла, ~520 KB, все модули  
**Итого найдено:** 4 критических · 4 высоких · 3 средних · 3 низких

---

## КРИТИЧЕСКИЕ (production money-at-risk)

---

### [C-1] Active trades не персистируются — startup recovery сломан полностью

**Файл:** `src/main.cpp:483`

```cpp
// каждые 10 секунд
persister_->save({account_state_, {}, last_reset_day_, false, ""});
//                               ^^
//          active_trades ВСЕГДА ПУСТОЙ
```

`PersistedData::active_trades` — это поле, которое `StartupRecovery::run()` сравнивает с реальными позициями на бирже, чтобы отличить «знакомые» позиции от «сирот». Поскольку поле всегда сохраняется пустым, при каждом рестарте все открытые позиции будут квалифицированы как orphan-positions. `drift_detected` всегда станет `true`, `auto_ack = false`, и бот будет требовать ручного подтверждения при каждом запуске, даже если всё в порядке.

Побочный эффект: emergency-стоп будет размещаться на каждую позицию при каждом рестарте, создавая дублирующиеся stop-ордера.

**Фикс:**

```cpp
// собрать active trades перед сохранением
std::vector<ActiveTrade> active_snapshot;
for (const auto& t : executor_->get_active_trades())
    if (t.state != TradeState::Closed)
        active_snapshot.push_back(t);

persister_->save({account_state_, active_snapshot, last_reset_day_, false, ""});
```

---

### [C-2] SL/TP-failure не генерирует алерт — позиция без стопа

**Файл:** `src/executor/LiveExecutor.cpp:394–418` (функция `place_stops_`)

```cpp
try {
    auto res = gateway_.place_order(connection_id_, sl);
    trade.stop_order_id = res.order_id;
} catch (const std::exception& e) {
    LOG_ERROR("LiveExecutor: failed to place SL for {}: {}", sl.ticker, e.what());
    // ← alert_cb_ НЕ вызывается
}
```

Если биржа вернула ошибку при размещении stop-loss, позиция остаётся открытой без защиты. `LOG_ERROR` попадёт в лог, но `alert_cb_` (webhook / Telegram) не вызывается. При высоком трафике лог может быть не замечен. Это самый опасный сценарий с финансовой точки зрения.

`error_streak_` при этом не увеличивается — инкремент счётчика происходит только в `submit()`, не в `place_stops_()`.

**Фикс:**

```cpp
} catch (const std::exception& e) {
    LOG_ERROR("LiveExecutor: failed to place SL for {}: {}", sl.ticker, e.what());
    if (alert_cb_)
        alert_cb_("CRITICAL: SL placement failed for " + sl.ticker + ": " + e.what());
    // опционально: принудительно закрыть позицию market-ордером
}
```

---

### [C-3] RiskManager создаётся с дефолтным конфигом — параметры из config.toml игнорируются

**Файл:** `src/main.cpp:150`

```cpp
risk_manager_ = std::make_unique<RiskManager>(universe_, news_);
// использует RiskManager::Config{} — всё hardcoded
```

`RiskManager::Config` содержит 20+ параметров: `max_daily_loss_pct`, `max_concurrent_positions`, `max_per_trade_risk_pct`, `max_leverage`, `news_blackout_min` и т.д. Все они имеют дефолтные значения в структуре и **никогда** не читаются из `config.toml`. Оператор не может изменить риск-параметры без перекомпиляции.

**Фикс:**

```cpp
RiskManager::Config rm_cfg;
rm_cfg.max_daily_loss_pct     = Config::get_or<double>("risk.max_daily_loss_pct", 3.0);
rm_cfg.max_concurrent_positions = Config::get_or<int64_t>("risk.max_concurrent_positions", 3);
rm_cfg.max_per_trade_risk_pct = Config::get_or<double>("risk.max_per_trade_risk_pct", 0.5);
// ... остальные поля
risk_manager_ = std::make_unique<RiskManager>(universe_, news_, rm_cfg);
```

---

### [C-4] R10 (rate-limit) считает закрытия сделок, а не их открытия

**Файл:** `src/risk/RiskManager.cpp:255–260`

```cpp
void RiskManager::record_trade_end(bool is_loss,
                                   std::chrono::system_clock::time_point ts) {
    std::lock_guard lock(mtx_);
    trade_history_.push_back(ts);   // ← добавляется при ЗАКРЫТИИ
    ...
}
```

В `evaluate()` проверяется:

```cpp
if (trade_history_.size() >= static_cast<size_t>(cfg_.max_trades_per_window)) {
    d.reason = RejectReason::TradeRateLimitHit;
```

Это означает: пока ни одна сделка не закрылась, можно открыть сколько угодно новых (R3 ограничивает лишь до `max_concurrent_positions`, но не темп открытий). При `max_concurrent_positions=3` и быстрых открытиях/закрытиях R10 срабатывает только постфактум.

Если замысел — ограничить частоту **подачи** ордеров, нужно добавлять в `trade_history_` при `evaluate(..., accepted)`.

---

## ВЫСОКИЕ

---

### [H-1] Order-matching по размеру — риск ложного захвата при дублирующихся сделках

**Файл:** `src/executor/LiveExecutor.cpp:135–168`

```cpp
constexpr double kSizeEps = 1e-6;
const bool size_close = std::abs(trade.plan.size_coin - upd.size) < kSizeEps;

const bool fresh_entry =
    !match_by_entry && !match_by_stop && !match_by_tp1 && !match_by_tp2 &&
    trade.entry_order_id == 0 && is_entry_type && side_eq && size_close;
```

Два сценария поломки:

1. **Два ордера одного тикера с одинаковым размером** (возможно при нескольких стратегиях или повторных входах). Первый входящий WS-update захватит первую незакрепившуюся запись, независимо от того, к какому ордеру он реально относится.

2. **Биржа может вернуть округлённый size** (по `size_increment`). Если `plan.size_coin = 0.0031415` а биржа вернёт `0.003`, разница `1.415e-4 > 1e-6` → fresh_entry = false → ордер навсегда остаётся в `entry_order_id == 0`, stops никогда не будут размещены.

`kSizeEps` должен быть относительным: `std::abs(a - b) < std::max(a, b) * 1e-4`.

Долгосрочное решение: передавать `client_order_id` при размещении и матчить по нему. `PlaceOrderResult.client_id` уже возвращается биржей, `RestOrder.client_id` — тоже optional есть. Поле просто не используется в `LiveExecutor`.

---

### [H-2] StartupRecovery размещает emergency-stop с `OrderType::Stop`, а не `StopLoss`

**Файл:** `src/executor/StartupRecovery.cpp:71`

```cpp
req.type = OrderType::Stop;   // ← должен быть StopLoss
```

Во всём остальном коде (`place_stops_`, `LiveExecutor`) для защитного ордера используется `OrderType::StopLoss`. `OrderType::Stop` — это тип для нотификационных стоп-ордеров без гарантии исполнения в MetaScalp-протоколе. При рестарте с открытой позицией emergency-стоп может быть размещён как неверный тип и не сработает при пробое цены.

---

### [H-3] schema_version не проверяется при загрузке — тихая деградация данных

**Файл:** `src/risk/AccountStatePersister.cpp:47–66`

```cpp
j["schema_version"] = 1;   // сохраняем
...
// при загрузке:
data.last_reset_day_utc = j.value("last_reset_day_utc", "");
// schema_version вообще не проверяется
```

При изменении формата JSON (новые/переименованные поля) старый файл загрузится без ошибки: новые поля примут дефолтные значения (`j.value(key, default)`), потенциально сбросив equity или kill_switch_triggered в 0/false.

**Фикс:**

```cpp
int ver = j.value("schema_version", 0);
if (ver != 1) {
    LOG_ERROR("Unsupported schema_version={}, refusing to load", ver);
    return std::nullopt;
}
```

---

### [H-4] ClosedTrade.reason неверен для второй части позиции после TP1

**Файл:** `src/executor/LiveExecutor.cpp:240–253`

```cpp
} else if (is_stop || is_tp2 || (is_tp1 && trade.tp1_filled)) {
    trade.state = TradeState::Closed;
    ...
    ct.reason = is_stop ? FixedString<32>("StopLoss")
              : (is_tp2  ? FixedString<32>("TP2")
                         : FixedString<32>("TP1"));   // ← но это не первый TP1-fill!
```

Когда `is_tp1 && trade.tp1_filled` — это второй TP1-fill (или случай полного закрытия через TP1 после уже частичного fill). `is_tp2 = false`, `is_stop = false` → reason записывается как `"TP1"`. В журнале это будет выглядеть как два закрытия через `TP1` у одной позиции. PnL-атрибуция в `TradeJournal` и RiskManager-steak-tracking получат неверный источник.

Нужен либо отдельный reason `"TP1_Remainder"`, либо проверка `tp1_filled` при формировании причины.

---

## СРЕДНИЕ

---

### [M-1] `update_extremes_` — мёртвый код в дублирующемся duplicate-check

**Файл:** `src/signals/LevelDetector.cpp:52–81`

```cpp
bool found = false;
for (size_t k = 0; k < extremes_.size(); ++k) {  // полный скан
    if (extremes_[k].ts == ts) { found = true; break; }
}
if (!found) {
    bool already_have = false;
    for (int k = ...; k >= extremes_.size() - 10; --k) {  // скан последних 10
        if (extremes_[k].ts == ts) { already_have = true; break; }
    }
    if (!already_have) { extremes_.push_back(...); }
}
```

Если `found == false` (полный скан не нашёл ts), то `already_have` в частичном скане тем более будет false — он просматривает подмножество. Второй цикл — мёртвый код, оставшийся после рефакторинга. Убирается без последствий.

---

### [M-2] PaperExecutor не учитывает fees и funding — результаты систематически оптимистичны

**Файл:** `src/executor/PaperExecutor.cpp`

Slippage моделируется (bps), но комиссия биржи (`~0.04%` maker/taker на futures) и funding-rate (`~0.01% / 8h`) игнорируются полностью. Для скальпинговой стратегии с коротким holding-time fees составляют значительную долю PnL. Paper-результаты будут систематически завышены относительно live.

Минимально нужно добавить `cfg_.taker_fee_bps` и вычитать `fee_usd = fill_price * size * fee_bps / 10000.0` из `pnl`.

---

### [M-3] `books_for_executor_` передаётся в PaperExecutor по ссылке до заполнения

**Файл:** `src/main.cpp:213–216`

```cpp
executor_ = std::make_unique<PaperExecutor>(books_for_executor_);
// books_for_executor_ — пуст в момент конструирования
```

`PaperExecutor` хранит `const std::map<Ticker, const OrderBook*>&` — динамически. Книги добавляются позже в affinity-хэндлере. Это корректно работает, если первый `tick()` приходит после инициализации universe. Но если таймер успеет сработать до первого `refresh_affinity()` (теоретически возможно при высоком load на старте), `tick()` не найдёт книги и молча пропустит все pending entries. Нет защитной проверки.

---

## НИЗКИЕ / СТИЛЬ

---

### [L-1] Нормализация ticker-name (`BTC_USDT` ↔ `BTCUSDT`) дублируется в двух местах

**Файл:** `src/main.cpp:273–276, 428–432`

Одна и та же логика разбора leader ticker вынесена дважды. Нужна утилита в `src/utils/` или `TickerUniverse`.

---

### [L-2] `authorize()` — длина токена утекает через timing

**Файл:** `src/control/DashboardServer.cpp:818–831`

```cpp
if (presented.size() != auth_token_.size()) return false;  // ← early-exit по длине
unsigned diff = 0;
for (...) diff |= ...;    // constant-time XOR
```

XOR-сравнение корректно защищает содержимое, но early-exit по `size()` позволяет аттакующему определить длину токена через timing. Для loopback-дашборда это нецелевая угроза, но если порт 8080 когда-нибудь откроется, это станет актуальным. Решение: всегда выполнять XOR на `min(size)` байт.

---

### [L-3] `PlaceOrderRequest.client_id` не используется — матчинг ордеров ненадёжен

Как указано в [H-1]: поле `client_id` присутствует в `RestOrder` и `PlaceOrderResult`, но не генерируется при `place_order()`. Если биржа возвращает `client_id`, это готовый детерминированный ключ для матчинга — надёжнее любого эвристического size-matching.

---

## Итоговая матрица рисков

| ID  | Severity | Модуль              | Суть                                       | Дефолтное поведение         |
|-----|----------|---------------------|--------------------------------------------|-----------------------------|
| C-1 | CRITICAL | main / persister    | active_trades не сохраняются               | Recovery требует ручного ACK всегда |
| C-2 | CRITICAL | LiveExecutor        | Провал SL placement без алерта             | Открытая позиция без стопа  |
| C-3 | CRITICAL | main / RiskManager  | Risk-параметры не читаются из конфига      | Все параметры — дефолты     |
| C-4 | CRITICAL | RiskManager         | R10 считает закрытия, не открытия          | Rate-limit не защищает вход |
| H-1 | HIGH     | LiveExecutor        | Order-matching по абсолютному size         | Неверный ордер захватывает ID |
| H-2 | HIGH     | StartupRecovery     | OrderType::Stop вместо StopLoss            | Emergency-стоп может не сработать |
| H-3 | HIGH     | AccountStatePersister | schema_version не проверяется            | Тихая потеря данных при обновлении |
| H-4 | HIGH     | LiveExecutor        | ClosedTrade.reason неверен для TP1-остатка | Неверная PnL-атрибуция в журнале |
| M-1 | MEDIUM   | LevelDetector       | Мёртвый код в duplicate-check             | Лишние итерации             |
| M-2 | MEDIUM   | PaperExecutor       | Нет fees/funding в PnL                    | Оптимистичные paper-результаты |
| M-3 | MEDIUM   | main / PaperExecutor | books_for_executor_ пуст при старте       | Молчаливый промах tick      |
| L-1 | LOW      | main                | Дублирование нормализации ticker           | Стиль                       |
| L-2 | LOW      | DashboardServer     | Token length утекает через timing          | Незначительно при loopback  |
| L-3 | LOW      | LiveExecutor        | client_id не используется                 | Надёжность матчинга         |

---

## Приоритет устранения

1. **[C-2]** — SL-провал без алерта. Риск реальных денег, один `if (alert_cb_)` на fix.
2. **[C-1]** — активные сделки не сохраняются. Один вызов `get_active_trades()` на fix.
3. **[C-3]** — risk-параметры из конфига. Без этого конфиг вообще бессмысленен.
4. **[H-2]** — OrderType::Stop → StopLoss. Одна строка.
5. **[C-4] + [H-1]** — требуют рефакторинга логики.
