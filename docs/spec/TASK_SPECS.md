# TASK_SPECS

Тикеты для ИИ-исполнителя (или младшего инженера). Каждый тикет —
**самодостаточный** и имеет чёткие acceptance criteria. Тикеты идут
в порядке выполнения; зависимости указаны явно.

## Формат тикета

```
ID: T<фаза>-<код>
Title: ...
Depends on: [список ID]
Phase: 0 / 1 / 2 / 3 / 4 / 5 (см. ROADMAP.md)
Deliverables:
  - файлы, которые должны быть созданы/изменены
Acceptance criteria:
  - тест, который должен проходить
  - команда, которая должна работать
Out of scope:
  - что явно НЕ делается в этом тикете
```

**Правила для исполнителя:**

1. Не выходить за scope тикета.
2. Любое число, которое "хочется захардкодить", — идёт в `config.toml` и читается через `Config::get<T>(path)`.
3. Код с зависимостями от транспортного слоя — только в `core/src/transport/`. Выше работаем с доменными типами.
4. Каждый тикет должен заканчиваться зелёным `ctest` и без warnings при `-Wall -Wextra -Wpedantic -Werror`.
5. После завершения — обновить `CHANGELOG.md` (одной строкой с ID тикета).

**Важно:** после реорганизации проекта (Этап 3) C++ ядро находится в `core/`. Все пути, указанные в тикетах (`src/`, `test/`, `bench/`, `tool/`, `CMakeLists.txt`, `conanfile.txt`), фактически находятся в `core/`:
- `src/...` → `core/src/...`
- `test/...` → `core/test/...`
- `bench/...` → `core/bench/...`
- `tool/...` → `core/tool/...`
- `CMakeLists.txt` → `core/CMakeLists.txt`
- `conanfile.txt` → `core/conanfile.txt`

---

## Фаза 0 — Фундамент

### T0-SETUP: Скелет CMake-проекта

**Depends on:** —

**Deliverables:**
- `CMakeLists.txt` (корневой)
- `conanfile.txt` с зависимостями: `libcurl/8.x`, `boost/1.83.0` (beast+asio), `nlohmann_json/3.11`, `spdlog/1.13`, `tomlplusplus/3.4`, `abseil-cpp/20230802`, `gtest/1.14`
- Структура каталогов как в `ARCHITECTURE.md § 6`
- `.clang-format` (LLVM-based, 100-col), `.clang-tidy` (readability-*, bugprone-*, performance-*, modernize-*)
- `src/main.cpp` — печатает `"trade_bot v0.0.1"` и выходит
- `test/unit/smoke_test.cpp` — тест, проверяющий что gtest запускается

**Acceptance criteria:**
- `cmake --preset release && cmake --build --preset release` — успешно на Linux (clang-18, gcc-13). Это единственная обязательная платформа до Фазы 4.
- `ctest --preset release` — один зелёный тест
- `./build/release/trade_bot` печатает версию

**Out of scope:** транспорт, логгер, конфиг. **Windows-сборка не требуется** в Фазе 0 — может быть добавлена отдельным будущим тикетом при необходимости.

---

### T0-LOG: Логгер + флаги компилятора

**Depends on:** T0-SETUP

**Deliverables:**
- `src/logger/Logger.{hpp,cpp}` — обёртка над spdlog: уровни `trace/debug/info/warn/error/critical`, вывод в файл `logs/trade_bot-YYYY-MM-DD.log` (с ротацией по суткам) и в stderr
- В CMake: `-Wall -Wextra -Wpedantic -Werror` для всех таргетов, `-fsanitize=address,undefined` для Debug
- В `main.cpp` — пять примеров лог-сообщений разных уровней

**Acceptance criteria:**
- Debug-сборка с ASan/UBSan компилируется и запускается без ошибок
- Логи появляются и в консоли, и в файле
- Тест `test/unit/logger_test.cpp` — проверяет, что `Logger::info(...)` пишет правильный формат `[YYYY-MM-DDTHH:MM:SS.sssZ] [info] <msg>`

**Out of scope:** TradeJournal — это отдельный тикет T3-JOURNAL.

---

### T0-NUMERIC: Numerical primitives (fixed-point + Welford + Kahan)

**Depends on:** T0-SETUP

**Deliverables:**
- `src/numeric/FixedPoint.hpp` — `PriceTick`, `SizeFix` strong types (см. `ARCHITECTURE.md § 11.3`):
  - `PriceTick::from_price(double, double increment) -> PriceTick`, `to_price(double increment) -> double`
  - арифметика: `+ - * /` с overflow-check в Debug (`__builtin_*_overflow`), без проверок в Release
  - сравнения: `<=>` (C++20 spaceship), `operator==` — bit-precise
  - hash для `absl::flat_hash_map`
- `src/numeric/Welford.hpp` — generic `WelfordAccumulator<T>` (`update(x)`, `mean()`, `variance()`, `stdev()`) согласно ARCH § 11.1
- `src/numeric/WelfordCorrelation.hpp` — extended Welford для Pearson correlation двух потоков (Schubert & Gertz 2018), используется в `LeaderTracker`
- `src/numeric/Kahan.hpp` — `KahanAccumulator<T>` для compensated summation согласно ARCH § 11.2
- `src/numeric/Ema.hpp` — `Ema<T>` (single + DEMA-комбинация согласно ARCH § 11.6), constructor от `α` или от `N`-period
- `src/numeric/HdrHistogramWrapper.hpp` — обёртка над `HdrHistogram_c` для latency tracking (см. ARCH § 11.5)

**Acceptance criteria:**
- `test/unit/fixed_point_test.cpp`: round-trip `double → PriceTick → double` точен до `PriceIncrement`; overflow в Debug ловится
- `test/unit/welford_test.cpp`: на 100k samples из N(0,1) — `|mean - 0| < 0.01`, `|stdev - 1| < 0.01`; корректность на edge cases (n=1, identical samples)
- `test/unit/welford_correlation_test.cpp`: на синтетике с известным `corr=0.7` — погрешность < 0.001 на 1000 samples
- `test/unit/kahan_test.cpp`: сумма 1e7 чисел `0.1` — Kahan ≤ 1e-9 ошибки, naive `> 1e-3`
- `test/unit/ema_test.cpp`: DEMA(N=20) реагирует на step-change в 2× быстрее SMA(N=20); проверено на synthetic step-signal
- `test/unit/hdr_histogram_test.cpp`: точность p99 ≤ 0.1% на N=1M samples из exponential distribution

**Out of scope:** SIMD-векторизация (это T0-PERF).

---

### T0-PERF: Performance infrastructure

**Depends on:** T0-NUMERIC, T0-LOG

**Deliverables:**
- `src/perf/CpuPinning.{hpp,cpp}` — `pin_thread_to_cpu(int cpu_id)` через `pthread_setaffinity_np`; runtime check `cpuset` доступен → fallback с WARN если нет (см. ARCH § 10.2)
- `src/perf/ArenaAllocator.{hpp,cpp}` — `std::pmr::monotonic_buffer_resource` обёртка с per-ticker arena (256 KiB конфиг), API совместим с STL контейнерами через `pmr::polymorphic_allocator`
- `src/perf/SpscQueue.hpp` — алиас `boost::lockfree::spsc_queue<T, capacity<N>>` с фиксированной capacity
- `src/perf/MpmcQueue.hpp` — обёртка `moodycamel::ConcurrentQueue` для multi-producer-multi-consumer
- `src/perf/CachelinePadded.hpp` — `template<class T> struct alignas(64) CachelinePadded { T value; }` для shared между потоками
- `src/perf/SimdOps.{hpp,cpp}` — обёртки над `xsimd` для:
  - `simd_sum_double(const double*, size_t)` — для `OrderBook::depth(N)`
  - `simd_dot_product(const double*, const double*, size_t)` — для correlation
  - runtime dispatch AVX2/AVX-512/scalar fallback (через `xsimd::available_architectures()`)
- `src/perf/LatencyTracer.{hpp,cpp}` — RAII-обёртка `Tracer trc("book_to_feature_us")` → запись в HdrHistogram (T0-NUMERIC) + Prometheus при shutdown trace; запись лок-фри (`LatencySample` event в MPMC)
- benchmark suite `bench/`: `bench_orderbook_apply.cpp`, `bench_welford.cpp`, `bench_kahan.cpp`, `bench_simd_sum.cpp`, `bench_arena_alloc.cpp` через `google/benchmark`
- conanfile.txt дополнения: `xsimd/13.0`, `concurrentqueue/1.0` (moodycamel), `hdr_histogram_c/0.11`, `boost.lockfree` (входит в boost)

**Acceptance criteria:**
- `test/unit/cpu_pinning_test.cpp`: `pin_thread_to_cpu(0)` на Linux в CI работает; на системе без cpuset — WARN, не падение
- `test/unit/arena_alloc_test.cpp`: 1M аллокаций по 64 байта — нет роста heap (RSS-monitoring через `getrusage`)
- `test/unit/spsc_queue_test.cpp`: producer 1M events / consumer — drop=0, p99 latency push/pop < 1 µs
- `bench/bench_simd_sum.cpp`: `simd_sum_double(arr, 16)` ≥ 3× быстрее scalar на AVX2 машине
- `bench/bench_orderbook_apply.cpp`: 100k random updates — p50 < 50 µs, p99 < 200 µs (см. SLO ARCH § 3.1)
- CI gate: regression check — нарушение порога p99 на ±5% от main → fail

**Out of scope:** io_uring (опционально, отдельный T0-IOURING если потребуется).

---

### T0-MONITOR-TIMESTAMPS: Мониторинг точности временных меток

**Depends on:** T0-PERF, T0-FEED

**Deliverables:**
- Расширение `LatencyTracer` для записи задержек между получением `trade_update` и `orderbook_update`.
- Метрики для отслеживания джиттера (стандартного отклонения) временных меток от MetaScalp API.

**Acceptance criteria:**
- `test/integration/timestamp_jitter_test.cpp`: При подключении к MetaScalp, собирает 1000 пар `trade_update`/`orderbook_update` и проверяет, что p99 задержки между ними < 10 мс, а джиттер < 2 мс. В случае превышения порогов — WARN.
- В `LatencyTracer` появляются новые метрики: `trade_orderbook_latency_us`, `trade_orderbook_jitter_us`.

**Out of scope:** Автоматическая коррекция временных меток.

---

### [COMPLETED] T0-CONFIG: Конфигурация

**Depends on:** T0-LOG

