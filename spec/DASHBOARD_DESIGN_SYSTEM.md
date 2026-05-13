# Dashboard Design System v2.0

> **Status:** Draft — согласован с пользователем  
> **Date:** 2026-05-11 (revised after audit)  
> **Scope:** Полная переработка дашборда + бэкенд для отображения состояний стратегий, графиков и стакана

---

## 1. Принципы дизайна

| Принцип | Описание |
|---------|----------|
| **Проактивность** | Показываем не только что случилось, но и *почему* не случается (разогрев, блокировки) |
| **Сканируемость** | Heatmap стратегий даёт мгновенный обзор всей системы одним взглядом |
| **Адаптивная глубина** | Быстрый обзор → клик → детальная панель (drill-down) |
| **Реалтайм** | Все данные обновляются через WebSocket, анимации сглаживают переходы |
| **Профессиональная эстетика** | Тёмная тема, стекло (backdrop-blur), градиенты, микро-анимации |
| **Low latency impact** | Сбор dashboard-данных не замедляет trading loop — вынесен в отдельную strand |

---

## 2. Архитектура (Key Design Decisions)

### 2.1 Embed HTML как внешний файл

**Проблема:** ~4500 строк HTML/CSS/JS внутри C++ string-константы — нет подсветки, нет hot-reload, escape-символы, мучительный DX.

**Решение:** HTML вынесен в `src/control/dashboard.html`. На этапе сборки cmake вызывает `xxd -i` и генерирует `dashboard_html.h` с `const unsigned char dashboard_html[]`. В `DashboardServer.cpp` только `#include "dashboard_html.h"` и использование.

**Плюсы:**
- Полноценная подсветка синтаксиса в редакторе
- Prettier/ESLint на HTML/CSS/JS независимо от C++
- Можно открыть `dashboard.html` в браузере для отладки (с мок-данными)
- Нет риска сломать C++ компиляцию через escape-последовательности

### 2.2 Async Dashboard Data Collection

**Проблема:** `engine.get_all_states()` + `chart_snapshot()` + `ob_snapshot(20)` в главном trading loop после каждого `tick()` (100ms) добавляют latency в критический путь.

**Решение:** Dashboard-данные собираются в отдельной `boost::asio::strand` с интервалом 250ms (не каждый тик), асинхронно от trading loop:

```
main loop (100ms)                    dashboard strand (250ms)
─────────────────                    ─────────────────────
tick_all_tickers()                   
  └─ on_frame/on_signal/on_trade    
  └─ strategy.tick()                
  └─ executor.process()             
                                     collect_dashboard_state()
                                       └─ engine.get_all_states()
                                       └─ chart_snapshot()
                                       └─ ob_snapshot(20)
                                       └─ dashboard_.update_state()
```

**Детали:**
- Trading loop только обновляет shared state (атомарно) — не копирует данные
- Dashboard strand читает shared state под reader-lock (shared_mutex)
- Сериализация JSON — самая тяжёлая операция — делается в strand, не в main loop
- Interval 250ms выбран как компромисс: достаточно часто для UI, не мешает trading

### 2.3 Thread Safety — selected_ticker

**Проблема:** HTTP POST `/api/ticker/select` изменяет `selected_ticker_` в `DashboardServer`, который одновременно читается при `serialize_state_locked_()`.

**Решение:** `selected_ticker_` защищён тем же `mutex_`, что и весь `current_state_`. Метод `serialize_state_locked_()` уже требует захвата `mutex_`. HTTP handler для `/api/ticker/select` захватывает `mutex_` перед записью. Race condition невозможен при правильном использовании `mutex_`.

---

## 3. Визуальный язык (Design Tokens)

### 3.1 Цветовая палитра

