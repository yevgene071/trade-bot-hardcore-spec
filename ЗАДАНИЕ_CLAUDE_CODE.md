# Задание: наведение порядка в trade-bot-hardcore-spec

> **Читай внимательно.** Это C++23 trading bot с реальной торговой логикой, прошедший несколько раундов аудита.
> Работай строго поэтапам. После каждого этапа жди подтверждения — не переходи к следующему без ответа пользователя.
> Файлы в `core/src/` — неприкосновенны без явного разрешения. Изменения торговой логики — только с согласования.

---

## Контекст: что уже сделано

Проект уже прошёл значительную переработку. Все этапы выполнены.

| Что | Статус |
|-----|--------|
| Встраиваемый dashboard удалён (`core/src/control/dashboard/`) | ✅ ГОТОВО |
| BotApp вынесен из `main.cpp` в `core/src/app/BotAppInit.cpp`, `BotAppRun.cpp` | ✅ ГОТОВО |
| Dashboard-пайплайн удалён из `core/CMakeLists.txt` | ✅ ГОТОВО |
| Тесты вынесены в отдельный `core/test/CMakeLists.txt` и подключены через `add_subdirectory(test)` | ✅ ГОТОВО |
| `mark_price_update` использует правильные PascalCase поля | ✅ ГОТОВО |
| Frontend dashboard вынесен в отдельное React/Vite приложение `dashboard/` | ✅ ГОТОВО |
| С++ ядро реорганизовано в `core/`, документация в `docs/spec/`, данные в `data/` | ✅ ГОТОВО |
| Хардкоженные Conan-пути убраны, `fmt` добавлен как Conan-зависимость | ✅ ГОТОВО |
| SignalLevelBridge и SignalLevelGateway интегрированы в `trade_bot` target | ✅ ГОТОВО |
| `.gitignore`, скрипты и CI обновлены под новую структуру | ✅ ГОТОВО |

---

> **Читай внимательно.** Это C++23 trading bot с реальной торговой логикой, прошедший несколько раундов аудита.
> Работай строго поэтапам. После каждого этапа жди подтверждения — не переходи к следующему без ответа пользователя.
> Файлы в `core/src/` — неприкосновенны без явного разрешения. Изменения торговой логики — только с согласования.

---

## Контекст: что уже сделано

Проект уже прошёл значительную переработку. Все этапы выполнены.

| Что | Статус |
|-----|--------|
| Встраиваемый dashboard удалён (`core/src/control/dashboard/`) | ✅ ГОТОВО |
| BotApp вынесен из `main.cpp` в `core/src/app/BotAppInit.cpp`, `BotAppRun.cpp` | ✅ ГОТОВО |
| Dashboard-пайплайн удалён из `core/CMakeLists.txt` | ✅ ГОТОВО |
| Тесты вынесены в отдельный `core/test/CMakeLists.txt` и подключены через `add_subdirectory(test)` | ✅ ГОТОВО |
| `mark_price_update` использует правильные PascalCase поля | ✅ ГОТОВО |
| Frontend dashboard вынесен в отдельное React/Vite приложение `dashboard/` | ✅ ГОТОВО |
| С++ ядро реорганизовано в `core/`, документация в `docs/spec/`, данные в `data/` | ✅ ГОТОВО |
| Хардкоженные Conan-пути убраны, `fmt` добавлен как Conan-зависимость | ✅ ГОТОВО |
| SignalLevelBridge и SignalLevelGateway интегрированы в `trade_bot` target | ✅ ГОТОВО |
| `.gitignore`, скрипты и CI обновлены под новую структуру | ✅ ГОТОВО |

---

## ✅ Этапы 1–6 — Выполнены

| Этап | Что сделано |
|------|-------------|
| **1. Мусор** | Удалены 22 `*:Zone.Identifier` файла, `Testing/`, `.omc/new-dashboard-extract/`, корневые `package.json`/`package-lock.json`/`node_modules/` |
| **2. .gitignore** | Добавлены: `/Testing/`, `/dashboard/dist/`, `.omc/`, `.beads/`, `/data/`, `/reports/*.png`, `/recovery-ack`. Dashboard `.gitignore` уже в порядке |
| **3. Реорганизация** | `git mv` C++ ядра (src, test, bench, tool, CMake*, conanfile, .clang-*) → `core/`. `spec/` → `docs/spec/`. Runtime данные (replay, journal, tickers_full) → `data/`. Пустая `cmake/` удалена. CMakePresets.json, scripts/build.sh, CI, .gitignore обновлены |
| **4. Хардкод пути** | `fmt/10.2.1` добавлен в `core/conanfile.txt`. `find_package(fmt REQUIRED CONFIG)` + `fmt::fmt` к `obj_logger` в `core/CMakeLists.txt`. Удалены 2 строки `include_directories(/home/yevge/.conan2/...)` |
| **5. Тесты** | `add_subdirectory(test)` уже присутствовал в `core/CMakeLists.txt` (подключено автоматически из-за core/ реорганизации) |
| **6. Сиротские файлы** | `core/CMakeLists.txt` — `SignalLevelBridge.cpp` и `SignalLevelGateway.cpp` добавлены в `trade_bot` target. SignalLevelBridge подписан на `SignalKind::LevelFormed`. Создание в `BotAppInit.cpp` |

