# Trade Bot — Hardcore Spec

Агентный скальпинг-бот для **MetaScalp** (фьючерсный маркет-мейкер / агрегатор ликвидности). Написан на C++23 с фокусом на детерминизм, низкую latency и полностью формальную логику принятия решений (никакого ML в production).

---

## Архитектура

```
┌─────────────────────────────────────────────────────────────────┐
│                        MetaScalp (localhost)                    │
│              HTTP REST :17845–17855    WS :17845–17855          │
└────────┬──────────────────────────────────────┬─────────────────┘
         │ REST                                  │ WebSocket
         ▼                                      ▼
┌────────────────────────┐  ┌──────────────────────────────┐
│   Transport Layer      │  │  MarketDataFeed              │
│   IHttpClient          │  │  IWsClient / BeastWsClient   │
│   OrderGateway         │  │  NotificationFeed            │
│   MetaScalpDiscovery   │  │  ReplayFeed                  │
└────────┬───────────────┘  └────────┬─────────────────────┘
         │                            │
         └────────────────┬───────────┘
                          ▼
┌──────────────────────────────────────────────────────────┐
│                    Processing Pipeline                    │
│                                                          │
│  TickerUniverse → OrderBook + TradeStream + LeaderTracker│
│       → FeatureExtractor (10–20 Hz FeatureFrame)         │
│       → SignalDetectors (density, iceberg, tape,         │
│         level, approach, leader)                         │
│       → StrategyEngine → RiskManager → Executor          │
└──────────────────────────────────────────────────────────┘
```

---

## Быстрый старт

### Требования

| Компонент | Версия |
|-----------|--------|
| Linux (Pop!_OS / Ubuntu 22.04+) | ядро 5.6+ (io_uring) |
| GCC | 14+ (C++23) |
| CMake | 3.28+ |
| Conan | 2.x |
| Ninja | recommended |

### Сборка

```bash
# Release (production)
./scripts/build.sh release

# Debug (ASan+UBSan, тесты)
./scripts/build.sh debug --tests

# Быстрая пересборка (без conan install + cmake configure)
./scripts/build.sh debug --quick

# Только trade_bot бинарник (без тестов, labeler)
./scripts/build.sh release --bin

# Очистка
./scripts/build.sh clean
```

**Флаги:**
- `--tests` — включить `BUILD_TESTS=ON`
- `--bench` — включить `BUILD_BENCHMARKS=ON`
- `--no-conan` — пропустить `conan install` (использовать кеш)
- `--quick` — пропустить conan install + cmake configure (fast incremental)
- `--bin` — собрать только `trade_bot` (без тестов/лейблера)
- `--jobs N` — переопределить число параллельных задач

### Тестирование

```bash
# Запустить все тесты (debug сборка)
./scripts/test.sh

# Релизная сборка
./scripts/test.sh release

# Фильтр по имени теста
./scripts/test.sh debug --filter "orderbook"
```

### Конфигурация

```bash
# Скопировать и отредактировать
cp config/config.example.toml config.toml
# Запустить
./build/release/bin/trade_bot --config config.toml
```

---

## Структура проекта

```
trade_bot/
├── CMakeLists.txt                            # Главный cmake-файл
├── CMakePresets.json                         # Presets: release (production), debug (ASan)
├── conanfile.txt                             # Зависимости: boost, spdlog, fmt, nlohmann_json, …
├── .clang-format / .clang-tidy
├── src/                                      # C++ ядро бота (~40 модулей)
│   ├── main.cpp                              # Точка входа
│   ├── app/                                  # BotApp оркестрация (init, run)
│   ├── transport/                            # HTTP (libcurl) + WS (Boost.Beast)
│   ├── domain/                               # Чистые доменные типы
│   ├── marketdata/                           # OrderBook, TradeStream, LeaderTracker
│   ├── features/                             # FeatureFrame, FeatureExtractor
│   ├── signals/                              # 7 детекторов + SignalBus + SignalLevelBridge
│   ├── strategy/                             # StrategyEngine + 3 стратегии
│   ├── universe/                             # TickerUniverse + affinity-фильтры
│   ├── risk/                                 # RiskManager, NewsCalendar, AccountState
│   ├── executor/                             # LiveExecutor, PaperExecutor, StartupRecovery
│   ├── control/                              # DashboardServer, KillSwitch, TickerController
│   ├── logger/                               # spdlog + TradeJournal (JSONL)
│   ├── config/                               # TOML-конфиг
│   ├── trading/                              # OrderReconciliator
│   ├── metrics/                              # HDR Histogram, Prometheus exporter
│   ├── perf/                                 # CpuPinning, TimestampMonitor
│   ├── numeric/                              # Welford, Kahan, KDE, ZigZag, HMM
│   └── utils/                                # CircularBuffer, UrlEncoder, FixedString
├── test/                                     # ~70 unit-тестов + integration + contract
├── bench/                                    # Google Benchmark (hot-path бенчмарки)
├── tool/labeler/                             # Инструмент разметки сигналов
├── spec/                                     # Архитектурная документация (см. docs/spec/)
├── dashboard/                                # React/Vite Web UI (TypeScript)
│   ├── src/                                  # 20+ компонентов на React 19
│   └── package.json
├── config/                                   # Конфигурация
│   ├── config.example.toml                   # Пример с дефолтами
│   └── news_calendar.*                       # Календарь новостей (R12)
├── config.toml                               # Runtime конфигурация
├── reports/                                  # Результаты аудитов и инспекций
├── metascalp-sdk/                            # SDK MetaScalp API (C# + python + js)
├── scripts/
│   ├── build.sh                              # Универсальная сборка
│   └── test.sh                               # Запуск тестов (ctest)
├── replay/                                   # Данные реплея
│   ├── dumps/                                # Сохранённые WS-сессии (dump.ndjson)
│   └── labels/                               # Размеченные сигналы
├── journal/                                  # JSONL журнал сделок (runtime)
├── .github/workflows/ci.yml                  # CI
└── package.json                              # Root package.json (для агентов)
```