```
┌─ Backgrounds ─────────────────────────────────────┐
│  --bg:         #03050a    deepest bg              │
│  --bg2:        #060912    elevated bg             │
│  --surface:    #0a0e1a    card surface            │
│  --surface2:   #0f1525    card inner surface      │
│  --surface-glass: rgba(10,15,28,0.75)  glass card │
│  --surface-raised: #131b2e  hovered card          │
├─ Borders ─────────────────────────────────────────┤
│  --border:     rgba(255,255,255,0.06)             │
│  --border-bright: rgba(255,255,255,0.1)           │
├─ Text ────────────────────────────────────────────┤
│  --text:       #e8edf5    primary                 │
│  --text-dim:   #c0c7d4    secondary               │
│  --muted:      #7b859c    labels                  │
│  --subtle:     #424b5e    disabled/hints          │
├─ Accent ──────────────────────────────────────────┤
│  --accent:     #6366f1    indigo (primary action) │
│  --accent2:    #06b6d4    cyan (info)             │
│  --accent3:    #8b5cf6    violet (highlight)      │
├─ Semantic ────────────────────────────────────────┤
│  --positive:   #10b981    green                   │
│  --positive-bright: #34d399                      │
│  --negative:   #ef4444    red                     │
│  --negative-bright: #f87171                      │
│  --warning:    #f59e0b    amber                   │
│  --warning-bright: #fbbf24                       │
│  --info:       #3b82f6    blue                    │
└───────────────────────────────────────────────────┘
```

### 3.2 Типографика

- **Primary font:** `'Inter', system-ui, sans-serif` (Google Fonts)
- **Monospace font:** `'JetBrains Mono', 'Cascadia Code', monospace`
- **Scale:** 7px → 8px → 9px → 10px → 11.5px → 13px → 15px → 18px → 22px
- **Weights:** 400, 500, 600, 700, 800, 900

### 3.3 Радиусы и тени

- `--radius: 10px`, `--radius-sm: 6px`, `--radius-lg: 14px`
- `--glow-pos: 0 0 12px rgba(16,185,129,0.25)`
- `--glow-neg: 0 0 12px rgba(239,68,68,0.25)`
- `--glow-accent: 0 0 16px rgba(99,102,241,0.3)`

### 3.4 Анимации

- `--transition: 0.2s cubic-bezier(0.4,0,0.2,1)`
- Все переходы состояний анимированы
- Pulse-анимация для live-индикаторов
- Slide-in для тостов и раскрывающихся панелей

---

## 4. Состояния стратегий (Strategy Lifecycle)

### 4.1 Цветовое кодирование

| Состояние | CSS класс | Цвет | Иконка | Значение |
|-----------|-----------|------|--------|----------|
| **Cold** | `st-cold` | `#424b5e` (subtle) | 🔵 | Только создана, нет данных |
| **Warming** | `st-warming` | `#f59e0b` (amber) | 🟡 | Копит сигналы/статистику |
| **Ready** | `st-ready` | `#10b981` (green) | 🟢 | Все условия выполнены |
| **Planning** | `st-planning` | `#8b5cf6` (violet) | 🟣 | TradePlan отправлен риск-менеджеру |
| **Trading** | `st-trading` | `#3b82f6` (blue) | 🔴 | Активная позиция |
| **Cooldown** | `st-cooldown` | `#6b7280` (dim) | ⚫ | Пауза после выхода |

### 4.2 Переходы состояний

```
Cold ──(первые данные)──▶ Warming ──(все пороги)──▶ Ready
                                                       │
                                                  (сигнал + план)
                                                       ▼
Cooldown ◀──(выход из сделки)── Trading ◀──(исполнение)── Planning
```

### 4.3 Формула readiness_pct

`readiness_pct = (met_conditions / total_conditions) * 100`

Где:
- `met_conditions` = количество `StrategyCondition` с `met == true`
- `total_conditions` = общее количество `StrategyCondition` в векторе

**Важно:** Все условия имеют равный вес. Взвешенная формула (с приоритетами) — потенциальное улучшение в будущем, не в v2.0.

Для состояний `Planning`, `Trading`, `Cooldown` — `readiness_pct = 100` (условия уже были выполнены).

---

## 5. Структура вкладок

