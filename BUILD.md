# Инструкция по сборке

Этот проект использует **CMake**, **Ninja** и **Conan 2**.

## Предварительные требования

- Компилятор с поддержкой C++23 (GCC 13+, Clang 17+ или MSVC 2022+)
- CMake 3.22+
- Система сборки Ninja (`sudo apt install ninja-build`)
- Conan 2.0+
- Python 3 (для эмбеддинга dashboard HTML)

---

## Быстрый старт (скрипты)

```bash
# Production-сборка одной командой
./scripts/build.sh

# Debug + тесты
./scripts/build.sh debug --tests

# Очистка
./scripts/build.sh clean

# Запуск тестов
./scripts/test.sh

# Конкретные тесты
./scripts/test.sh --filter orderbook
```

Подробнее: `./scripts/build.sh --help`, `./scripts/test.sh --help`

---

## Ручная сборка через пресеты

Если нужно больше контроля, используйте пресеты напрямую.

### Release (Максимальная производительность)

```bash
conan install . --output-folder=build/release --build=missing -s build_type=Release
cmake --preset release
cmake --build --preset release -j$(nproc)
```

Бинарный файл: `build/release/bin/trade_bot`
compile_commands.json: `build/release/compile_commands.json`

### Debug (ASan + UBSan)

```bash
conan install . --output-folder=build/debug --build=missing -s build_type=Debug
cmake --preset debug
cmake --build --preset debug -j$(nproc)
```

Бинарный файл: `build/debug/bin/trade_bot`

### Тесты через CTest

```bash
ctest --test-dir build/debug --output-on-failure
# или с фильтром:
ctest --test-dir build/debug --output-on-failure -R orderbook
```

---

## Режимы сборки

| Режим    | AVX2 | LTO | Оптимизации              | Санитайзеры |
|----------|------|-----|--------------------------|-------------|
| Release  | ✓    | ✓   | -O3 -march=native        | —           |
| Debug    | ✓    | —   | -O0                      | ASan + UBSan|

---

## Возможные проблемы

### Ошибка: Could not find toolchain file
Если CMake ругается на отсутствие `conan_toolchain.cmake`, запустите `conan install` для нужного режима сборки.

### Ошибка: Could not find build program corresponding to "Ninja"
Установите Ninja:
- **Ubuntu/Debian:** `sudo apt install ninja-build`
- **MacOS:** `brew install ninja`

### GCC/Clang version too old
Требуется GCC ≥ 13 или Clang ≥ 17. Сборка прервётся с понятным сообщением об ошибке.
