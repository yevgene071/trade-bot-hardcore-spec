# trade_bot

Production-торговый бот для крипто-фьючерсов, работающий поверх
[MetaScalp](./metascalp-sdk/docs/MetaScalp-Api.md) (REST + WebSocket на localhost).

**Язык:** C++17/20 · **Сборка:** CMake + Conan · **Runtime:** headless-демон (Linux/Windows).

---

## 📚 Документация

Вся проектная спецификация — в каталоге [`spec/`](./spec/README.md).

Основные документы (читать в этом порядке):

1. [`spec/ROADMAP.md`](./spec/ROADMAP.md) — фазы разработки и milestones
2. [`spec/ARCHITECTURE.md`](./spec/ARCHITECTURE.md) — модули, структуры данных, потоки
3. [`spec/SIGNAL_DETECTION.md`](./spec/SIGNAL_DETECTION.md) — формальные детекторы рыночных паттернов
4. [`spec/STRATEGIES.md`](./spec/STRATEGIES.md) — формализация торговых стратегий
5. [`spec/RISK_MANAGEMENT.md`](./spec/RISK_MANAGEMENT.md) — 12 правил риска с числами
6. [`spec/TASK_SPECS.md`](./spec/TASK_SPECS.md) — тикеты для ИИ-исполнителя

Индекс и карта зависимостей документов — [`spec/README.md`](./spec/README.md).

---

## 🔌 MetaScalp API

Локальный API, через который бот получает данные и торгует.

- Документация API: [`metascalp-sdk/docs/MetaScalp-Api.md`](./metascalp-sdk/docs/MetaScalp-Api.md)
- Готовые SDK (JS / Python / .NET) для быстрой проверки: [`metascalp-sdk/README.md`](./metascalp-sdk/README.md)

> Сам бот пишется на C++ **без** использования этих SDK — см. `ARCHITECTURE.md § 2.1 Transport Layer`.

---

## 🏗️ Статус проекта

| Фаза | Описание | Статус |
|------|----------|--------|
| 0 | Фундамент (transport, replay, kill-switch) | 📝 спецификация готова |
| 1 | Market Data + FeatureFrame | 📝 спецификация готова |
| 2 | Signal Detectors | 📝 спецификация готова |
| 3 | Strategy Engine (paper) | 📝 спецификация готова |
| 4 | Live Executor + RiskManager | 📝 спецификация готова |
| 5 | Бэктест и оптимизация | 📝 спецификация готова |

Код пока не написан — см. [`spec/TASK_SPECS.md`](./spec/TASK_SPECS.md) для
плана реализации. Точка старта — тикет `T0-SETUP`.

---

## 🛡️ Ключевые принципы

- **Production, не песочница.** Живые ордера появляются только после Фазы 4 (Risk + Executor).
- **Короткий стоп — грааль.** Любой вход имеет явный объект для стопа. Вход без основания запрещён.
- **Все пороги — в `config.toml`.** Ни одного магического числа в коде.
- **Kill-switch.** Файл `./killswitch` или SIGINT — мгновенная отмена ордеров и выход.
- **Reproducibility.** Replay-дампы + детерминированные часы → regression-тесты на любой сценарий.

Полный список — [`spec/README.md`](./spec/README.md) раздел «Принципы работы с документами».

---

## 🚀 Быстрый старт (для исполнителя)

1. Прочитай документы в порядке из таблицы выше.
2. Возьми тикет `T0-SETUP` из [`spec/TASK_SPECS.md`](./spec/TASK_SPECS.md).
3. Реализуй строго в рамках acceptance criteria тикета.
4. PR по шаблону из `TASK_SPECS.md § Приложение A`.
5. После зелёного CI и code review → следующий тикет.

---

## License

TBD (internal / private).