| # | ID | Название | Содержание |
|---|-----|----------|------------|
| 1 | `command` | 🎯 Command | Strategy Heatmap + Risk Gauges + KPI Summary |
| 2 | `charts` | 📊 Charts | Price chart + Volume + Spread + Volatility |
| 3 | `orderbook` | 📖 Order Book | Depth Chart + Ladder (bids/asks) |
| 4 | `positions` | 📈 Positions | Active trades + SL/TP progress |
| 5 | `signals` | ⚡ Signals | Signal stream + filters |
| 6 | `universe` | 🌐 Universe | Ticker table + strategy indicators |
| 7 | `journal` | 📋 Journal | Trade history + PnL chart |
| 8 | `controls` | ⚙️ Controls | Recorder, logs, config info |

---

## 6. Макеты вкладок

### 6.1 Command Tab (Главный пульт)

```
┌─ COMMAND ───────────────────────────────────────────────┐
│  ┌─ KPI Bar ─────────────────────────────────────────┐  │
│  │  Equity: $12,450  PnL: +$340  WR: 62%  PF: 1.8   │  │
│  └───────────────────────────────────────────────────┘  │
│                                                          │
│  ┌─ Strategy Heatmap (scrollable, max 10 rows visible)─┐ │
│  │  Filter: [ALL ▾] [Ready only] [Trading only]        │ │
│  │  Ticker ↓ / Strat →   LeaderLag   Breakout  Bounce  │ │
│  │  BTC_USDT              🟢 ready    🟡 warm   🔴 trd │ │
│  │  ETH_USDT              🟡 warm     🔵 cold   🟡 warm│ │
│  │  SOL_USDT              🟢 ready    🟡 warm   🔵 cold│ │
│  │  ...scroll for more tickers...                      │ │
│  │                                                     │ │
│  │  ▸ Click any cell to expand detail panel ↓         │ │
│  └─────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌─ Detail Panel (collapsed by default) ──────────────┐  │
│  │  BTC_USDT / BreakoutEatThrough  🟡 WARMING  4/7    │  │
│  │  ┌─────────────────────────────────────────────┐   │  │
│  │  │ CUSUM:     ████████░░ 24/30     80%  ⏳      │   │  │
│  │  │ Density/h: ██░░░░░░░░  1.2/3.0  40%  ❌      │   │  │
│  │  │ Affinity:  ██████████  0.71/0.55 ✓          │   │  │
│  │  │ Spread:    ██████████  2.1 bps    ✓          │   │  │
│  │  │ Ldr corr:  ██████░░░░  0.52/0.60  ❌         │   │  │
│  │  │ Volume:    ████████░░  1.42/1.50  ❌         │   │  │
│  │  │ ResistCl:  ██████████  0.32/0.80  ✓          │   │  │
│  │  ├─────────────────────────────────────────────┤   │  │
│  │  │ Last reject: Leader corr below 0.60 (12s)   │   │  │
│  │  └─────────────────────────────────────────────┘   │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌─ Gauges ──────────┐  ┌─ Equity Chart ─────────────┐  │
│  │  ⊘ Margin  42%    │  │     equity curve (canvas)   │  │
│  │  ⊘ Exposure 18%   │  │     220px height            │  │
│  │  ⊘ Daily PnL +2%  │  │                              │  │
│  └───────────────────┘  └──────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

**Heatmap scroll behaviour:** 
- Видимы первые 10 тикеров, остальные — скролл
- Фильтр по умолчанию: показывать все (ALL)
- Quick-фильтры: Ready only, Trading only, Warming only
- При 3+ стратегиях колонки фиксированной ширины, горизонтальный скролл

### 6.2 Charts Tab

```
┌─ CHARTS ────────────────────────────────────────────────┐
│  Ticker: [BTC_USDT ▾]   Timeframe: [1m ▾] [5m] [15m]   │
│                                                          │
│  ┌─ Price Chart ──────────────────────────────────────┐  │
│  │  mid (white)  ─ volatility band (translucent)      │  │
│  │       ╱╲         ╱╲                                │  │
│  │      ╱  ╲       ╱  ╲    ╱╲                        │  │
│  │     ╱    ╲     ╱    ╲  ╱  ╲                       │  │
│  │  ──╱──────╲───╱──────╲╱────╲──                    │  │
│  │    ● signal markers on chart                       │  │
│  │    ▎▎▎ ▎ ▎▎▎▎▎ ▎▎ ▎▎▎ volume bars below           │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌─ Buy/Sell Volume ──────────────────────────────────┐  │
│  │  ████████░░ buy    ████░░░░░░ sell                 │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌─ Spread bps ────────┐  ┌─ Volatility 1m bps ──────┐  │
│  │  ──── spread ───    │  │  ──── vol ────           │  │
│  └─────────────────────┘  └───────────────────────────┘  │
│                                                          │
│  ┌─ Tape Aggression ───┐  ┌─ Leader Correlation ──────┐  │
│  │  ──── aggression ── │  │  ──── correlation ───     │  │
│  └─────────────────────┘  └───────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### 6.3 Order Book Tab