**Deliverables:**
- `src/config/Config.{hpp,cpp}` — обёртка над `toml++`
  - `Config::load(path)` — загружает файл, бросает `ConfigError` с читабельной причиной
  - `Config::get<T>(std::string_view dotted_path)` — тайп-безопасный геттер (int, double, bool, string, std::vector<string>)
  - `Config::has(path)` — проверка существования
- `config/config.example.toml` — полный пример со всеми секциями из `SIGNAL_DETECTION.md`, `STRATEGIES.md`, `RISK_MANAGEMENT.md`
- Строгая валидация: при запуске бота читаем все required-ключи и падаем с понятной ошибкой, если чего-то нет

**Acceptance criteria:**
- `test/unit/config_test.cpp` покрывает: happy path, отсутствующий ключ, неверный тип, неверный путь
- `main.cpp`: если `config.toml` отсутствует — бот печатает `Config 'config.toml' not found; copy from config/config.example.toml` и выходит с кодом 2

**Out of scope:** hot-reload конфига в рантайме (в этой версии не делаем).

---

### T0-ORDER-RECONCILIATION: Механизмы разрешения неопределенности ордеров

**Depends on:** T0-HTTP, T0-WS, T0-DOMAIN

**Deliverables:**
- `src/trading/OrderReconciliator.{hpp,cpp}`: Модуль для разрешения неопределенности ордеров в состоянии `SubmitUnknown`.
  - Методы: `enter_submit_unknown(Ticker, OrderId)`, `poll_open_orders(Ticker)`, `resolve_order(Ticker, OrderId, MetaScalpOrderId)`.
- Реализация компенсаторных механизмов, описанных в `METASCALP_API_CONTRACT.md` для `SubmitUnknown`.

**Acceptance criteria:**
- `test/unit/order_reconciliator_test.cpp`: Мокирование MetaScalp API для симуляции:
  - HTTP-таймаута при отправке ордера.
  - Успешного обнаружения ордера при последующем опросе.
  - Необнаружения ордера и перехода в состояние ожидания ручного вмешательства.
- `test/integration/submit_unknown_contract_test.cpp`: Реальный тест с MetaScalp (с флагом `--with-metascalp`):
  - Отправка ордера, искусственное создание HTTP-таймаута (например, через `iptables` или задержку на прокси).
  - Проверка корректного перехода бота в `SubmitUnknown` и последующего разрешения состояния (либо обнаружение ордера, либо алерт).

**Out of scope:** Автоматическое повторное выставление ордеров без подтверждения.

---

### [COMPLETED] T0-HTTP: HTTP-клиент + MetaScalp discovery

**Depends on:** T0-LOG

**Deliverables:**
- `src/transport/IHttpClient.hpp` — интерфейс
- `src/transport/CurlHttpClient.{hpp,cpp}` — реализация (blocking, через libcurl easy-интерфейс)
  - методы: `get(url) -> HttpResponse`, `post(url, json) -> HttpResponse`, `put`, `del`
  - `HttpResponse { int status; std::string body; std::map<std::string,std::string> headers; }`
  - таймаут настраиваемый (default 5 сек)
- `src/transport/MetaScalpDiscovery.{hpp,cpp}`:
  - `discover() -> std::optional<int>` — сканирует порты 17845..17855, на каждом делает `GET /ping`, проверяет `App == "MetaScalp"`, возвращает первый найденный порт (или nullopt через 5 сек)
  - логирует `Version` из ответа

**Acceptance criteria:**
- `test/unit/http_client_test.cpp` на моке (`httpmock` или собственный мини-сервер на boost.beast в тесте)
- `test/integration/discover_test.cpp` (запускается с флагом `--with-metascalp` — если MetaScalp не поднят, тест скипается)

**Out of scope:** WebSocket, OrderGateway.

---

### [COMPLETED] T0-WS: WebSocket-клиент

**Depends on:** T0-HTTP

**Deliverables:**
- `src/transport/IWsClient.hpp` — интерфейс
  - `connect(url)`, `send(std::string_view)`, `disconnect()`
  - колбэки: `on_message(json)`, `on_close(code, reason)`, `on_error(msg)`
- `src/transport/BeastWsClient.{hpp,cpp}` — реализация на Boost.Beast (async) — один io_context, запущенный в отдельном потоке
- Автоматический `reconnect` с экспоненциальным backoff (1s, 2s, 4s, 8s, max 30s)
- heartbeat-пинг каждые 20 сек (просто WebSocket ping, MetaScalp сам отвечает)

**Acceptance criteria:**
- `test/integration/ws_smoke_test.cpp`: подключается к живому MetaScalp, отправляет `{"Type":"subscribe","Data":{"ConnectionId":1}}`, получает ack `subscribed`, отключается — 0 утечек под ASan

**Out of scope:** парсинг доменных событий (это T0-DOMAIN + T0-FEED).

---

### [COMPLETED] T0-DOMAIN: Доменные типы

**Depends on:** T0-SETUP

**Deliverables:**
- `src/domain/Types.hpp`:
  - `enum class Side { None, Buy, Sell }`
  - `enum class OrderType { Limit, Stop, StopLoss, TakeProfit, Market }`
  - `enum class OrderStatus { New, Open, Closed }`
  - `enum class PositionStatus { New, Open, Closed }`
  - `using Ticker = std::string;` (plain alias, валидация — отдельно)
  - `struct PriceLevel { double price; double size; Side side; };`
  - `struct Trade { double price; double size; Side side; std::chrono::system_clock::time_point timestamp; };`
  - `struct OrderBookSnapshot { Ticker ticker; std::vector<PriceLevel> asks; std::vector<PriceLevel> bids; std::chrono::system_clock::time_point ts; };`
  - `struct OrderBookUpdate { Ticker ticker; std::vector<PriceLevel> changes; std::chrono::system_clock::time_point ts; };` (size=0 означает удаление)
  - `struct OrderUpdate { int64_t order_id; Ticker ticker; Side side; OrderType type; double price; double filled_price; double size; double filled_size; double fee; std::string fee_currency; OrderStatus status; std::chrono::system_clock::time_point time; };` (WS `order_update`)
  - `struct RestOrder { int64_t id; std::optional<std::string> client_id; Ticker ticker; Side side; OrderType type; double price; double size; double filled_size; double filled_price; double remaining_size; OrderStatus status; std::optional<double> trigger_price; std::chrono::system_clock::time_point create_date; };` (REST `GET /api/connections/{id}/orders?Ticker=...`)
  - `struct PlaceOrderResult { std::string status; std::string client_id; double execution_time_ms; };`
  - `struct PositionUpdate { ... }` (WS `position_update`, включает `AvgPrice`, `AvgPriceFix`, `AvgPriceDyn`)
  - `struct BalanceEntry { std::string coin; double total, free, locked; };`
  - `struct BalanceUpdate { std::vector<BalanceEntry> balances; };`

**Acceptance criteria:**
- Все типы — `struct`, без методов (кроме operator==, в unit-тесте покрыт)
- Никаких зависимостей от boost/curl/spdlog в этом файле — чистый STL + chrono

**Out of scope:** сериализация (это T0-CODEC).

---

### [COMPLETED] T0-CODEC: Парсинг сообщений MetaScalp (JSON codec)

**Depends on:** T0-DOMAIN, T0-WS

**Deliverables:**
- `src/transport/MetaScalpCodec.{hpp,cpp}`:
  - одна таблица JSON-ключей `api::fields::*` (как в `ARCHITECTURE.md § 8`)
  - функции `parse_order_update(json) -> OrderUpdate`, `parse_rest_order(json) -> RestOrder`, `parse_place_order_result(json) -> PlaceOrderResult`, `parse_position_update`, `parse_balance_update`, `parse_trade_update -> std::vector<Trade>`, `parse_orderbook_snapshot`, `parse_orderbook_update`, `parse_finres_update -> FinresUpdate`, `parse_signal_level_triggered -> SignalLevelTriggered`
  - **дуальный парсинг `OrderType`**: в WS приходит строкой (`"Limit"`, `"Stop"`, `"StopLoss"`, `"TakeProfit"`, `"Market"`), в REST — integer (0..4). Реализовать `parse_order_type(json::value_type)` с ветвлением по `is_string()` / `is_number()`, маппинг integer→enum задокументирован в комментарии к функции. Любое неизвестное значение — `ParseError` с полем `OrderType=<value>`.
  - **поля `AvgPriceFix` и `AvgPriceDyn`** есть на `PositionUpdate`, не на `OrderUpdate`: `AvgPriceFix` → `avg_fill_price` (weighted average of entry orders only, для BE-стопа после TP1), `AvgPriceDyn` → `avg_fill_price_dyn` (adjusted by realized exit profit, для UI/аналитики). См. ARCH § 2.8, README OQ-5, METASCALP_API_CONTRACT § 5.
  - **graceful degradation**: неизвестные поля — WARN лог и игнор; отсутствующие required поля — `ParseError` с именем поля

**Acceptance criteria:**
- `test/unit/codec_test.cpp`: набор фикстур (эталонные JSON из документации MetaScalp) парсится корректно
- Отдельный тест `codec_order_type_dual_test.cpp`: строковый `"Stop"` и integer `1` дают одинаковый `OrderType::Stop`; неизвестные `7` и `"Unknown"` — ParseError
- Тест на fixture `position_update` с `AvgPriceFix != AvgPriceDyn` после частичного закрытия
- Тест `codec_finres_test.cpp` и `codec_signal_level_test.cpp` на их фикстуры
- Тест на неизвестное поле: не падает, пишет WARN
- Тест на отсутствующее required поле: бросает ParseError

**Out of scope:** Feed/Gateway.

---

### [COMPLETED] T0-FEED: MarketDataFeed (WS-обёртка)

**Depends on:** T0-CODEC

**Deliverables:**
- `src/transport/MarketDataFeed.{hpp,cpp}` — высокоуровневая обёртка: держит `IWsClient`, подписывается на нужные тикеры (trade_subscribe / orderbook_subscribe / subscribe по ConnectionId), превращает сырые JSON-сообщения через `MetaScalpCodec` в сильнотипизированные события и раздаёт через интерфейс `IMarketDataListener`
- поддержка `subscribed`/`unsubscribed` ack'ов и корректной обработки `error`

**Acceptance criteria:**
- `test/integration/feed_smoke_test.cpp`: на живом MetaScalp подписывается на BTCUSDT, получает ≥ 1 trade + orderbook_snapshot в течение 30 сек
- Unit-тест с mock-WS: проверяет корректную переподписку после reconnect

---

### [COMPLETED] T0-GATEWAY: OrderGateway (REST-обёртка)

