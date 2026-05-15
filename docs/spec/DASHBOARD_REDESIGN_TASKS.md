# Dashboard Redesign — Task Breakdown

> **Date:** 2026-05-11 (revised after audit)  
> **Design System:** [DASHBOARD_DESIGN_SYSTEM.md](./DASHBOARD_DESIGN_SYSTEM.md)  
> **Total tasks:** 30  
> **Phases:** 5

---

## Диаграмма зависимостей

```
Phase 1 ──── Базовые структуры данных ─────────────────────────────┐
  DS-01, DS-02, DS-03  (независимы, параллельны)                   │
    │                                                               │
Phase 2 ──── get_state() в стратегиях ─────────────────────────────┤
  DS-04 (LeaderLag)                                                 │
  DS-05 (BreakoutEatThrough)    ← зависят от DS-01                 │
  DS-06 (BounceFromDensity)     (параллельны между собой)           │
    │                                                               │
Phase 3 ──── Engine + Интеграция ───────────────────────────────────┤
  DS-07 (StrategyEngine)       ← зависит от DS-01, DS-04..06       │
  DS-08 (ChartHistory буфер)   ← зависит от DS-02                  │
  DS-09 (Ob via TickerCtrl)    ← зависит от DS-03                  │
  DS-10 (main.cpp интеграция)  ← зависит от DS-07,08,09            │
  DS-10.5 (async dash strand)  ← зависит от DS-10                  │
  DS-11 (JSON сериализация)    ← зависит от DS-03, DS-10           │
  DS-11.5 (cmake embed HTML)   ← не зависит, можно раньше          │
    │                                                               │
Phase 4 ──── Фронтенд (ВСЁ ПОСЛЕДОВАТЕЛЬНО в одном файле) ────────┘
  DS-12 (HTML: структура 8 вкладок)  ← зависит от DS-11, DS-11.5
  DS-13 (CSS: дизайн-система v2)     ← зависит от DS-12
  DS-14 (Command tab: heatmap)       ← зависит от DS-13
  DS-15 (Charts tab)                 ← зависит от DS-13
  DS-16 (Order Book tab)             ← зависит от DS-13
  DS-17 (Positions tab)              ← зависит от DS-13
  DS-18 (Signals tab)                ← зависит от DS-13
  DS-19 (Universe tab)               ← зависит от DS-13
  DS-20 (Journal tab)                ← зависит от DS-13
  DS-21 (Controls tab)               ← зависит от DS-13
  DS-22 (JS: WebSocket + рендереры)  ← зависит от DS-14..21
  DS-23 (JS: ticker selector API)    ← зависит от DS-22
    │
Phase 5 ──── Валидация ────────────────────────────────────────────┘
  DS-24 (Typecheck: cmake build)
  DS-25 (Unit tests)
  DS-26 (Dashboard unit test)     ← зависит от DS-11 (НЕ от DS-23!)
  DS-27 (Code review)
```

---

## PRE-FLIGHT: Git tag (перед Phase 4)

```bash
git tag dashboard-v1-backup -m "Backup before dashboard redesign Phase 4"
```
**Критерий:** Возможность отката `git checkout dashboard-v1-backup -- core/src/control/DashboardServer.cpp`

---

## Phase 1: Базовые структуры данных (backend)

### DS-01 — StrategyState, StrategyCondition, StrategyReadyState
**Файлы:** `core/src/strategy/IStrategy.hpp`  
**Описание:** Добавить enum `StrategyReadyState`, struct `StrategyCondition`, struct `StrategyState`. Добавить pure virtual метод `get_state()` в `IStrategy`.  
**Документировать:** `readiness_pct = met_conditions / total_conditions * 100` (все условия равновесны). Для Planning/Trading/Cooldown → readiness_pct = 100.  
**Зависит от:** —  
**Блокирует:** DS-04, DS-05, DS-06, DS-07  
**Критерий:** Компилируется, все стратегии обязаны реализовать `get_state()`