```
┌─ ORDER BOOK: BTC_USDT ──────────────────────────────────┐
│                                                          │
│  ┌─ Depth Chart (Canvas) ─────────────────────────────┐  │
│  │  Bids (green, left)  │    │  Asks (red, right)     │  │
│  │       ╱╲              │    │              ╱╲        │  │
│  │      ╱  ╲             │    │             ╱  ╲       │  │
│  │     ╱    ╲            │    │            ╱    ╲      │  │
│  │  ──╱──────╲── mid ────│────│──╱──────╲──────       │  │
│  │  ╱          ╲         │    │ ╱          ╲          │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                          │
│  ┌─ Ladder ────────────────────────────────────────────┐  │
│  │  # │ Price       │ Size     │ Bar        │ Sum      │  │
│  │  ──┼─────────────┼──────────┼────────────┼──────────│  │
│  │  5 │ $67,250.30  │ 12.4 BTC │ ████████░░ │ 12.4     │  │
│  │  4 │ $67,249.80  │  5.2     │ ███░░░░░░░ │ 17.6     │  │  Asks
│  │  3 │ $67,248.50  │ 10.1     │ ██████░░░░ │ 27.7     │  │
│  │  2 │ $67,247.10  │  3.8     │ ██░░░░░░░░ │ 31.5     │  │
│  │  1 │ $67,246.20  │  7.5     │ ████░░░░░░ │ 39.0     │  │
│  │  ──│─────────────│──────────│────────────│──────────│  │
│  │    │ $67,245.00  │   MID    │            │          │  │
│  │  ──│─────────────│──────────│────────────│──────────│  │
│  │  1 │ $67,244.20  │  8.3     │ █████░░░░░ │  8.3     │  │
│  │  2 │ $67,243.10  │ 15.7     │ ██████████ │ 24.0     │  │  Bids
│  │  3 │ $67,241.80  │  2.1     │ █░░░░░░░░░ │ 26.1     │  │
│  │  4 │ $67,240.50  │  9.4     │ ██████░░░░ │ 35.5     │  │
│  │  5 │ $67,239.90  │  6.8     │ ████░░░░░░ │ 42.3     │  │
│  └───────────────────────────────────────────────────────┘  │
│  Spread: 1.5 bps | Imbalance: +0.12 (buy pressure) | Mid: $67,245.00 │
└──────────────────────────────────────────────────────────┘
```

### 6.4 Positions Tab (улучшенный)

Добавляем к существующему:
- Раскрывающийся detail по клику на строку: stop/TP цены, время входа, размер, риск
- Визуальная полоса SL→Entry→TP с текущей ценой-маркером
- Цветовое кодирование: зелёный (в плюсе), красный (в минусе), жёлтый (около стопа)

### 6.5 Signals Tab

Без критичных изменений, уже хорош. Добавить:
- Badge с количеством непросмотренных сигналов
- Авто-скролл вниз (уже есть)

### 6.6 Universe Tab

Добавить к существующему:
- Мини-индикатор состояния стратегий для каждого тикера (цветные точки 8×8px)
- Фильтр: показывать только тикеры с ready-стратегиями
- Скролл-таблица, не больше 15 строк видимо

### 6.7 Journal Tab

Добавить:
- График cumulative PnL по времени (Canvas, такой же как equity chart)

### 6.8 Controls Tab

Без критичных изменений. Добавить:
- Статус всех подключений с latency
- Версии компонентов

---