**Depends on:** T0-CODEC

**Deliverables:**
- `src/transport/OrderGateway.{hpp,cpp}` — REST-обёртка над документированными endpoints: `POST /api/connections/{id}/orders`, `POST /api/connections/{id}/orders/cancel`, `POST /api/connections/{id}/orders/cancel-all`, `GET /api/connections/{id}/orders?Ticker=...`, `GET /api/connections/{id}/positions`, `GET /api/connections/{id}/balance`, `GET /api/connections/{id}/tickers`, `GET /api/connections`, `GET /api/connections/{id}` (single, для health-check)
- **обязательные сигнатуры** (контракт между T0-GATEWAY и потребителями T1-UNIVERSE / T4-EXECUTOR / T4-RECOVERY):
  ```cpp
  std::vector<ConnectionInfo> get_connections();
  ConnectionInfo get_connection(int id);                          // health-check
  std::vector<TickerInfo> get_tickers(int connection_id, bool refresh = false);
  std::vector<OrderInfo> get_open_orders(int connection_id, const Ticker&);
  std::vector<PositionInfo> get_positions(int connection_id);
  BalanceInfo get_balance(int connection_id);
  PlaceOrderResult place_order(int connection_id, const PlaceOrderRequest&);
  void cancel_order(int connection_id, int64_t order_id);
  void cancel_all_orders(int connection_id, const Ticker&);
  ```
- ответы парсятся через `MetaScalpCodec`, ошибки маппятся в доменные исключения

**Acceptance criteria:**
- `test/unit/order_gateway_test.cpp` с mock HTTP
- `test/integration/order_gateway_smoke_test.cpp` (опционально, только с `--with-metascalp`): запрос `GET /api/connections/{id}/balance` возвращает данные

---

### T0-REPLAY: Replay-источник

**Depends on:** T0-CODEC

**Deliverables:**
- `src/transport/ReplayFeed.{hpp,cpp}`:
  - читает файл `*.ndjson`, где каждая строка — объект `{ "recv_ts_ns": <int>, "message": <raw JSON> }`
  - проигрывает сообщения с опцией `speed_multiplier` (1.0 = реальное время, 0.0 = as-fast-as-possible)
  - виртуализированные часы через `IClock` интерфейс (реализация `WallClock` и `VirtualClock`)
- `tool/recorder/`: утилита, которая подключается к живому MetaScalp и пишет дамп-файл в формате, который читает ReplayFeed
- 1 эталонный дамп в `replay/dumps/btcusdt_5min_sample.ndjson` (записанный заранее, checked in git LFS если большой)

**Acceptance criteria:**
- `test/integration/replay_roundtrip_test.cpp`: записываем 30 сек событий в тестовый файл, проигрываем через ReplayFeed, сверяем что наблюдатель получил все сообщения в том же порядке

**Out of scope:** OrderBook-проверка по replay (это T1-ORDERBOOK).

---

### T0-CLOCK: Мониторинг дрифта системных часов

**Depends on:** T0-LOG

**Deliverables:**
- `src/control/ClockDriftMonitor.{hpp,cpp}`:
  - периодически (раз в `clock_check_interval_sec`, default **30**) сравнивает `std::chrono::system_clock::now()` с NTP-источником (`pool.ntp.org` или конфигурируемый)
  - использует ограниченный SNTP-клиент (поверх `asio::udp::socket`) — без сторонних зависимостей сверх существующих boost
  - экспонирует `drift_ms() -> int64_t` (knuth moving-average)
  - генерит WARN при `|drift| >= clock_warn_drift_ms` (default **200**)
  - при `|drift| >= max_clock_drift_ms` (default **500**) — вызывает `KillSwitch::trigger(ClockDrift)`
- конфиг-секция `[clock]`: `sources = ["pool.ntp.org", "time.google.com"]`, `check_interval_sec`, `warn_drift_ms`, `max_clock_drift_ms`

**Acceptance criteria:**
- `test/unit/clock_drift_monitor_test.cpp` с mock-NTP (фейковый UDP-сервер в тесте): возвращает смещение +700 мс → через 1 полл WARN, через 2 полла — KillSwitch triggered
- Failover: если основной NTP-сервер не ответил за таймаут, используется следующий из списка

**Out of scope:** коррекция системного времени (это обязанность OS/chrony/ntpd).

---

### T0-NEWS: News calendar loader (перенесён из Фазы 4)

**Depends on:** T0-CONFIG

**Deliverables:**
- `src/risk/NewsCalendar.{hpp,cpp}` — читает `news_calendar.json` (schema: массив `{ts_utc, importance, ticker?, note}`), предоставляет `minutes_to_next_news(now, ticker)`
- `config/news_calendar.example.json` — пример со структурой
- `config/news_calendar.schema.json` — JSON-Schema, валидация при загрузке
- hot-reload при SIGHUP (обязательно — оператор может обновить календарь без рестарта)
- `watch_file` через inotify (Linux): автоматический reload при изменении файла

**Acceptance criteria:**
- `test/unit/news_calendar_test.cpp`: загрузка валидного/невалидного файла, корректный расчёт `minutes_to_next_news`
- тест на hot-reload: файл перезаписан → новый календарь подгрузился за ≤ 2 сек
- тест на SIGHUP: отправка SIGHUP → reload

**Почему перенесён:** R12 news blackout нужен уже в paper-торговле (Фаза 3),
чтобы не обучаться на сделках, которые в live торговались бы через блэкаут.

**Out of scope:** автоматический скачиватель календаря — оператор кладёт файл сам.

---

### T0-KILLSWITCH: Kill-switch

**Depends on:** T0-LOG

**Deliverables:**
- `src/control/KillSwitch.{hpp,cpp}`:
  - watchdog-тред: каждые 500 мс проверяет файл `./killswitch` и системные сигналы (SIGINT, SIGTERM)
  - `KillSwitch::is_triggered()` — потокобезопасный геттер
  - `KillSwitch::trigger(reason)` — устанавливает флаг и создаёт файл
- В `main.cpp` — если файл существует при старте — не стартуем, печатаем `Kill-switch file present, remove it to run` и выходим с кодом 42

**Acceptance criteria:**
- `test/unit/killswitch_test.cpp`: создаём файл, убеждаемся что `is_triggered()` становится true в течение 1 сек
- SIGINT приводит к `is_triggered() == true`

---

### T0-GATE — Фаза-0 integration gate

**Depends on:** все T0-*

**Acceptance criteria:**
- бот подключается к живому MetaScalp
- делает discover
- подписывается на BTCUSDT (trades + orderbook)
- в течение 60 сек пишет все события в лог
- при Ctrl-C корректно закрывается (никаких утечек, все ордера отменены — но в фазе 0 ордеров нет)
- T0-CLOCK работает: при искусственном дрифте (тест через mock-NTP) kill-switch срабатывает
- T0-NEWS: при наличии `news_calendar.json` — загружен, hot-reload работает
- zero ASan/UBSan findings
- `ctest` — весь набор зелёный

---

## Фаза 1 — Market Data & Feature Extractor

### T1-ORDERBOOK: Локальная реплика стакана

**Depends on:** T0-CODEC, T0-FEED

**Deliverables:**
- `src/marketdata/OrderBook.{hpp,cpp}` (согласно `ARCHITECTURE.md § 2.3`)
- `OrderBook::apply_snapshot`, `apply_update`, `best_bid()`, `best_ask()`, `spread()`, `mid()`, `depth(int n_levels)`, `volume_at_range(double lo, double hi)`
- Внутри — `absl::btree_map<PriceTick, SizeFix, std::greater<>>` (bids) и `absl::btree_map<PriceTick, SizeFix>` (asks). Ключ — **fixed-point** PriceTick из T0-NUMERIC, не double (см. ARCH § 2.3, § 11.3).
- Cache-friendly B-tree layout, `reserve(book_capacity_levels=128)` на старте.
- Удаление уровня при `size == 0`.
- `best_bid()`, `best_ask()` — O(1) через cached top-of-book (обновляется только при изменении первого уровня).
- `depth(N)` для N=10 — через `simd_sum_double` (T0-PERF).
- `apply_update_batch` для batched обновления ≥ 8 уровней — через `std::execution::par_unseq`.
- `noexcept` на всех публичных методах, zero allocations на hot path (см. ARCH § 10.1).
- **Sanity check:** раз в `orderbook_sanity_check_sec` (default 30) переподписка → сверка top-20 уровней → при расхождении > 3 — WARN + full resync.

**Depends on:** T0-CODEC, T0-FEED, T0-NUMERIC, T0-PERF

**Acceptance criteria:**
- `test/unit/orderbook_test.cpp`:
  - applySnapshot → best_bid/ask/spread возвращают ожидаемое
  - applyUpdate добавляет/изменяет/удаляет уровни
  - depth(10) корректна
  - стресс-тест: 100k случайных апдейтов без роста памяти выше 2× после ~5 секунд (нет утечек в btree)
  - sanity check: симулируем gap (3 пропущенных update) → sanity detect + resync
  - PriceTick round-trip (`from_price`/`to_price`) точен до `PriceIncrement`
- `bench/bench_orderbook_apply.cpp`: **p50 < 50 µs, p99 < 200 µs** на 100k mixed updates (см. SLO ARCH § 3.1) — gate в CI
- `bench/bench_orderbook_depth.cpp`: `depth(10)` SIMD ≥ 3× быстрее scalar fallback на AVX2

**Out of scope:** детекторы (это фаза 2).

---

### T1-TRADESTREAM: Поток сделок и агрегаты (incremental algorithms)

**Depends on:** T0-CODEC, T0-FEED, T0-NUMERIC, T0-PERF

**Deliverables:**
- `src/marketdata/TradeStream.{hpp,cpp}` — incremental aggregates (см. ARCH § 2.3):
  - ring-buffer `std::vector<Trade>` фикс. capacity (избегаем `std::deque` page allocations)
  - **WelfordAccumulator** для `avg_print_size`, `stdev_print_size` (T0-NUMERIC)
  - **Hawkes process intensity** `λ(t) = μ + Σ α·exp(-β(t-tᵢ))` для `prints_per_sec` — экспоненциально-затухающая интенсивность с `half_life_sec=2.0` (конфиг `[signals.tape.hawkes]`)
  - **CUSUM accumulator** для `TapeFade` change-point (см. SIGNAL § 3 implementation notes)
  - SIMD prefix-sum (`simd_sum_double` из T0-PERF) для cumulative volume по окнам 1s/5s/30s
  - **t-digest** (per-ticker) для distribution trade-size с online merge — даёт `q99` для адаптивного `flush_min_size_usd` (см. SIGNAL § 3.3)
  - **Kahan summation** для всех volume-аккумуляторов
