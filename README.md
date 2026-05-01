# Trade Bot Hardcore Spec 🚀

[![CI](https://github.com/yevgene071/trade-bot-hardcore-spec/actions/workflows/ci.yml/badge.svg)](https://github.com/yevgene071/trade-bot-hardcore-spec/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Production-торговый бот для крипто-фьючерсов, работающий поверх [MetaScalp API](./metascalp-sdk/docs/MetaScalp-Api.md) (REST + WebSocket на localhost). Проект ориентирован на высокую производительность, строгий риск-менеджмент и автоматизацию проверенных торговых стратегий.

---

## 🏗 Архитектура

Проект построен на принципах **Domain-Driven Design (DDD)** и разделен на четкие слои:

*   **Transport Layer**: Низкоуровневая работа с HTTP/WebSocket (Boost.Beast).
*   **Market Data**: Реплика стакана (OrderBook) и потоки сделок с агрегацией признаков.
*   **Signal Detectors**: Детекторы паттернов (Density, Iceberg, Tape, Levels).
*   **Strategy Engine**: Изолированная логика стратегий (Bounce, Breakout, Leader-Lag).
*   **Risk Manager**: "Вратарь" системы, блокирующий любые отклонения от риск-профиля.
*   **Executor**: Управление жизненным циклом ордеров и позиций.

Подробности в [ARCHITECTURE.md](./spec/ARCHITECTURE.md).

---

## 📅 Дорожная карта (Roadmap)

Проект разделен на 6 ключевых фаз:

1.  **Phase 0: Foundation** — Базовая инфраструктура и транспорт.
2.  **Phase 1: Market Data** — Стакан, лента сделок и фичи.
3.  **Phase 2: Signals** — Детекторы торговых сигналов.
4.  **Phase 3: Strategy** — Реализация стратегий и Paper Trading.
5.  **Phase 4: Live & Risk** — Реальное исполнение и жесткий риск-менеджмент.
6.  **Phase 5: Optimization** — Бэктестинг и тонкая настройка.

Весь прогресс отслеживается в [GitHub Projects](https://github.com/users/yevgene071/projects/1).

---

## 🛠 Технологический стек

*   **Язык**: C++20
*   **Сборка**: CMake
*   **Зависимости**: Conan / vcpkg
*   **Библиотеки**:
    *   `Boost.Asio / Beast` (Сетевое взаимодействие)
    *   `nlohmann_json` (Парсинг API)
    *   `spdlog` (Логирование)
    *   `tomlplusplus` (Конфигурация)
    *   `GTest` (Тестирование)

---

## 🚀 Быстрый старт

### Требования
*   GCC 13+ или Clang 18+
*   CMake 3.25+
*   MetaScalp запущен локально (порт 17845)

### Сборка
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Запуск тестов
```bash
cd build && ctest
```

---

## 📋 Документация

Вся проектная спецификация — в каталоге [`spec/`](./spec/README.md).

1.  [`spec/ROADMAP.md`](./spec/ROADMAP.md) — фазы разработки и milestones.
2.  [`spec/ARCHITECTURE.md`](./spec/ARCHITECTURE.md) — модули, структуры данных, потоки.
3.  [`spec/SIGNAL_DETECTION.md`](./spec/SIGNAL_DETECTION.md) — формальные детекторы рыночных паттернов.
4.  [`spec/STRATEGIES.md`](./spec/STRATEGIES.md) — формализация торговых стратегий.
5.  [`spec/RISK_MANAGEMENT.md`](./spec/RISK_MANAGEMENT.md) — 12 правил риска с числами.
6.  [`spec/TASK_SPECS.md`](./spec/TASK_SPECS.md) — тикеты для ИИ-исполнителя.

---

## 🛡️ Ключевые принципы

*   **Production, не песочница.** Живые ордера появляются только после Фазы 4 (Risk + Executor).
*   **Короткий стоп — грааль.** Любой вход имеет явный объект для стопа. Вход без основания запрещён.
*   **Все пороги — в `config.toml`.** Ни одного магического числа в коде.
*   **Kill-switch.** Файл `./killswitch` или SIGINT — мгновенная отмена ордеров и выход.
*   **Reproducibility.** Replay-дампы + детерминированные часы → regression-тесты на любой сценарий.

---

## 🤝 Контакты
Автор: [@yevgene071](https://github.com/yevgene071)