### DS-02 — ChartPoint + ChartHistory (кольцевой буфер)
**Файлы:** `core/src/features/ChartHistory.hpp` (новый)  
**Описание:** struct `ChartPoint` (облегчённая версия FeatureFrame). Класс `ChartHistory` — кольцевой буфер на 300 точек: `push(FeatureFrame)`, `snapshot() → vector<ChartPoint>`.  
**Зависит от:** —  
**Блокирует:** DS-08, DS-10  
**Критерий:** Юнит-тест: добавить 400 точек → snapshot().size() == 300 (последние)

### DS-03 — ObLevel + OrderBook::get_top_levels(N)
**Файлы:** `core/src/marketdata/OrderBook.hpp`, `core/src/marketdata/OrderBook.cpp`  
**Описание:** struct `ObLevel { double price, size; }`. Метод `get_top_levels(int n)` → `{vector<ObLevel> bids, vector<ObLevel> asks}`. Bids — топ-N по убыванию цены, Asks — топ-N по возрастанию.  
**Зависит от:** —  
**Блокирует:** DS-09, DS-10, DS-11  
**Критерий:** `orderbook_test.cpp` расширен тестом `get_top_levels(20)`

---

## Phase 2: get_state() в стратегиях

### DS-04 — LeaderLag::get_state()
**Файлы:** `core/src/strategy/LeaderLag.hpp`, `core/src/strategy/LeaderLag.cpp`  
**Описание:** Реализовать `get_state()`. Условия: CUSUM warmup (через LeaderTracker, если доступен), LeaderMove availability, correlation threshold, spread, our_change_2s, density_on_path.  
**Зависит от:** DS-01  
**Блокирует:** DS-07  
**Критерий:** `leaderlag_strategy_test.cpp` проверяет `get_state()` в warming/ready

### DS-05 — BreakoutEatThrough::get_state()
**Файлы:** `core/src/strategy/BreakoutEatThrough.hpp`, `core/src/strategy/BreakoutEatThrough.cpp`  
**Описание:** Реализовать `get_state()`. Условия: DensityEating availability, eating progress (ratio), TapeBurst, tape aggression, relative volume, leader alignment, resistance clusters, support presence, density-to-best distance.  
**Зависит от:** DS-01  
**Блокирует:** DS-07  
**Критерий:** `breakout_strategy_test.cpp` проверяет `get_state()`

### DS-06 — BounceFromDensity::get_state()
**Файлы:** `core/src/strategy/BounceFromDensity.hpp`, `core/src/strategy/BounceFromDensity.cpp`  
**Описание:** Реализовать `get_state()`. Условия: LevelApproach availability, approach speed, density at level, relative density, tape speed ratio, leader alignment, TapeFade, LeaderMove contra, iceberg absence.  
**Зависит от:** DS-01  
**Блокирует:** DS-07  
**Критерий:** `bounce_strategy_test.cpp` проверяет `get_state()`

---

## Phase 3: Engine + Интеграция

### DS-07 — StrategyEngine::get_all_states()
**Файлы:** `core/src/strategy/StrategyEngine.hpp`, `core/src/strategy/StrategyEngine.cpp`  
**Описание:** Метод `get_all_states() const → vector<StrategyState>`. Обходит все стратегии (ticker_strategies_ + global_strategies_), вызывает `get_state()`. Для `Planning`/`Trading`/`Cooldown` — проверяет `active_plan_` и текущие позиции (через executor callback).  
**Зависит от:** DS-01, DS-04, DS-05, DS-06  
**Блокирует:** DS-10  
**Критерий:** `strategy_engine_test.cpp` с мок-стратегией

### DS-08 — ChartHistory в TickerController
**Файлы:** `core/src/control/TickerController.hpp`, `core/src/control/TickerController.cpp`  
**Описание:** Член `ChartHistory chart_history_`. В `tick()` → `chart_history_.push(last_frame_)`. Метод `chart_snapshot() const → vector<ChartPoint>`.  
**Зависит от:** DS-02  
**Блокирует:** DS-10  
**Критерий:** После 400 тиков `chart_snapshot().size() == 300`

### DS-09 — TickerController::ob_snapshot()
**Файлы:** `core/src/control/TickerController.hpp`, `core/src/control/TickerController.cpp`  
**Описание:** Метод `ob_snapshot(int n_levels) → pair<vector<ObLevel>,vector<ObLevel>>` — делегирует в `book->get_top_levels(n_levels)`.  
**Зависит от:** DS-03  
**Блокирует:** DS-10  
**Критерий:** Вызов из main.cpp возвращает топ 20 bids/asks