- `on_trade(Trade)` колбэк для детекторов

**Acceptance criteria:**
- `test/unit/tradestream_welford_test.cpp`: на 1000 trades с известным mean/stdev — погрешность < 0.1%
- `test/unit/tradestream_hawkes_test.cpp`: после burst в 30 trades за 2 сек, `λ(t)` ≥ 3× baseline и спадает экспоненциально к baseline за ~6 сек
- `test/unit/tradestream_tdigest_test.cpp`: на 100k samples из exp distribution, `q99` погрешность < 1%
- `test/unit/tradestream_window_test.cpp`: окна правильно скользят, eviction по deadline
- `bench/bench_tradestream_on_trade.cpp`: **p99 < 2 µs** на trade (см. SIGNAL § 3 hot-path budget)

---

### T1-LEADER: LeaderTracker (Welford correlation + Kalman lag)

**Depends on:** T1-ORDERBOOK, T1-TRADESTREAM, T0-NUMERIC, T0-PERF

**Deliverables:**
- `src/marketdata/LeaderTracker.{hpp,cpp}`
- Для каждого настроенного поводыря (из конфига): свой OrderBook + TradeStream
- **Online Pearson correlation через extended Welford** (Schubert & Gertz 2018) — реализация `WelfordCorrelation` из T0-NUMERIC; численно стабильно, O(1) на каждом sample (см. ARCH § 2.3 LeaderTracker)
- **Lag estimation через Kalman filter** — state = `[lag_ms, lag_drift_ms_per_sec]`, observation = текущий argmax cross-correlation на 10-сек окне (шаг 100 мс):
  - `src/marketdata/KalmanLagEstimator.{hpp,cpp}` — 2-state Kalman, configurable `Q` (process noise) и `R` (observation noise)
  - `confidence` из `P` (covariance) → передаётся в `LeaderMove.confidence`
- **FFT-based cross-correlation** (warmup, 60-сек history): `pocketfft` через conanfile, `IFFT(FFT(x) · conj(FFT(y)))` за O(N log N) — выполняется на старте `LeaderTracker` для каждого тикера ≤ 100 µs
- **CUSUM на корреляции** для раннего обнаружения раскорреляции (early-exit для `LeaderLag § 3.7` invalidation)
- **fallback**: при недостатке данных (warmup < 60 sec) — argmax cross-correlation без Kalman

**Acceptance criteria:**
- `test/unit/leader_correlation_welford_test.cpp`: на синтетике с известным `corr=0.7` — погрешность < 0.001 на 1000 samples; устойчивость к near-equal values (catastrophic cancellation test)
- `test/unit/kalman_lag_estimator_test.cpp`: при медленном дрифте лага 200→250 мс за 10 сек — Kalman трекает с ошибкой < 20 мс; при шумных наблюдениях (σ=50 мс) — output stdev < 30 мс
- `test/unit/leader_test.cpp`: синтетический датасет (альт = BTC * 1.5 со сдвигом 200 мс + шум) → corr > 0.95, estimated_lag_ms в [150, 250], confidence > 0.7
- `test/unit/leader_cusum_decorrelation_test.cpp`: при step-change `corr` 0.8→0.3 за 5 сек — CUSUM эмитит alarm на 300+ мс быстрее наивного `corr < 0.5`
- `bench/bench_leader_on_frame.cpp`: **p99 < 0.5 µs** per feature-frame

---

### T1-FRAME: FeatureExtractor и FeatureFrame

**Depends on:** T1-ORDERBOOK, T1-TRADESTREAM, T1-LEADER, T0-NUMERIC, T0-PERF

**Deliverables:**
- `src/features/FeatureFrame.hpp` (структура как в `ARCHITECTURE.md § 2.4`):
  - Layout — **Struct-of-Arrays** для полей, по которым итерируем (детекторы) — vectorization-friendly (см. ARCH § 10.3)
  - все volume-поля в `int64_t` cents (fixed-point из T0-NUMERIC), конвертация в `double` только при сериализации
- `src/features/FeatureExtractor.{hpp,cpp}` — по таймеру (10 Гц default):
  - все rolling stats — через `WelfordAccumulator` (T0-NUMERIC)
  - `volatility_1min` — Welford-stdev лог-доходностей
  - `imbalance` через `simd_sum` для `bid_depth_10` / `ask_depth_10` (T0-PERF)
  - `LatencyTracer` обёртки для каждой стадии: `book_to_feature_us`, `feature_total_us`
  - hot-path: zero allocations, arena-pmr (см. ARCH § 10.1)
- `src/features/FeatureRecorder.{hpp,cpp}` — опционально пишет фреймы в Apache Arrow / Parquet (через arrow-cpp), не CSV — формат удобнее для backtest и оффлайн-анализа

**Acceptance criteria:**
- `test/unit/feature_extractor_test.cpp`: фрейм содержит все поля
- `test/unit/feature_welford_test.cpp`: `volatility_1min_bps` — погрешность < 0.5% относительно reference numpy.std на синтетике
- `bench/bench_feature_extractor.cpp`: **p99 < 100 µs** на полный фрейм (30 тикеров × 10 Гц = 300 фреймов/сек, бюджет ≤ 3 мс/сек CPU); ≥ 10_000 фреймов/сек (10× запас)
- В integration-тесте на replay: 60 сек × 10 Гц = 600 фреймов, получено ≥ 595 (drop ≤ 1%); HdrHistogram p50/p99 укладываются в SLO ARCH § 3.1

---

### T1-UNIVERSE: TickerUniverse — pool + per-strategy affinity

**Depends on:** T0-GATEWAY, T1-TRADESTREAM

**Deliverables:**
- `src/universe/ConnectionSelector.{hpp,cpp}` — выбирает торговый коннекшен из `GET /api/connections` (фильтр: `State == Connected`, `ViewMode == false`, опционально предпочесть `DemoMode == true` в paper-режиме)
- `src/universe/UniverseFilters.{hpp,cpp}` — glob-pattern matcher + static-фильтр (`[universe.pool]`)
- `src/universe/TickerUniverse.{hpp,cpp}` — реализация модуля из `ARCHITECTURE.md § 2.11`:
  - **Pool layer**: `OrderGateway::get_tickers()` → static-фильтр → warmup на trade-stream → dynamic-фильтр → опубликовать `active()`
  - **Affinity layer**: для каждого тикера и каждой стратегии из конфига вычислить score, эмитить `on_affinity_change` при пересечении threshold
  - `manual_allow` / `manual_deny` (на pool-уровне)
  - `pool_refresh_min` (default 30) — периодический пересмотр
  - `affinity_refresh_sec` (default 60) + триггер по BigTick
  - `is_boosted(ticker)` — true в течение `boost_ttl_sec` после BigTick / BigOrderBookAmount
- `TickerMeta` — кеш `PriceIncrement`, `SizeIncrement`, `MinSize`, `MaxSize` для R8 / Executor

**Acceptance criteria:**
- `test/unit/universe_filters_test.cpp`: glob-паттерны работают (deny `*UP*`/`*BULL*`/`*1000*`, allow `*USDT`)
- `test/unit/ticker_universe_pool_test.cpp` с mock `IOrderGateway`:
  - candidate с volume < `min_volume_24h_usd` → исключается после warmup
  - candidate с avg_spread > `max_avg_spread_bps` → исключается
  - `manual_allow` форс-добавляет, `manual_deny` форс-исключает
  - `max_pool_size = 3` — выбирает топ-3 по volume
- `test/unit/ticker_universe_affinity_test.cpp`:
  - BounceFromDensity: при `min_density_events_per_hour = 4` и подсчитанных 5 — strategy enabled; при 2 — disabled
  - LeaderLag с `require_leader=BTC_USDT` и `min_correlation_60s=0.6`: альт с corr=0.7 — enabled; corr=0.4 — disabled; сам BTC_USDT при `exclude_self_for_leader=true` — disabled
  - переключение состояния при boost-событии не происходит до пересчёта affinity
- `test/unit/connection_selector_test.cpp`: `ViewMode==true` отбрасывается; `State!=Connected` отбрасывается
- `test/integration/universe_smoke_test.cpp` (`--with-metascalp`): pool собирается за ≤ `warmup_sec + 5`, ≥ 1 тикер с affinity ≥ threshold хотя бы для одной стратегии

**Out of scope:**
- screener-уведомления (T1-NOTIF)
- cluster-snapshot polling (T1-CLUSTER)
- REST orderbook snapshot seed / `FetchSnapshot=false` (T1-BOOK-SEED)
- auto-калибровка density.min_size_usd по orderbook-settings (T1-CALIB)

---

### T1-CLUSTER: Cluster-snapshot polling + интеграция с уровнями

**Depends on:** T0-GATEWAY, T1-UNIVERSE

**Deliverables:**
- `src/transport/ClusterSnapshotClient.{hpp,cpp}` — REST: `GET /api/connections/{id}/cluster-snapshot?Ticker&TimeFrame&ZoomIndex`, парсинг через `MetaScalpCodec`
- `src/marketdata/ClusterSnapshot.{hpp,cpp}` — кэш футпринта на тикер × таймфрейм; pollers по `[clusters].poll_interval_sec` для каждого тикера из `universe.active()` × `clusters.poll_timeframes` (`["M5","M15","H1"]`), с `[clusters].poll_jitter_sec` и `[clusters].max_concurrent_requests` как safe-fallback для REST rate limits (см. README OQ-6)
- интеграция: `LevelDetector` подписывается на снапшоты, добавляет «уровни от объёма» (точки `Items[].AskSize+BidSize >= cluster_min_volume_pct * sum(column)`) к собственным уровням от экстремумов

**Acceptance criteria:**
- `test/unit/cluster_snapshot_codec_test.cpp` — фикстура с 10-колоночным ответом парсится корректно
- `test/unit/level_detector_with_clusters_test.cpp`: уровни найденные кластером добавляются с `source = "cluster"`, не дублируют ранее найденные по экстремумам (cluster_tolerance_bps)
- `test/integration/cluster_snapshot_smoke_test.cpp` (`--with-metascalp`): для BTCUSDT за 30 сек получено ≥ 1 снапшот M5 и ≥ 1 снапшот H1

**Out of scope:** запись cluster-снапшотов в replay-дампы (опционально в Фазе 5).

---

### T1-BOOK-SEED: REST orderbook-snapshot + FetchSnapshot=false

**Depends on:** T0-GATEWAY, T0-FEED, T1-UNIVERSE

