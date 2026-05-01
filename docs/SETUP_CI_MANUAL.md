# Инструкция по настройке CI/CD (GitHub Actions)

Из-за ограничений безопасности GitHub, автоматические системы (как я) не могут напрямую создавать файлы в папке `.github/workflows/`. Чтобы ваш проект автоматически собирался и тестировался, вам нужно создать один файл вручную.

### Что нужно сделать:

1. В вашем репозитории на GitHub нажмите кнопку **"Add file"** -> **"Create new file"**.
2. В поле имени файла введите: `.github/workflows/ci.yml` (именно так, с точкой в начале).
3. Скопируйте и вставьте туда следующий текст:

```yaml
name: CI

on:
  push:
    branches: [ main, master ]
  pull_request:
    branches: [ main, master ]

jobs:
  build:
    runs-on: ubuntu-latest

    steps:
    - uses: actions/checkout@v3

    - name: Install Dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y cmake build-essential libboost-all-dev libcurl4-openssl-dev

    - name: Configure CMake
      run: cmake -B build -DCMAKE_BUILD_TYPE=Release

    - name: Build
      run: cmake --build build -j$(nproc)

    - name: Run Tests
      run: cd build && ctest --output-on-failure
```

4. Нажмите **"Commit changes..."**.

### Что это даст:
Теперь при каждом пуше кода GitHub будет автоматически проверять:
*   Устанавливаются ли все зависимости.
*   Компилируется ли проект без ошибок.
*   Проходят ли все тесты (Unit-тесты).

Это стандарт для профессиональной разработки, который гарантирует, что ваш "master" всегда находится в рабочем состоянии.