---

## ✅ Этап 7 — Обновление документации (текущий этап)

Документация обновлена: пути `src/` → `core/src/`, `spec/` → `docs/spec/`, структура проекта с `core/`.

### 7.1 METASCALP_API_CONTRACT.md

Если есть новая версия API-спецификации от MetaScalp, обнови `docs/spec/METASCALP_API_CONTRACT.md`.

### 7.2 Аудит-находки (не входят в эту задачу)

В `reports/` есть отчёты аудита с критическими багами (killswitch, command endpoint, R14, R15).
Эти проблемы **не должны исправляться** в рамках реорганизации — это отдельная кодовая работа.

---

## Итоговые артефакты (выполненные этапы 1–7)

| Артефакт | Где | Статус |
|---|---|---|
| `core/` | C++ ядро бота | ✅ |
| `docs/spec/` | Архитектура, стратегии, сигналы, API-контракт | ✅ |
| `data/` | Runtime данные (replay, journal, tickers) | ✅ |
| `.gitignore` | Обновлён под новую структуру | ✅ |
| `core/CMakeLists.txt` | Без хардкоженных путей | ✅ |
| `core/conanfile.txt` | Добавлен `fmt` | ✅ |
| Тесты в CMake | Подключены через `add_subdirectory(test)` | ✅ |
| Zone.Identifier | Удалены все | ✅ |
| Root package.json | Удалён | ✅ |
| SignalLevelBridge/Gateway | Интегрированы в `trade_bot` | ✅ |
| Обновлены скрипты и CI | Пути под `core/` | ✅ |

> ⚠️ **Не входит в объём:** `README.md` (Этап 8) и `Makefile` (Этап 9) не создавались — ожидают отдельного запроса.

---

## Запрещено

- ❌ Не трогать торговую логику в `core/src/strategy/`, `core/src/signals/`, `core/src/risk/`
- ❌ Не изменять версии зависимостей в `conanfile.txt` без явного указания
- ❌ Не удалять файлы из `core/src/`, `metascalp-sdk/`, `data/`, `core/test/`
- ❌ Не трогать `config.toml` и файлы в `config/`
- ❌ Не трогать `.github/workflows/` кроме обновления путей после перемещения
- ❌ Любое удаление файлов из `core/src/` — только после явного подтверждения
- ❌ Не исправлять критические баги из audit-отчётов без явного запроса (это отдельная задача)

---

## Известные issues (из аудита, НЕ входят в эту задачу)

Следующие проблемы найдены аудитом, но **не должны исправляться** в рамках этой реорганизации:

| # | Проблема | Файл | Серьёзность |
|---|----------|------|-------------|
| 1 | `/api/killswitch/toggle` не вызывает `KillSwitch::instance()` | `core/src/control/DashboardServer.cpp:466` | 🔴 КРИТ |
| 2 | `/api/command` — заглушка (no-op) | `core/src/control/DashboardServer.cpp:480` | 🔴 КРИТ |
| 3 | R14 Single Position Loss Kill не реализован | `core/src/risk/RiskManager` | 🔴 КРИТ |
| 4 | R15 Entry Slippage Control не реализован | `core/src/executor/LiveExecutor` | 🔴 КРИТ |
| 5 | FinresHandler: balance == equity | `core/src/transport/FinresHandler.cpp:43` | 🟡 СРЕД |
| 6 | FlushReversal: config есть, реализации нет | — | 🔴 КРИТ |
| 7 | Аффинити-система обойдена (все стратегии на все тикеры) | `core/src/main.cpp` | 🟡 СРЕД |
| 8 | `cleanup_triggered` без проверки HTTP статуса | `core/src/transport/SignalLevelGateway.cpp:65` | 🟢 НИЗК |