MetaScalp SDK v1.0.7 добавил two-step модель для массовых подписок:
`orderbook_subscribe` можно отправить с `FetchSnapshot=false`, а начальное
состояние стакана при необходимости получать отдельным REST
`GET /api/connections/{id}/orderbook-snapshot?Ticker=...`.

**Deliverables:**
- `src/transport/OrderBookSnapshotClient.{hpp,cpp}` или метод в существующем
  gateway: REST `GET /api/connections/{id}/orderbook-snapshot?Ticker&ZoomIndex&DepthLevels&DepthPercent`
- codec-парсер response: shape как `orderbook_snapshot` + `UpdateId`
- `MarketDataFeed` режим `[feed].fetch_snapshot_on_subscribe=false`:
  - WS subscribe отправляет `FetchSnapshot=false`
  - seed делается REST snapshot с rate-limit/concurrency guard
  - при REST `501` ticker/exchange помечается как `ws_snapshot_only`, и
    `FetchSnapshot=false` для него больше не используется
- resync path: `force_resync_orderbook()` сначала пытается REST snapshot,
  затем WS resubscribe fallback

**Acceptance criteria:**
- `test/unit/orderbook_snapshot_client_test.cpp`: URL содержит
  `Ticker`, `ZoomIndex`, `DepthLevels`, `DepthPercent`, ответ с `UpdateId`
  парсится в `OrderBookSnapshot`
- `test/unit/feed_test.cpp`: subscribe body содержит `FetchSnapshot=false`
  только при `[feed].fetch_snapshot_on_subscribe=false` и успешном REST seed;
  при `501` следующий subscribe идёт без `FetchSnapshot=false`
- `test/integration/orderbook_snapshot_smoke_test.cpp` (`--with-metascalp`):
  REST snapshot для поддерживаемой биржи применим к локальному `OrderBook`
- `test/integration/mass_subscribe_smoke_test.cpp` (`--with-metascalp`):
  ≥ 50 тикеров подписываются без REST snapshot storm; dropped events = 0

**Out of scope:** изменение торговых правил. Этот тикет только про корректный
и щадящий bootstrap стаканов через MetaScalp API.

---

### T1-NOTIF: Notification subscribe (screener / big-tick / big-amount)

**Depends on:** T0-FEED, T1-UNIVERSE

**Deliverables:**
- `src/transport/NotificationFeed.{hpp,cpp}` — подписка на WS `notification_subscribe`, парсинг `notification_snapshot` / `notification_update` через `MetaScalpCodec`
- доменный enum `NotificationKind { Trade, SignalLevel, BigOrderBookAmount, BigOrderBookAmount2, BigTick, ScreenerNewCoin }`
- **клиентский фильтр только по документированным полям** (см. README OQ-3): `notification_subscribe` app-wide и не содержит `ConnectionId`; события с `ExchangeId != connection.exchange_id`, `MarketType != configured_market_type` или ticker вне allowlist/pool — отбрасываются до роутинга. Счётчик отброшенных — в метриках `notification_dropped_wrong_connection_total`.
- роутинг:
  - `ScreenerNewCoin` → `TickerUniverse::on_screener_new_coin(ticker)` (если `use_screener_notifications=true` — стартует pool-warmup для нового кандидата)
  - `BigTick`, `BigOrderBookAmount`, `BigOrderBookAmount2` → `TickerUniverse::on_big_*(ticker, size_usd)` (boost priority + триггер пересчёта affinity)
  - `BigOrderBookAmount` → опциональный мост в SignalBus при `forward_big_amount_to_signals=true` (по умолчанию выкл — не дублируем DensityDetector)
- модуль выключается флагом `[notifications].subscribe=false`

**Acceptance criteria:**
- `test/unit/notification_codec_test.cpp` — фикстуры на каждый из 6 типов
- `test/unit/notification_routing_test.cpp`: BigTick → `is_boosted(ticker) == true` сразу; через `boost_ttl_sec` → `false`; ScreenerNewCoin неизвестного тикера → корректно стартует pool-warmup в моке universe
- `test/unit/notification_filter_test.cpp`: event с `ExchangeId=99` при `connection.exchange_id=1` и event с чужим `MarketType` — отброшены, счётчик инкрементнут
- `test/integration/notification_smoke_test.cpp` (`--with-metascalp`, опционально): подписка не падает, сообщения парсятся

**Out of scope:** `signal_level_subscribe` (отдельный T3-SIGLEVEL).

---

### T1-CALIB: Auto-калибровка density.min_size_usd через orderbook-settings

**Depends on:** T0-GATEWAY, T1-UNIVERSE

**Deliverables:**
- `src/universe/OrderbookSettingsLoader.{hpp,cpp}` — REST: `GET /api/connections/{id}/orderbook-settings?Ticker=X` (на каждый тикер из `universe.active()` единожды + при попадании в pool)
- интеграция: при `[universe.calibration].use_orderbook_settings = true` `TickerUniverse::density_min_size_usd(ticker)` возвращает `max(config.signals.density.min_size_usd, ob_settings.LargeAmountUsd)`; `DensityDetector` спрашивает универс перед использованием порога

**Acceptance criteria:**
- `test/unit/orderbook_settings_loader_test.cpp`: парсит ответ, корректно определяет `LargeAmountUsd` / `LargeAmountUsd2`
- `test/unit/density_calibration_test.cpp`: при `LargeAmountUsd=200_000` и config-default `50_000` детектор использует `200_000`
- `test/integration/orderbook_settings_smoke_test.cpp` (`--with-metascalp`, опционально)

**Out of scope:** запись настроек обратно (`PUT`) — бот **не меняет** UI оператора.

---

### T1-GATE — Фаза-1 integration gate

**Depends on:** T1-FRAME, T1-UNIVERSE, T1-CLUSTER, T1-NOTIF, T1-CALIB

**Acceptance criteria:**
- на записанном replay-дампе: OrderBook идентичен ожидаемому, FeatureFrame на 10 Гц без пропусков ≥ 0.5 сек
- на живом MetaScalp: TickerUniverse строит pool ≥ 5 тикеров за warmup; для ≥ 1 тикера активна ≥ 1 стратегия; MarketDataFeed подписан **только** на `universe.active()`
- ConnectionSelector корректно отказывается от `ViewMode==true` коннекшенов (тестируем переключением в UI)
- BigTick на тикер X → `is_boosted(X)==true` в течение `boost_ttl_sec`

---

## Фаза 2 — Signal Detectors

### T2-SIGNAL-BASE: SignalBus и базовый интерфейс

**Depends on:** T1-FRAME

**Deliverables:**
- `src/signals/Signal.hpp` (SignalKind enum, Signal struct — см. ARCHITECTURE.md)
- `src/signals/SignalBus.{hpp,cpp}` — `subscribe(std::function<void(const Signal&)>)`, `publish(Signal)`. Single-threaded, sync.
- `src/signals/IDetector.hpp` — базовый интерфейс: `on_frame(FeatureFrame)`, `on_trade(Trade)`, `on_book_update(OrderBookUpdate)`

---

### T2-DENSITY: DensityDetector

**Depends on:** T2-SIGNAL-BASE, T0-NUMERIC

**Deliverables:**
- `src/signals/DensityDetector.{hpp,cpp}` (см. `SIGNAL_DETECTION.md § 1` + Implementation notes):
  - `avg_level_size` через **DEMA** (T0-NUMERIC `Ema`) — реакция в 2× быстрее SMA на сдвиг распределения
  - tracking levels в `absl::flat_hash_map<PriceTick, LevelMeta>` (PriceTick — T0-NUMERIC fixed-point)
  - branchless-update path с `[[likely]]` на стандартный апдейт
  - эмитит `DensityDetected`, `DensityRemoved`, `DensityEating` (sub-detector § 1.1)
- Пороги из конфига секции `[signals.density]`

**Acceptance criteria:**
- `test/unit/density_detector_test.cpp`:
  - появление крупного уровня + 2.5 сек удержания → эмитит `DensityDetected`
  - снятие уровня до sticky_duration → эмитит `DensityRemoved(fake=true)`
  - уровень ближе `min_distance_bps` от mid → игнор
  - серия из 5+ агрессивных принтов, выедающих ≥ 50% уровня за 3 сек → эмитит `DensityEating`
- `bench/bench_density_on_book_update.cpp`: **p99 < 1 µs** (см. SIGNAL § 1 implementation notes)
- Размеченная выборка из T2-LABELING (≥200 положительных, ≥400 отрицательных): **precision ≥ 70%** при Wilson 95%-CI lower bound ≥ 60%, **recall ≥ 50%** (см. `SIGNAL_DETECTION.md § 8`)

---

### T2-ICEBERG: IcebergDetector (Bayesian evidence)

**Depends on:** T2-SIGNAL-BASE

**Deliverables:**
- `IcebergDetector.cpp`, секция `[signals.iceberg]`:
  - **Bayesian posterior** `P(iceberg | evidence)` обновляется по каждому refill-событию (см. SIGNAL § 2 implementation notes)
  - `iceberg_posterior_threshold` (default **0.80**) — эмит при posterior ≥ threshold
  - fallback на legacy hard-counter (`evidence_count ≥ 3`) если posterior threshold отсутствует в конфиге
  - event-join window 100 мс через ring-buffer last-100-events на тикер для синхронизации trade ↔ orderbook_update

**Acceptance criteria:**
- `test/unit/iceberg_bayesian_test.cpp`: prior=0.05, после 4 strong-evidence refill — posterior ≥ 0.80; после 1 weak — posterior < 0.30
- unit-тесты по фикстурам и прогон на T2-LABELING выборке с **precision ≥ 70% (Wilson lower ≥ 60%) и recall ≥ 50%**

---

### T2-TAPE: TapeAnalyzer (Hawkes + CUSUM + t-digest)

**Depends on:** T2-SIGNAL-BASE, T1-TRADESTREAM

**Deliverables:**
- `TapeAnalyzer.cpp` реализует 4 sub-detectors:
  - **TapeBurst** через Hawkes intensity (из `T1-TRADESTREAM`): `λ_buy(t) / λ_sell(t) >= burst_ratio` И `λ_total >= k·μ`
  - **TapeFade** через CUSUM (Page 1954) — раннее детектирование на 200–500 мс быстрее наивного порога
  - **TapeFlush** — outlier через t-digest q99 (адаптивно к ликвидности)
  - **Distribution** — узкий диапазон через Welford-stdev цены
- Секция `[signals.tape]` + sub-секции `[signals.tape.hawkes]`, `[signals.tape.cusum]`

