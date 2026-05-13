# AGENTS.md

## Сборка
- `./scripts/build.sh` собирает Release по умолчанию; бинарник находится в `build/release/bin/trade_bot`.
- Для тестов используйте режим `debug`: `./scripts/build.sh debug --tests`.
- Флаги:
  - `--tests` → `BUILD_TESTS=ON`.
  - `--bench` → `BUILD_BENCHMARKS=ON`.
  - `--no-conan` → пропустить установку Conan.
  - `--jobs N` → параллельные задачи (по умолчанию `nproc`).
- Пресеты CMake: `cmake --preset release` или `debug`. Пресеты включают `ENABLE_AVX2=ON`, `ENABLE_SANITIZERS` только в debug.
- Требует GCC ≥ 13 или Clang ≥ 17; CMake ≥ 3.22.

## Тесты
- `./scripts/test.sh` запускает тесты в Debug‑сборке по умолчанию.
- Сначала необходимо собрать нужный режим (например, `./scripts/build.sh debug --tests`).
- Для выбора конкретных тестов используйте `--filter PATTERN` (передаётся в `ctest -R`).
- Печатается сводка; код возврата отражает падения.

## Конфигурация
- `config.toml` в корне репозитория содержит все параметры выполнения; значения по умолчанию находятся в `config.example.toml`.
- В коде нет «магических» чисел, пороги берутся из этого файла.
- Важные секции: `[network]`, `[risk]`, `[dashboard]`, `[signals]`, `[strategies]`.

## Выполнение
- API MetaScalp должен быть доступен на localhost (порт 17845‑17855). При отсутствии бот завершается.
- Сервер дашборда стартует на `dashboard.bind_address` (по умолчанию `127.0.0.1`) и `dashboard.port` (по умолчанию `8080`). Доступ по `http://<addr>:<port>`.
- HTML дашборда встраивается во время компиляции через `cmake/embed_html.py`.
- Переключатель «kill switch»: создайте файл `killswitch` в корне репозитория или отправьте SIGINT для мгновенной отмены ордеров.
- `killswitch` учитывает параметры `kill_switch_*` из конфигурации.

## Зависимости
- Управление сторонними библиотеками через Conan; `scripts/build.sh` выполняет `conan install`, если не указан `--no-conan`.
- Директория вывода сборки: `build/<mode>/bin`. Путь задаётся через `CMAKE_RUNTIME_OUTPUT_DIRECTORY`.
- Объектные библиотеки (`obj_logger`, `obj_simd_ops`, …) разделяются между целями.

## Прочее
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` включён для поддержки clangd.
- В Debug‑сборке включены AddressSanitizer и UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`).
- В Release‑сборке включены `-march=native` и LTO.
