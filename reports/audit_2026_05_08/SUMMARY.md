# Отчет по проекту: trade-bot-hardcore-spec

**Дата:** 8 мая 2026 г.
**Статус:** Текущий — Аудит и исправления (Ongoing)

## 📝 Общая информация о проекте

**trade-bot-hardcore-spec** — это высокопроизводительный торговый бот на C++, оптимизированный для HFT-скальпинга.

- **Ключевые технологии:** SIMD (xsimd), Lock-free очереди, Arena аллокаторы, FixedPoint математика, Boost.Asio, absl::btree_map.
- **Архитектура:** Модульная система с четким разделением на `marketdata` (сбор данных), `signals` (детектирование паттернов), `strategy` (логика принятия решений) и `executor` (исполнение ордеров).

## 🔄 Методология работы (Workflow)

1. **Review:** Запуск статических анализаторов (Cppcheck, Clang-Tidy) для выявления проблем.
2. **Issue Creation:** Создание GitHub Issues для каждой найденной группы проблем (категоризация по критичности T0-T2).
3. **Remediation:** Surgical-исправления кода, внедрение тестов.
4. **Validation:** Повторный запуск анализаторов и тестов.
5. **Closure:** Закрытие Issues с подробным комментарием и пуш в `master`.

## ✅ Сессия 2026-05-09: Аудит v2 — 14 issues (#172–#185)

### T0-BUG / T0-STYLE (критичные)

| Issue | Компонент | Проблема | Исправление |
|-------|-----------|----------|-------------|
| #172 | CurlHttpClient | C-style cast `(int)http_code` | `static_cast<int>` + `static_cast<long>` |
| #173 | OrderBook | Data race на `top_dirty_` в `par_unseq` | `bool` → `std::atomic<bool>` |
| #174 | SignalBus | Рекурсивный дедлок в `publish()` через мьютекс | Убран мьютекс, добавлен reentrancy guard `in_publish_` |
| #175 | LevelDetector | `static last_rebuild` разделялась между всеми тикерами | Перенесена в член `last_rebuild_` |

### T1-PERF (производительность)

| Issue | Компонент | Проблема | Исправление |
|-------|-----------|----------|-------------|
| #176 | LevelDetector::rebuild_levels_ | O(C×E) вложенный скан экстремумов | O((C+E)logE) через отсортированный вектор + binary search |
| #177 | IcebergDetector::on_book_update | O(N×M) `accumulate` для каждого book-уровня | Предвычисленная карта `buy_vol_by_tick`/`sell_vol_by_tick`, O(M+N) |
| #178 | main.cpp::tick() | `static std::map` внутри функции — скрытое состояние | Перенесена в член `last_funding_times_` класса `BotApp` |
| #179 | SignalBus | `std::mutex` overhead на hot-path однопоточного bus | Убран мьютекс (закрыт вместе с #174) |
| #180 | LevelDetector | `std::map<double,...>` в `check_approaches_` | `absl::flat_hash_map` — O(1) average |

### T2/T1-STYLE (полировка)

| Issue | Компонент | Проблема | Исправление |
|-------|-----------|----------|-------------|
| #181 | OrderBook, LevelDetector | Отсутствие `[[nodiscard]]` на query-методах | Добавлено на все query-методы |
| #182 | MetaScalpCodec::check_required | `const std::string&` вместо `string_view` | Параметр → `std::string_view` |
| #183 | LeaderSignal | Мёртвая ветка `|| corr == 0` на `double` | Убрана (guard выше гарантирует `|corr| ≥ min_corr > 0`) |
| #184 | IcebergDetector | Отсутствует `<cstdlib>` для `std::abs(int64_t)` | Добавлен `#include <cstdlib>` |
| #185 | StrategyContext | `signal_history` без жёсткого ограничения размера | Добавлен `kMaxHistorySize = 1024` с принудительной очисткой |

**Состояние тестов:** 56/65 passed (9 "Not Run" — pre-existing Boost dependency issues).

## ✅ Сессия 2026-05-09: Числовые алгоритмы и сетевой слой (#163–#171)

8 issues закрыты. Исправлены:
- `Hmm`: предвычисленный `inv_var_` — деление на hot-path заменено умножением
- `MarketDataFeed`: один `j.find("Data")` вместо двух hash-lookup + `string_ref`
- `StrategyContext`: амортизированная O(1) ленивая очистка вместо O(N) `erase-from-front`
- `Clustering (DBSCAN)`: `queue`/`in_queue` вынесены из loop; единственный O(N) результирующий проход
- `Kde`: `std::for_each` вместо сырого цикла

**Дополнительно:** исправлено 2 pre-existing падения тестов (codec и tradestream), исправлен notification_routing тест (Kind integer поле + Date timestamp).

## 🚀 Следующие шаги

1. Реализация стратегии **FlushReversal** (развороты на ликвидациях).
2. Детектор ликвидаций **LiquidationDetector**.
3. Настройка автоматического CI в GitHub Actions.