**Acceptance criteria:**
- `test/unit/tape_burst_hawkes_test.cpp`: synthetic burst 30 trades / 2 сек → emit на 4-м trade'е (latency ≤ 500 мс)
- `test/unit/tape_fade_cusum_test.cpp`: при step-change rate 20→2 prints/sec — alarm на 200+ мс быстрее наивного fade-порога
- `test/unit/tape_flush_tdigest_test.cpp`: outlier print 30× медианы — `TapeFlush` эмитится; обычный large print (5×) — нет
- `bench/bench_tape_on_trade.cpp`: **p99 < 2 µs** (см. SIGNAL § 3)
- T2-LABELING выборка: **precision ≥ 70% (Wilson lower ≥ 60%) и recall ≥ 50%** на каждый sub-detector

---

### T2-LEVEL: LevelDetector (DBSCAN + KDE)

**Depends on:** T2-SIGNAL-BASE, T1-CLUSTER, T0-PERF

**Deliverables:**
- `LevelDetector.cpp` — кластеризация уровней:
  - **DBSCAN** для extremes (`eps = cluster_tolerance_bps`, `min_pts = 2` → `touches_min`) — реализация через `mlpack::dbscan` или собственная компактная версия
  - **KDE** (Gaussian kernel, Silverman bandwidth) на cluster-snapshot M15/H1 — локальные максимумы плотности → kde-уровни; SIMD-ускорение через `xsimd` (T0-PERF), цель ≤ 50 µs на 200×30 точек
  - **Confidence score** = `0.4·touches + 0.4·kde_score + 0.2·recency_score` ∈ [0,1] — в `Signal.payload`
  - адаптивный `min_reversal_bps` через DEMA от историч. волатильности (T0-NUMERIC)
  - секция `[signals.level]` + `[signals.level.kde]`

**Acceptance criteria:**
- `test/unit/level_dbscan_test.cpp`: 100 синтетических экстремумов с 3 кластерами → DBSCAN находит ровно 3 кластера, noise отброшен
- `test/unit/level_kde_test.cpp`: на synthetic volume profile с 2 пиками — KDE возвращает 2 локальных максимума с правильными ценами (±5 bps)
- `bench/bench_level_kde.cpp`: ≤ 50 µs на 200×30 точках (cluster-snapshot batch)
- T2-LABELING: **precision ≥ 70% (Wilson lower ≥ 60%) и recall ≥ 50%**

---

### T2-APPROACH: ApproachAnalyzer (3-state HMM)

**Depends on:** T2-SIGNAL-BASE, T2-LABELING

**Deliverables:**
- `ApproachAnalyzer.cpp` — классификатор impulse/slow/consolidation:
  - **3-state HMM** (см. SIGNAL § 5 implementation notes): observations `[speed, pullbacks, dist]`, Gaussian-mixture emission, fixed transition matrix
  - **Forward algorithm** O(N·S²) на 30-сек окне → posterior probabilities → argmax
  - emission параметры обучаются раз в неделю на labeled events из T2-LABELING (Baum-Welch / EM); training-tool отдельный (`tool/hmm_train/`)
  - **ZigZag** для pullback detection (`pullback_min_bps=5`)
  - **OLS regression** для оценки speed (Welford-based)
  - fallback на if-else пороги при отсутствии training data
  - секция `[signals.approach]` + `[signals.approach.hmm]`

**Acceptance criteria:**
- `test/unit/approach_hmm_forward_test.cpp`: на synthetic impulse trace HMM выдаёт `Impulse` posterior > 0.7; на slow trace — `Slow` > 0.7
- `test/unit/approach_zigzag_test.cpp`: на noisy цене с 3 pullbacks ≥ 5 bps — `num_pullbacks=3`, мелкий шум 1 bps игнорируется
- `bench/bench_approach_on_frame.cpp`: ≤ 50 µs на feature-frame
- T2-LABELING: **precision ≥ 70% (Wilson lower ≥ 60%) и recall ≥ 50%** для каждого из 3 классов

---

### T2-LEADER: LeaderSignal

**Depends on:** T2-SIGNAL-BASE, T1-LEADER

**Deliverables:**
- `LeaderSignal.cpp` — использует Welford-correlation и Kalman-lag из `T1-LEADER`:
  - эмитит `LeaderMove` с `confidence` из Kalman covariance
  - **CUSUM на корреляции** для early-exit инвалидации (см. SIGNAL § 6 implementation notes)
  - секция `[signals.leader]`

**Acceptance criteria:**
- `test/unit/leader_signal_test.cpp`: на synthetic alt = btc·1.5·shifted(200ms), corr=0.8 — `LeaderMove.confidence > 0.7`, направление совпадает
- `test/unit/leader_decorrelation_cusum_test.cpp`: при step-change corr 0.8→0.3 — `LeaderDecorrelated` event на 300+ мс быстрее наивного fence
- T2-LABELING: **precision ≥ 70% (Wilson lower ≥ 60%) и recall ≥ 50%**

---

### T2-LABELING: Сбор размеченной выборки (≥200 положительных на детектор)

**Depends on:** T1-GATE, T2-SIGNAL-BASE

**Deliverables:**
- `tool/labeler/` — интерактивный инструмент разметки:
  - читает replay-дамп, проигрывает в ускоренном режиме, отображает candidate-events (детектор-pre-run с low thresholds)
  - оператор через CLI отмечает каждое событие как `true_positive` / `false_positive` / `skip`
  - также оператор может добавить **positive-событие, которое детектор пропустил** (для подсчёта recall)
  - сохраняет в `replay/labels/<ticker>_<detector>_labels.jsonl` формата:
    ```json
    {"timestamp_ns": 1234567890, "signal_kind": "DensityDetected", "is_true_positive": true, "evidence_notes": "clear 12x level, held 8 sec"}
    ```
- **целевые объёмы** (обязательно до T2-BACKTEST):
  - Density: ≥200 положительных, ≥400 отрицательных
  - Iceberg: ≥200 / ≥400
  - Tape (каждый sub-detector отдельно): ≥200 / ≥400
  - Level: ≥200 / ≥400
  - Approach: ≥200 / ≥400
  - Leader: ≥200 / ≥400
- `replay/labels/README.md` — методология разметки, чтобы разные операторы метили consistently

**Acceptance criteria:**
- все JSONL-файлы валидны по schema (`replay/labels/schema.json`)
- объёмы достигнуты (автоматический отчёт `tool/labeler/report.sh`)
- inter-rater reliability: 10% выборки размечены повторно другим оператором, Cohen's kappa ≥ 0.7

**Почему это отдельный тикет:** сбор 1200+ положительных событий на 6
детекторов — недели работы, нельзя блокировать этим T2-BACKTEST. Разметка
может идти параллельно с реализацией детекторов.

---

### T2-BACKTEST: Harness для offline-валидации

**Depends on:** все T2-*, T2-LABELING

**Deliverables:**
- `tool/backtest/signal_validator.cpp` — читает JSONL-разметку из T2-LABELING, прогоняет replay-дамп через все детекторы, считает precision/recall с Wilson 95% CI, выдаёт отчёт
- `replay/labels/btcusdt_labels.jsonl` — собран в T2-LABELING

**Acceptance criteria:**
- Все 6 детекторов имеют precision ≥ 70%, recall ≥ 50% (с CI lower bound ≥ 60% для precision) на валидационной выборке

---

### T2-GATE

Все детекторы работают онлайн на живом MetaScalp, события идут в SignalBus
с задержкой от book-update до эмита < 5 мс (measured), 60 минут работы —
нет утечек памяти, нет варнингов.

---

## Фаза 3 — Strategy Engine

### T3-PLAN: TradePlan + IStrategy + StrategyEngine

**Depends on:** T2-*

**Deliverables:**
- `src/strategy/TradePlan.hpp` (структура как в `ARCHITECTURE.md § 2.6`)
- `src/strategy/IStrategy.hpp`
- `src/strategy/StrategyEngine.{hpp,cpp}` — держит список IStrategy, раздаёт им FeatureFrame и Signal, собирает TradePlan-ы и отправляет в RiskManager (stub в этой задаче)
- `StrategyContext` — вспомогательная структура с текущим FeatureFrame и активными сигналами за последние N секунд

---

### T3-BOUNCE: BounceFromDensity strategy

**Depends on:** T3-PLAN

**Deliverables:**
- `src/strategy/BounceFromDensity.{hpp,cpp}` — реализация всех условий C1..C6 из `STRATEGIES.md § 1`
- Все пороги из конфига `[strategies.bounce]`
- Инвалидация до/после входа (§ 1.6, 1.7)

**Acceptance criteria:**
- `test/unit/bounce_strategy_test.cpp`:
  - сценарий: плотность на ask, импульсный подход, tape fade → порождает Long-план с корректным stop/tp1/tp2
  - сценарий: плотность снята до входа → план инвалидируется, ордер отменяется
  - сценарий: leader двинулся против → план не генерируется

---

### T3-BREAKOUT: BreakoutEatThrough strategy

**Depends on:** T3-PLAN

**Deliverables:**
- `src/strategy/BreakoutEatThrough.{hpp,cpp}` — реализация всех условий C* из `STRATEGIES.md § 2`
- все пороги из секции конфига `[strategies.breakout]`
- unit-тесты на happy path, каждую инвалидацию, каждый граничный случай

---

### T3-LEADERLAG: LeaderLag strategy

**Depends on:** T3-PLAN

**Status:** `gated` (FN-004). The source-backed leader/follower lag pattern may
run only with explicit leader mapping, fresh streams, correlation/staleness
checks, and density-on-path rejection. Spot/futures dislocation and
robot/density-release variants are `phase-later` until instrument identity and
multi-feed replay fixtures exist.

**Deliverables:**
- `src/strategy/LeaderLag.{hpp,cpp}` — реализация всех условий C* из `STRATEGIES.md § 3`
- все пороги из секции конфига `[strategies.leaderlag]`
- unit-тесты на happy path, low correlation, stale `LeaderMove`, density-on-path,
  direction/sign handling, and post-entry leader/correlation invalidations
- spec/status consistency test asserting `LeaderLag` remains explicitly `gated`

---

### T3-SIGLEVEL: Серверные signal-levels от MetaScalp

**Depends on:** T0-GATEWAY, T0-FEED, T2-LEVEL