### DS-10 — main.cpp: сбор dashboard-данных (структура, без async)
**Файлы:** `core/src/main.cpp`  
**Описание:** В BotApp добавить метод `collect_dashboard_state()`:
1. `engine.get_all_states()` → `dash_state.strategy_states`
2. Для `selected_ticker`: `chart_snapshot()` → `dash_state.chart_history`
3. Для `selected_ticker`: `ob_snapshot(20)` → `dash_state.bids_top20`, `asks_top20`, `ob_mid`, `ob_spread_bps`, `ob_imbalance`
4. `dash_state.selected_ticker`
**Важно:** Пока вызывается синхронно (будет вынесено в strand в DS-10.5).  
**Зависит от:** DS-07, DS-08, DS-09  
**Блокирует:** DS-10.5, DS-11  
**Критерий:** `collect_dashboard_state()` заполняет все новые поля

### DS-10.5 — Async Dashboard Strand
**Файлы:** `core/src/main.cpp`  
**Описание:** Вынести вызов `collect_dashboard_state()` + `dashboard_.update_state()` в отдельную `boost::asio::strand` с таймером 250ms:
- Trading loop не блокируется сбором dashboard-данных
- Shared state (FeatureFrame, OrderBook) читается под reader-lock
- JSON-сериализация происходит в strand, не в main loop
**Зависит от:** DS-10  
**Блокирует:** —  
**Критерий:** `collect_dashboard_state()` не вызывается из `tick()` напрямую, latency trading loop не меняется

### DS-11 — JSON сериализация новых полей
**Файлы:** `core/src/control/DashboardServer.cpp` (метод `serialize_state_locked_()`)  
**Описание:** Сериализовать `strategy_states`, `chart_history`, `bids_top20`, `asks_top20`, `ob_mid`, `ob_spread_bps`, `ob_imbalance`, `selected_ticker` в JSON. Thread-safety: `selected_ticker_` защищён `mutex_` (тот же, что и `current_state_`).  
**Зависит от:** DS-03, DS-10  
**Блокирует:** DS-12, DS-26  
**Критерий:** `serialize_state()` возвращает валидный JSON со всеми новыми полями, `dashboard_server_test` обновлён

### DS-11.5 — cmake embed dashboard.html
**Файлы:** `core/CMakeLists.txt`, `core/src/control/dashboard.html` (новый), `core/src/control/DashboardServer.cpp`  
**Описание:**
1. Вырезать HTML/CSS/JS из `kDashboardHtml` в `core/src/control/dashboard.html`
2. В `core/CMakeLists.txt`: `add_custom_command` с `xxd -i dashboard.html > dashboard_html.h`
3. В `DashboardServer.cpp`: `#include "dashboard_html.h"`, заменить `kDashboardHtml` на `reinterpret_cast<const char*>(dashboard_html)`
**Зависит от:** — (можно делать в любой момент)  
**Блокирует:** DS-12  
**Критерий:** `dashboard.html` редактируется как обычный файл с подсветкой, cmake генерирует заголовок, билд успешен

---

## Phase 4: Фронтенд

**ВАЖНО:** Все задачи Phase 4 — ПОСЛЕДОВАТЕЛЬНЫЕ. Они работают в одном файле `core/src/control/dashboard.html` и не могут быть распараллелены без merge-конфликтов. Порядок: DS-12 → DS-13 → DS-14..21 (последовательно) → DS-22 → DS-23.

### DS-12 — HTML: структура 8 вкладок
**Файлы:** `core/src/control/dashboard.html`  
**Описание:** Базовая HTML-структура: header, tab-bar (8 кнопок), 8 tab-content панелей с placeholder-контентом.  
**Зависит от:** DS-11, DS-11.5  
**Блокирует:** DS-13  
**Критерий:** Дашборд открывается, 8 вкладок переключаются