## 7. Модель данных (Бэкенд)

### 7.1 StrategyState (новое)

```cpp
enum class StrategyReadyState {
    Cold,       // только создана, нет данных
    Warming,    // копит данные, не все условия выполнены
    Ready,      // все условия есть, ждёт сигнала
    Planning,   // TradePlan сгенерирован, ждёт executor
    Trading,    // в активной сделке
    Cooldown    // пауза после выхода
};

struct StrategyCondition {
    std::string name;          // "CUSUM warmup", "Leader corr"
    double      current;       // текущее значение
    double      target;        // порог
    bool        met;           // выполнено?
    std::string unit;          // "%", "bps", "samples"
};

struct StrategyState {
    Ticker                       ticker;
    std::string                  strategy_name;
    StrategyReadyState           ready_state;
    double                       readiness_pct;  // met/total * 100 (см. §4.3)
    std::vector<StrategyCondition> conditions;
    std::string                  last_reject_reason;
    double                       seconds_since_last_reject;
    int                          signals_last_60s;
};
```

### 7.2 ChartPoint (новое)

```cpp
struct ChartPoint {
    int64_t ts_unix_ms;
    double  mid, best_bid, best_ask;
    double  spread_bps;
    double  buy_vol_5s, sell_vol_5s;
    double  volatility_1min_bps;
    double  tape_aggression;
    double  leader_change_1s, leader_correlation;
};
// Кольцевой буфер: 300 точек (~5 мин при 1с cadence)
```

### 7.3 ObLevel (новое)

```cpp
struct ObLevel {
    double price;
    double size;
};
// Топ 20 bids + топ 20 asks
```

### 7.4 Расширенный DashboardServer::State

```cpp
struct State {
    // ... существующие поля ...
    
    // Новые поля:
    std::vector<StrategyState>   strategy_states;
    std::vector<ChartPoint>      chart_history;      // последние 300 точек
    std::vector<ObLevel>         bids_top20;
    std::vector<ObLevel>         asks_top20;
    double                       ob_mid;
    double                       ob_spread_bps;
    double                       ob_imbalance;
    Ticker                       selected_ticker;
};
```

---

## 8. Потоки данных (Data Flow)

```
trading loop (100ms)            dashboard strand (250ms, async)
────────────────────            ──────────────────────────────
tick_all_tickers()              
  └─ обновляет FeatureFrame    
  └─ обновляет OrderBook       
  └─ strategy.tick()            
  └─ executor.process()        
                                collect_dashboard_state()
                                  └─ reader-lock shared state
                                  └─ engine.get_all_states()     → strategy_states
                                  └─ chart_snapshot()            → chart_history
                                  └─ ob_snapshot(20)             → bids/asks top20
                                  └─ serialize JSON (nlohmann)
                                  └─ dashboard_.update_state()
                                      └─ WebSocket broadcast

Частота обновлений:
  Strategy states ──── 250ms (throttled, не каждый tick)
  Chart history  ──── 250ms
  Order book     ──── 250ms (только для selected_ticker)
  Equity/Positions ── каждый tick (лёгкие данные, без тормозов)
```

**WebSocket payload budget:**
- Strategy states: ~2-5 KB (10 тикеров × 3 стратегии × ~150 байт каждая)
- Chart history: ~4-8 KB (300 точек × ~20 байт)
- Order book: ~1-2 KB (40 уровней × ~30 байт)
- Equity/Positions: ~1-2 KB (существующее)
- **Total:** ~8-17 KB на апдейт (раз в 250ms), ~32-68 KB/s на браузер — допустимо

---

## 9. Компоненты фронтенда

### 9.1 Файловая организация

```
src/control/
  dashboard.html         ← весь HTML/CSS/JS, редактируется как обычный файл
  dashboard_html.h       ← auto-generated cmake: xxd -i dashboard.html
  DashboardServer.cpp    ← #include "dashboard_html.h", использует dashboard_html[]
  DashboardServer.hpp    ← без изменений в API
```

### 9.2 Переиспользуемые UI-компоненты