**Deliverables:**
- `src/transport/SignalLevelGateway.{hpp,cpp}` — REST: `GET /api/connections/{id}/signal-levels?Ticker=...`, `POST /api/connections/{id}/signal-levels` с body `{Ticker, Price}`, `DELETE /api/connections/{id}/signal-levels/{id}`, `DELETE /api/connections/{id}/signal-levels?Ticker=...`, `DELETE /api/signal-levels/triggered` (periodic cleanup)
- `src/signals/SignalLevelBridge.{hpp,cpp}`:
  - перед create убеждается, что `orderbook_subscribe` для тикера активен: API требует market data, иначе `POST /api/connections/{id}/signal-levels` вернёт `No market data...`
  - через `NotificationFeed` подписывается на WS `signal_level_subscribe` и парсит `signal_levels_snapshot`, `signal_level_placed`, `signal_level_triggered`, `signal_level_removed`, `signal_levels_removed_all`, `signal_levels_removed_triggered`
  - подписывается на `LevelFormed` от `LevelDetector`
  - для каждого уровня вызывает `SignalLevelGateway::create(ticker, price)` — MetaScalp сам определяет `TriggerRule` от текущего best ask
  - при `signal_level_triggered` от WS `signal_level_subscribe` — немедленно эмитит `LevelBreak` в SignalBus с `payload.source="server"` (без polling)
  - при `signal_level_removed` / `signal_levels_removed_all` / `signal_levels_removed_triggered` синхронизирует локальную карту уровней bridge без REST polling
  - periodic cleanup: раз в час `DELETE /api/signal-levels/triggered` чтобы не накапливать мусор
- конфиг `[signals.level_bridge]`: `enabled=true`, `signal_level_subscribe=true`, `cleanup_interval_min=60`, `max_server_levels=50`
- **LRU-eviction:** при достижении `max_server_levels` удалять самые
  старые и самые далёкие от текущей цены signal-levels перед
  созданием новых (приоритет удаления: triggered > дальние > старые)

**Acceptance criteria:**
- `test/unit/signal_level_bridge_test.cpp` с mock-gateway:
  - `LevelFormed` → вызов `create` с правильными параметрами
  - `signal_level_triggered` event с matching id → эмит `LevelBreak` в SignalBus
- `test/unit/notification_routing_test.cpp`:
  - при наличии `SignalLevelBridge` отправляется `signal_level_subscribe`
  - direct SDK event `signal_level_triggered` → `LevelBreak` в `SignalBus`
- `test/integration/signal_level_smoke_test.cpp` (`--with-metascalp`): создали уровень, выставили цену, триггер пришёл за <100 мс

**Зачем:** устраняет client-side polling уровней в `LevelApproach` (оценка
−20% CPU у `LevelDetector` при 30+ тикеров в pool). Event-driven реакция
быстрее polling на 50-200 мс.