### DS-13 — CSS: дизайн-система v2
**Файлы:** `core/src/control/dashboard.html` (секция `<style>`)  
**Описание:** Все новые CSS-классы:
- `.hm-cell` — 6 состояний (`st-cold/warming/ready/planning/trading/cooldown`)
- `.detail-panel` — раскрывающаяся панель (max-height animation)
- `.prog-ring` — мини SVG-кольцо (20×20px)
- `.condition-row` — строка условия
- `.ob-ladder`, `.ob-bar` — стакан
- `.ticker-select` — селектор тикера
- `.heatmap-grid` — грид с max-height и overflow-y: auto
- Все гриды для новых вкладок
**Зависит от:** DS-12  
**Блокирует:** DS-14..DS-21  
**Критерий:** Все 6 состояний heatmap отображаются в статике

### DS-14 — JS: Command Tab (Strategy Heatmap + Detail Panel)
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderCommand(state)`:
- Heatmap grid: строки = тикеры, колонки = стратегии (scroll, max 10 rows visible)
- Фильтр-баттоны: ALL / Ready only / Trading only
- Цвет ячеек по `ready_state` с pulse-анимацией для transitioning
- Клик по ячейке → detail-panel с `conditions` (progress-ring + current/target)
- `last_reject_reason` и `seconds_since_last_reject`
- KPI bar + risk gauges + equity chart (портировать из старого кода)
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** Heatmap показывает реальные состояния, detail раскрывается, фильтры работают

### DS-15 — JS: Charts Tab
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderCharts(state)`:
- Ticker selector → `selectTicker()`
- Timeframe buttons (1m/5m/15m) — фильтрация на фронте
- PriceChart (Canvas): mid line + volatility band + signal markers (точки)
- Volume bars (Canvas): buy=green, sell=red, stacked
- 4× MiniChart (Canvas): spread, volatility, tape aggression, leader correlation
- requestAnimationFrame для всех Canvas
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** Все графики рисуются, таймфреймы переключаются

### DS-16 — JS: Order Book Tab
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderOrderBook(state)`:
- DepthChart (Canvas): кумулятивные bids (зелёный) + asks (красный) + mid (белая линия)
- Ladder (DOM-таблица): price, size, bar (CSS width), cumulative sum
- Цветовое кодирование строк: bids зелёные, asks красные
- Spread + imbalance индикаторы в футере
- Smooth transition: `transition: width 0.25s ease` на барах
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** Стакан рисуется, бары анимируются, depth chart корректен

### DS-17 — JS: Positions Tab
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderPositions(state)` — улучшения:
- Раскрывающийся detail по клику: время входа, размер, риск, reason
- SL→Entry→TP progress-bar с маркерами (entry=белый, текущая цена=accent)
- PnL с градиентом: зелёный→жёлтый→красный в зависимости от % до SL
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** Детали раскрываются, прогресс-бары с маркерами

### DS-18 — JS: Signals Tab
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderSignals(state)` — улучшения:
- Badge "NEW" на непросмотренных сигналах (сброс при открытии вкладки)
- Группировка по тикеру (коллапсируемые секции)
- Фильтры-чипы (уже есть, портировать)
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** Сигналы группируются, badge работает

### DS-19 — JS: Universe Tab
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderUniverse(state)` — улучшения:
- Мини-точки состояния стратегий (8×8px, цвет по ready_state) в колонке Strategies
- Фильтр: All / Ready / Trading
- Клик по тикеру → `selectTicker(ticker)` для Charts/OrderBook
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** Точки состояния отображаются, клик выбирает тикер, фильтр работает

### DS-20 — JS: Journal Tab
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderJournal(state)` — улучшения:
- Cumulative PnL chart (Canvas, как equity chart но другие данные)
- Фильтр по стратегии (выпадающий список)
- CSV export (портировать)
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** PnL график отображается, фильтр по стратегии работает

### DS-21 — JS: Controls Tab
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** `renderControls(state)` — улучшения:
- Статус MetaScalp: connected/latency
- Execution mode (live/paper)
- Recorder controls (портировать)
- Log feed (портировать)
- Version info
**Зависит от:** DS-13  
**Блокирует:** DS-22  
**Критерий:** Статусы подключений отображаются, recorder работает

### DS-22 — JS: WebSocket + update() + общий рендерер
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`)  
**Описание:** Главная функция `update(data)`:
- Парсить все новые JSON-поля
- Вызывать все 8 render-функций
- `computeMetrics()`, `detectDiffs()` (портировать)
- WebSocket: `connect()`, reconnect 2s, индикатор
- Обработка ошибок парсинга (try/catch, console.error)
**Зависит от:** DS-14..DS-21  
**Блокирует:** DS-23  
**Критерий:** WS-сообщения парсятся, все 8 вкладок рендерятся без JS-ошибок