---

## Технологический стек

| Компонент | Технология |
|-----------|-----------|
| **Язык** | C++23 (ISO/IEC 14882:2024) |
| **Сборка** | CMake 3.28+ / Conan 2.x |
| **HTTP** | libcurl |
| **WebSocket** | Boost.Beast |
| **JSON** | nlohmann_json |
| **Логи** | spdlog + fmt |
| **Конфиг** | tomlplusplus (TOML) |
| **Контейнеры** | absl::btree_map (OrderBook), absl::flat_hash_map |
| **SIMD** | xsimd (AVX2 baseline) |
| **Тесты** | Google Test + Google Benchmark |
| **Латенси** | HDR Histogram |
| **Frontend** | React 19 + Vite + Tailwind CSS 4 |

---

## Сигналы (7 типов)

| Сигнал | Алгоритм | Описание |
|--------|----------|----------|
| DensityDetected | SIMD-сумма стакана | Плотность > 10× avg, стоит > 2с |
| IcebergDetected | Bayesian update | Накопление evidence скрытого объёма |
| TapeBurst | Hawkes process | Всплеск агрессии (3× baseline) |
| TapeFade | CUSUM change-point | Затухание ленты на 200–500 мс раньше |
| LevelFormed | DBSCAN + KDE | Горизонтальный уровень |
| LevelApproach | ZigZag + HMM | Импульс/медленный/консолидация |
| LeaderMove | Kalman lag | Лидер двинулся — мы отстаём |

---

## Стратегии

| Стратегия | Триггер | Entry |
|-----------|---------|-------|
| **BounceFromDensity** | LevelApproach + плотность у уровня | Limit |
| **BreakoutEatThrough** | DensityEating + TapeBurst | Market / aggressive limit |
| **LeaderLag** | LeaderMove + lag > threshold | Market |
| **FlushReversal** | TapeFlush + liquidation | Фаза 5 |

---

## Производительность (SLO)

| Путь | p50 | p99 |
|------|-----|-----|
| WS → OrderBook | 0.5 мс | 2 мс |
| Book → FeatureFrame | 1 мс | 5 мс |
| FeatureFrame → Signal | 2 мс | 10 мс |
| Signal → TradePlan | 1 мс | 5 мс |
| **Total book → submit** | **5 мс** | **25 мс** |

---

## Документация

Вся архитектурная документация — в [`docs/spec/`](docs/spec/):

- [Архитектура](docs/spec/ARCHITECTURE.md) — модули, потоки, performance engineering, численная корректность
- [Стратегии](docs/spec/STRATEGIES.md) — детальная бизнес-логика всех стратегий
- [Сигналы](docs/spec/SIGNAL_DETECTION.md) — формулы и пороги детекторов
- [Риск-менеджмент](docs/spec/RISK_MANAGEMENT.md) — 15 правил + kill-switch
- [API-контракт](docs/spec/METASCALP_API_CONTRACT.md) — документированный контракт MetaScalp
- [Roadmap](docs/spec/ROADMAP.md) — план развития на 5 фаз
- [Task Specs](docs/spec/TASK_SPECS.md) — технические спецификации задач