| Компонент | CSS класс | Назначение |
|-----------|-----------|------------|
| Card | `.card` | Контейнер с backdrop-blur и border |
| KPI | `.kpi` | Метрика (label + value + sub) |
| Gauge | `.gauge-ring` | SVG-кольцо (margin, exposure, daily pnl) |
| ProgressRing | `.prog-ring` | Мини-кольцо для метрик разогрева |
| ProgressBar | `.progress-bar` | Линейный прогресс |
| HeatmapCell | `.hm-cell` | Ячейка heatmap-грида стратегий |
| DetailPanel | `.detail-panel` | Раскрывающаяся панель (slide анимация) |
| SignalChip | `.sig-chip` | Фильтр-чип в Signals tab |
| Toast | `.toast` | Всплывающее уведомление |
| Badge | `.badge` | Счётчик на вкладке |

### 9.3 Canvas-компоненты

| Компонент | Назначение |
|-----------|------------|
| `EquityChart` | График эквити (уже есть) |
| `PriceChart` | Ценовой график mid + volume бары |
| `DepthChart` | Стакан depth chart (bids/asks) |
| `MiniChart` | Мини-график спреда/волатильности/агрессии |

### 9.4 JavaScript API

```javascript
// Глобальное состояние
let _state = null;
let _selectedTicker = 'BTC_USDT';
let _expandedCell = null;  // {ticker, strategy}

// Рендер-функции (по одной на вкладку)
renderCommand(state);
renderCharts(state);
renderOrderBook(state);
renderPositions(state);
renderSignals(state);
renderUniverse(state);
renderJournal(state);
renderControls(state);

// API для смены тикера
selectTicker(ticker);  // → HTTP POST /api/ticker/select
```

---

## 10. CSS Grid-система

```css
/* Основные сетки */
.grid-2  { grid-template-columns: 1fr 1fr; }
.grid-3  { grid-template-columns: 1fr 1fr 1fr; }
.grid-4  { grid-template-columns: 1fr 1fr 1fr 1fr; }
.grid-2-1 { grid-template-columns: 2fr 1fr; }
.grid-1-2 { grid-template-columns: 1fr 2fr; }

/* Heatmap grid */
.heatmap-grid {
  display: grid;
  grid-template-columns: 140px repeat(auto-fill, minmax(100px, 1fr));
  gap: 2px;
  max-height: 320px;
  overflow-y: auto;
}

/* Charts layout */
.charts-grid {
  display: grid;
  grid-template-columns: 3fr 1fr;
  grid-template-rows: auto auto;
  gap: 8px;
}

/* Order book layout */
.ob-layout {
  display: grid;
  grid-template-columns: 1fr 1fr;
  grid-template-rows: 200px auto;
  gap: 8px;
}
```

---

## 11. Производительность

- Dashboard data collection: async strand, 250ms interval — не блокирует trading
- WebSocket JSON payload: ~8-17 KB на апдейт (раз в 250ms)
- Order book levels: throttle встроен в 250ms strand
- Canvas-графики перерисовываются только при новых данных (requestAnimationFrame)
- CSS-анимации через `transform` и `opacity` (GPU-ускоренные)
- Heatmap: виртуальный скролл не нужен, максимум ~30 тикеров × 3 стратегии = 90 ячеек

---

## 12. Состояния загрузки и ошибок

| Состояние | Отображение |
|-----------|-------------|
| Нет данных | Пустая карточка с иконкой и текстом "Waiting for data..." |
| WS отключён | Серый индикатор, попытка переподключения каждые 2с |
| Нет позиций | "No active positions" в Positions tab |
| Нет сигналов | "No signals yet" в Signals tab |
| Нет сделок | "No trades yet" в Journal tab |
| Heatmap пуст | "No strategies active" с подсказкой проверить universe |

---

## 13. Rollback Strategy

Перед началом Phase 4 (фронтенд):
```bash
git tag dashboard-v1-backup  # точка восстановления старого дашборда
```

Если новый дашборд сломан:
```bash
git checkout dashboard-v1-backup -- src/control/DashboardServer.cpp
cmake --build build
```

Старый `DashboardServer.cpp` содержит embed-строку с HTML — она полностью самодостаточна, откат безопасен.