### DS-23 — JS: Ticker Selector API
**Файлы:** `core/src/control/dashboard.html` (секция `<script>`), `core/src/control/DashboardServer.cpp`  
**Описание:**
- Фронт: `selectTicker(ticker)` → HTTP POST `/api/ticker/select` с JSON `{"ticker":"BTC_USDT"}`
- Бэк: handler читает `ticker`, захватывает `mutex_`, пишет в `current_state_.selected_ticker`
- Thread-safety: `selected_ticker` под `mutex_` (документировано в DS-11)
**Зависит от:** DS-22  
**Блокирует:** —  
**Критерий:** Смена тикера на фронте → Charts и Order Book показывают новый тикер

---

## Phase 5: Валидация

### DS-24 — Typecheck (cmake build)
**Команда:** `cmake --build build`  
**Описание:** Полный билд. 0 ошибок, 0 warnings.  
**Зависит от:** DS-11, DS-23  
**Критерий:** Билд успешен

### DS-25 — Unit tests
**Команда:** `ctest --test-dir build`  
**Описание:** Все существующие тесты + новые (DS-02, DS-03, DS-04, DS-05, DS-06, DS-07, DS-09).  
**Зависит от:** DS-24  
**Критерий:** Все тесты зеленые

### DS-26 — Dashboard server unit test
**Файлы:** `core/test/unit/dashboard_server_test.cpp`  
**Описание:** Проверить JSON-сериализацию новых полей: `strategy_states`, `chart_history`, `bids_top20`, `asks_top20`.  
**Зависит от:** DS-11 (НЕ от DS-23! Можно запускать параллельно с DS-12..DS-23)  
**Критерий:** Тест проверяет наличие всех новых ключей в JSON

### DS-27 — Code review
**Описание:** Полный review через code-reviewer-deepseek (все изменённые файлы).  
**Зависит от:** DS-24, DS-25, DS-26  
**Критерий:** Нет критических замечаний

---

## Сводка по фазам

| Фаза | Задачи | Зависимость | Оценка (строк кода) |
|------|--------|-------------|---------------------|
| Phase 1 | DS-01..03 | — | ~150 C++ |
| Phase 2 | DS-04..06 | Phase 1 | ~250 C++ |
| Phase 3 | DS-07..11.5 | Phase 1+2 | ~300 C++ + 50 cmake |
| Phase 4 | DS-12..23 | Phase 3 | ~4000-5000 HTML/CSS/JS |
| Phase 5 | DS-24..27 | Phase 3+4 | ~100 C++ (тесты) |
| **Total** | **30 задач** | | **~4500-5500 строк** |

> Оценка строк реалистичная, с учётом 6 Canvas-компонентов (~1000 строк), 8 render-функций (~1500 строк), CSS (~500 строк), HTML-структуры (~200 строк), JS-утилит (~500 строк), WebSocket + state (~500 строк).

---

## Порядок выполнения (critical path)

```
DS-01,02,03 ──▶ DS-04,05,06 ──▶ DS-07 ──▶ DS-10 ──▶ DS-10.5 ──▶ DS-11
                                 DS-08 ──┘                    │
                                 DS-09 ──┘                    │
                                                    DS-11.5 ──┘
                                                         │
                                                    DS-12 → DS-13 → DS-14..21 (последовательно) → DS-22 → DS-23
                                                         │
                                                    DS-24 → DS-25,26 → DS-27
```

**Ключевые моменты:**
- DS-01,02,03 — параллельно (независимые файлы)
- DS-04,05,06 — параллельно (разные файлы)
- DS-07,08,09 — параллельно (разные компоненты)
- DS-11.5 — можно сделать в любой момент до DS-12
- DS-14..21 — СТРОГО ПОСЛЕДОВАТЕЛЬНО (один файл `dashboard.html`)
- DS-24 и DS-26 — параллельно (разные аспекты)
- **Rollback:** `git tag dashboard-v1-backup` перед Phase 4