**Out of scope:** отключение fallback client-side polling — он остаётся как
backup при `enabled=false`, параллельно с server-side не запускаем (дубль event'ов).

---

### T3-PAPER: Paper-Executor

**Depends on:** T3-PLAN

**Deliverables:**
- `src/executor/PaperExecutor.{hpp,cpp}` — реализует `IExecutor` интерфейс
  - принимает TradePlan, эмулирует исполнение по best_bid/best_ask из OrderBook (мгновенный fill для Market, limit fill когда best проходит цену)
  - эмулирует slippage: `actual_fill_price = plan.entry_price ± slippage_bps` (из конфига, default 1 bps)
  - отслеживает виртуальную позицию, PnL, вызывает стопы/TP по плану
  - пишет в TradeJournal каждую сделку с полной evidence

---

### T3-JOURNAL: TradeJournal

**Depends on:** T0-LOG, T3-PAPER

**Deliverables:**
- `src/logger/TradeJournal.{hpp,cpp}` — JSONL формат
- Один файл на день `journal/YYYY-MM-DD.jsonl`
- Запись в отдельном потоке через lock-free очередь
- Каждая запись: TradePlan + все Signals + FeatureFrame snapshot + результат (PnL, holding time, cause_of_exit)

---

### T3-GATE

48 часов paper-trading на живом MetaScalp без крешей, все сделки в журнале
с полной трассировкой. Отдельный отчёт: число сделок, win rate, avg R.
T3-SIGLEVEL включён и работает (есть события `LevelBreak` с
`payload.source="server"` от `signal_level_triggered`).

---

## Фаза 4 — Live Executor & Risk

### T4-RISK: RiskManager

**Depends on:** T3-PLAN, T0-NEWS, T4-FUNDING, T4-FINRES, T1-UNIVERSE

**Deliverables:**
- `src/risk/RiskManager.{hpp,cpp}` — pre-trade правила **R1..R13** и **R15** из `RISK_MANAGEMENT.md § 2`; **R14** выполняется runtime в executor/position monitor как hard market-close
- `src/risk/AccountState.{hpp,cpp}` — обновляется по balance_update/position_update/**finres_update** (realized_pnl — из finres, не self-tracking)
- `src/risk/TradingDay.{hpp,cpp}` — UTC-ресет через cron-коллбек, **идемпотентный при рестарте** (см. RISK § 5): читает `journal/account_state.json`, проверяет `last_reset_day_utc`
- `src/risk/AccountStatePersister.{hpp,cpp}` — state-persist thread (см. ARCH § 3), атомарная запись через tmp+rename, интервал `state_persist.interval_sec`

**Acceptance criteria:**
- `test/unit/risk_manager_test.cpp`: по одному позитивному и негативному тесту на pre-trade правила R1..R13 и R15
- `test/unit/r13_funding_test.cpp`: в пределах blackout-окна (±30 сек от funding) — reject; вне окна — pass; при stale funding-feed — WARN + pass (fail-open); при kill-stale — reject (fail-closed)
- `test/unit/r14_single_position_loss_test.cpp`: runtime monitor закрывает позицию market-ордером при превышении `max_single_position_loss_pct`
- `test/unit/trading_day_reset_idempotent_test.cpp`: рестарт после UTC 00:00 — ресет не повторяется; рестарт до 00:00 — ресет происходит при наступлении часа X
- `test/integration/risk_flow_test.cpp`: симуляция дня с серией убытков, проверка моржа и block'а новых планов
- `test/integration/state_persistence_test.cpp`: крэш симулируется kill -9, после рестарта все active_trades восстановлены, AccountState консистентен

---

### [COMPLETED] T4-EXECUTOR: LiveExecutor

**Depends on:** T4-RISK, T0-GATEWAY, T4-RECOVERY

**Deliverables:**
- `src/executor/LiveExecutor.{hpp,cpp}`:
  - принимает одобренный TradePlan
  - размещает entry-ордер через `OrderGateway.place_order`
  - **ambiguous submit handling** (см. README OQ-1, ARCH § 2.8, METASCALP_API_CONTRACT § 3): если HTTP failure произошёл до отправки body — retry разрешён; если timeout/5xx после отправки body или статус неизвестен — проверяется local recent `order_update`, затем `GET /api/connections/{id}/orders?Ticker=...`; при совпадении используется найденный OrderId, при отсутствии совпадения live НЕ делает второй POST, а переводит сделку в `SubmitUnknown`, ставит ticker на pause и требует recovery/operator ack
  - **state-machine** (см. ARCH § 2.8): `PendingEntry → Open → Exiting → Closed`, с явными `Cancelling` и `SubmitUnknown` для разрешения race cancel ↔ order_update и ambiguous POST
  - **истина — подтверждённые `order_update` / `position_update` + REST reconciliation**, локальный cancel-оптимизм НЕ источник истины; не использовать несуществующий `position_update.ServerTime`
  - при filling — размещает Stop и TakeProfit отдельными ордерами на бирже
  - **server stop contract test (см. README OQ-2)**: до live проверить на demo, что документированные `Type=Stop/StopLoss/TakeProfit` безопасно ставятся через `POST /api/connections/{id}/orders` с `Price` как trigger price и без request-поля `TriggerPrice`; если не подтверждено — live блокируется, soft-stop разрешён только в paper/test
  - JSON body для `POST /orders` содержит только поля SDK v1.0.7: `Ticker`, `Side`, `Price`, `Size`, `Type`, `ReduceOnly`. `ClientId`, `ClientOrderId` и другие idempotency fields не отправляются.
  - при срабатывании TP1 — частичное закрытие + перевод стопа в БУ по `AvgPriceFix` (не по текущей цене)
  - **локальный aggregate `balance_reservation`** (см. README OQ-4): резервирует маржу сразу после отправки order; API не даёт `OrderId`/reservation id в `balance_update`, поэтому резерв снимается/пересчитывается при reject/cancel, подтверждённом `order_update`, следующем `balance_update` по connection или `reservation_timeout_ms=1000`
  - retries с backoff на сетевые ошибки (3 попытки), circuit breaker после `exchange_error_streak` ошибок подряд

**Acceptance criteria:**
- `test/unit/executor_state_machine_test.cpp`: все переходы из ARCH § 2.8, включая race `cancel → order_update(Closed, size>0)` → `Open`
- `test/unit/executor_ambiguous_submit_test.cpp`: после timeout-after-send и matching order найден в `GET /api/connections/{id}/orders?Ticker=...` — повторный POST не отправляется; после timeout-after-send и no-match — live переходит в `SubmitUnknown` и тоже не отправляет второй POST
- `test/unit/executor_balance_reservation_test.cpp`: два последовательных place_order с интервалом 100 мс — R9 проверяет с резервом, не с stale balance
- `test/unit/executor_balance_reservation_timeout_test.cpp`: если `BalanceUpdate` не пришёл за 1 сек, резерв освобождается и событие логируется WARN
- `test/integration/executor_live_smoke_test.cpp` (`--with-metascalp --demo`): круг — entry/stop/tp1/tp2/be-stop — все ордера размещены и отслежены

---

### T4-EXTERNAL: ExternalFeedRegistry + базовый фреймворк

**Depends on:** T0-HTTP, T0-WS

**Deliverables:**
- `src/transport/external/IExternalFeed.hpp` — интерфейс (`last_value()`, `last_update_ts()`, `is_stale(max_age_sec)`)
- `src/transport/external/ExternalFeedRegistry.{hpp,cpp}` — регистрирует активные feed'ы (по `[external_feeds.*].enabled=true`), выдаёт их потребителям по `FeedKind`
- `src/transport/external/FeedStalenessMonitor.{hpp,cpp}` — периодический чек `is_stale()` для всех зарегистрированных feed'ов; при staleness_warn — WARN; при staleness_kill — триггер kill-switch (если `external_feeds.<name>.kill_on_staleness=true`)
- **External-IO thread** (см. ARCH § 3) — отдельный `asio::io_context` для всех внешних клиентов
- `external_ticker_map` в конфиге: MetaScalp-тикер → provider-symbol (обязательно явный маппинг)

**Acceptance criteria:**
- `test/unit/external_feed_registry_test.cpp`: enabled=false feed не регистрируется; два feed'а одного kind — ParseError на старте (ambiguous)
- `test/unit/staleness_monitor_test.cpp`: feed возвращает stale_age=200 сек при `staleness_warn_sec=120` → WARN; при 1000 сек и `kill_on_staleness=true` → kill-switch trigger

---

### T4-FUNDING: Binance funding rate feed + R13 интеграция

**Depends on:** T4-EXTERNAL, T4-RISK

**Deliverables:**
- `src/transport/external/BinanceFundingClient.{hpp,cpp}` — REST `GET /fapi/v1/premiumIndex` polling (интервал из конфига, default 60 сек)
- возвращает для каждого тикера: `funding_rate`, `next_funding_time_utc`
- реализует `IExternalFeed`
- rate-limiting: Binance weight 1, лимит 1200/min — polling 60 тикеров каждую минуту укладывается
- R13 (RiskManager) использует `feed.last_value(ticker).next_funding_time_utc` для расчёта `blackout_window`; staleness берётся из `external_feeds.funding.staleness_warn_sec`, `external_feeds.funding.staleness_kill_sec`, `external_feeds.funding.kill_on_staleness`

**Acceptance criteria:**
- `test/unit/binance_funding_client_test.cpp` с httpmock: фикстурный JSON парсится корректно
- `test/unit/r13_funding_integration_test.cpp`: в окне ±30 сек от `next_funding_time` — plan rejected
- `test/integration/funding_smoke_test.cpp` (опционально, требует интернет): реальный вызов Binance, парсинг, ≥1 тикер в feed'е за 120 сек

---

### T4-FINRES: finres_update WS handling

**Depends on:** T0-CODEC, T4-RISK

**Deliverables:**
- `src/transport/FinresHandler.{hpp,cpp}` — обработка `finres_update`, который приходит из обычного WS `subscribe` по `ConnectionId`; отдельного `finres_subscribe` в MetaScalp API docs нет
- `AccountState.realized_pnl_today_usd` обновляется **только** из finres_update (источник истины): `Finreses[].Result` в API — PnL since connection initialized, поэтому дневной PnL считается как `current_result - finres_day_start_result_usd`, а не суммой update'ов
- baseline `finres_day_start_result_usd` фиксируется при `on_new_trading_day()` и persist'ится в `journal/account_state.json`
- если после успешного connection-level `subscribed` не приходит `finres_update` в течение `finres_ready_timeout_sec`, live trading блокируется: self-tracking PnL не считается источником истины для риска

**Acceptance criteria:**
- `test/unit/finres_handler_test.cpp`: baseline `Result=100`, затем updates `120` и `115` → `realized_pnl_today_usd` равен `20`, затем `15`, без суммирования событий
- `test/unit/finres_ready_timeout_test.cpp`: нет `finres_update` после subscribe → live trading blocked + WARN
- `test/integration/finres_smoke_test.cpp` (`--with-metascalp --demo`): после 1 paper-trade realized_pnl из finres совпадает с self-tracked с точностью до комиссии

---

### [COMPLETED] T4-RECOVERY: Startup recovery (open positions + orders)

**Depends on:** T0-GATEWAY, T4-EXECUTOR (частично — интерфейс `ActiveTrade`)

**Deliverables:**
- `src/executor/StartupRecovery.{hpp,cpp}`:
  - на старте читает `journal/account_state.json` (последний persisted снапшот, см. RISK § 5)
  - выполняет `GET /api/connections/{id}/positions` один раз на connection и `GET /api/connections/{id}/orders?Ticker=...` для каждого тикера из persisted active_tickers + static whitelist; `Status=Open` не отправлять, API его не документирует
  - **reconciliation**:
    - для каждой найденной позиции, matching persisted ActiveTrade — восстановить `state=Recovered`
    - для позиций без matching persisted — создать «orphan» ActiveTrade с консервативным emergency-stop на `entry_price ± max_recovery_stop_bps` (default 30 bps)
    - для ордеров-сирот (нет matching позиции и не-entry) — отменить через `POST /api/connections/{id}/orders/cancel`
  - все recovery-события пишутся в отдельный `journal/recovery.YYYY-MM-DD.jsonl`
  - если recovery обнаружил рассогласование > `position_drift_coin` на ≥ 1 тикере — **не стартуем автоторговлю**, ждём ручного подтверждения через файл `./recovery-ack`
- конфиг `[recovery]`: `max_recovery_stop_bps=30`, `auto_ack_on_clean=true` (если всё consistent — стартуем без `recovery-ack`), `orphan_cancel_policy="cancel"`

**Acceptance criteria:**
- `test/unit/startup_recovery_test.cpp` с mock gateway + mock persisted state:
  - clean case: persisted state совпадает с сервером → `auto_ack=true` → торговля стартует
  - drift case: на сервере лишняя позиция → auto-ack=false, ждёт recovery-ack
  - orphan-order case: ордер без позиции → отменён
  - no-stop case: позиция без стопа → выставлен emergency-stop на 30 bps
- `test/integration/recovery_smoke_test.cpp` (`--with-metascalp --demo`): paper-trade, kill -9 бота, restart → позиция восстановлена, стоп/TP на месте

**Почему это блокер перед Фазой 4:** без recovery любой рестарт (плановый
или аварийный) оставляет открытую позицию без управления — прямой риск
уезда в максимальный убыток.

---

### [COMPLETED] T4-DASHBOARD: Минимальный дашборд

**Depends on:** T4-EXECUTOR

**Deliverables:**
- `core/src/control/DashboardServer.{hpp,cpp}` — встроенный Web UI (HTML + WebSocket)
- Показывает: PnL, позиции, последние 20 сделок, статус KillSwitch, счетчики сигналов
- Доступен по адресу `http://localhost:8080`

---

### T4-GATE

Неделя непрерывной работы на демо-счёте MetaScalp без over-risk events,
автоматический отчёт (из TradeJournal) по итогам каждой сессии.
**Обязательно:** минимум 2 симуляции WS-потери (принудительный kill WS-клиента)
с открытой позицией — бот должен выполнить WS-loss recovery без потери
> `ws_loss_soft_stop_bps`.

---

### T4-STOP-CONTRACT: Контракт-тест серверных стопов

**Depends on:** T0-GATEWAY

**Блокер для live.** Без прохождения live-торговля запрещена.

**Deliverables:**
- `test/contract/stop_contract_test.cpp` (`--with-metascalp --demo`):
  1. Выставить `Type=Stop` (1), `Type=StopLoss` (2), `Type=TakeProfit` (3)
     через `POST /api/connections/{id}/orders` с `Price=X`
  2. Проверить `GET /api/connections/{id}/orders?Ticker=...`: поле
     `TriggerPrice` совпадает с `X`
  3. Дождаться WS `order_update` соответствующего типа
  4. Спровоцировать trigger (двинуть цену на demo), убедиться что
     ордер сработал и позиция закрылась
- `config/contract_test_results.json` — результаты, читаются при
  старте live-модуля; если не пройдены — отказ в запуске

**Acceptance criteria:**
- Все 3 типа стопов корректно выставляются и срабатывают
- TriggerPrice в REST response совпадает с отправленным Price
- Если хотя бы один тип не работает — live блокируется, требуется
  альтернативный план (описывается в отчёте)

---

### [COMPLETED] T4-METRICS: Observability и мониторинг

**Depends on:** T4-EXECUTOR

**Deliverables:**
- `core/src/metrics/MetricsExporter.{hpp,cpp}` — Prometheus-совместимый HTTP endpoint (`/metrics`, port 9090)
- `core/src/metrics/MetricsRegistry.{hpp,cpp}` — хранилище метрик (PnL, сделки, сигналы, латентность)
- `core/src/metrics/AlertWebhook.{hpp,cpp}` — POST webhook для алертов (Telegram/Discord)
- Интеграция во все ключевые модули (LiveExecutor, BeastWsClient, main loop)

**Acceptance criteria:**
- `curl localhost:9090/metrics` возвращает валидный Prometheus exposition format
- после 1 trade — `trade_bot_trades_total` > 0
- после kill-switch — `trade_bot_killswitch_triggered` = 1

---

## Фаза 5 — Оптимизация

### T5-BACKTEST: Исторический backtest engine

Backtest-движок использует тот же StrategyEngine с ReplayFeed вместо LiveFeed,
поэтому код не дублируется.

---

### T5-GRID: Grid search параметров

Калибровка thresholds по историческим данным после Фазы 4.

---

### T5-FLUSH: FlushReversal + LiquidationDetector

**Status:** current `FlushReversal` is `gated` for paper/offline replay; live is
`phase-later` until this ticket is complete (FN-004). `allow_live=true` alone is
not sufficient for live-grade approval.

Доведение FlushReversal до production-качества с liquidations/open-interest
подтверждением из external feeds.

**Acceptance additions:**
- `LiquidationFlush` detector and open-interest/history confirmations are wired
  into the strategy live gate.
- Plain `TapeFlush` cannot satisfy live mode; automated tests prove live plans
  require liquidation/OI evidence.
- Config and docs keep `strategies.flushreversal.allow_live=false` as the safe
  default until all live gates are implemented and tested.

---

### T5-WALKFORWARD: Walk-forward validation

Проверка устойчивости параметров на последовательных out-of-sample окнах.

---

## Приложение A — Шаблон PR для исполнителя

```
## Тикет: T<X>-<CODE>

## Что сделано
- ...

## Файлы
- src/...
- test/...

## Тесты
- [ ] unit-тесты покрывают все публичные методы
- [ ] нет warning-ов (-Wall -Wextra -Wpedantic -Werror)
- [ ] ASan/UBSan debug-build чист
- [ ] clang-tidy: 0 новых замечаний
- [ ] все пороги вынесены в config.toml

## Ссылки
- ARCHITECTURE.md § ...
- SIGNAL_DETECTION.md § ...
- RISK_MANAGEMENT.md § ...
```

## Приложение B — Инвалидированные сценарии для AI-исполнителя

- Захардкоженное число в коде вместо `Config::get`
- Любая прямая работа с `nlohmann::json` выше `src/transport/`
- Использование `std::any` или приведение `reinterpret_cast`
- Игнорирование ошибок (`catch (...) {}`)
- Удаление тестов без замены
- Коммит с неверным форматированием (clang-format)
- Коммит с warning-ами

Любой из этих пунктов — отказ PR.
