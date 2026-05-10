# Инструкция по сборке

Этот проект использует **CMake**, **Ninja** и **Conan 2**.

## Предварительные требования

- Компилятор с поддержкой C++23 (GCC 13+, Clang 17+ или MSVC 2022+)
- CMake 3.22+
- Система сборки Ninja (`sudo apt install ninja-build`)
- Conan 2.0+

---

## Сборка через пресеты (Рекомендуется)

В проекте настроены пресеты в `CMakePresets.json`. Сборка проходит в три этапа: установка зависимостей, конфигурация и сама компиляция.

### 1. Release (Максимальная производительность)
Используйте этот вариант для работы робота.

```bash
# 1. Установка зависимостей через Conan
conan install . --output-folder=build/release --build=missing -s build_type=Release

# 2. Конфигурация CMake
cmake --preset release

# 3. Сборка
cmake --build --preset release -j$(nproc)
```
**Бинарный файл:** `build/release/bin/trade_bot`

### 2. Debug (Для разработки и отладки)
Включает санитайзеры (ASan, UBSan) и отладочную информацию.

```bash
# 1. Установка зависимостей через Conan
conan install . --output-folder=build/debug --build=missing -s build_type=Debug

# 2. Конфигурация CMake
cmake --preset debug

# 3. Сборка
cmake --build --preset debug -j$(nproc)
```
**Бинарный файл:** `build/debug/bin/trade_bot`

---

## Возможные проблемы

### Ошибка: Could not find toolchain file
Если CMake ругается на отсутствие `conan_toolchain.cmake`, значит шаг `conan install` не был выполнен или был выполнен для другой директории. Убедитесь, что `--output-folder` в команде conan совпадает с ожидаемым путем в пресете.

### Ошибка: Could not find build program corresponding to "Ninja"
Установите Ninja:
- **Ubuntu/Debian:** `sudo apt install ninja-build`
- **MacOS:** `brew install ninja`
- **Windows:** `choco install ninja`

---

## Запуск тестов

```bash
# Через CTest
ctest --test-dir build/debug --output-on-failure

# Или через рабочий процесс (workflow)
cmake --workflow --preset debug
```
