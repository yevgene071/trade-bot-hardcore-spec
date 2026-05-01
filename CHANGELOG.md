# CHANGELOG

## [Unreleased]
- T0-CODEC: Реализован JSON-кодек для парсинга сообщений MetaScalp API с поддержкой дуальных типов ордеров.
- T0-WS: Реализован асинхронный WebSocket-клиент на базе Boost.Beast с поддержкой авто-реконнекта и heartbeats.
- T0-DOMAIN: Описаны базовые доменные типы (Ticker, Side, Order, Position, Balance) для работы с MetaScalp API.
- T0-HTTP: Реализован HTTP-клиент на базе libcurl и модуль автоматического обнаружения MetaScalp API.
- T0-CONFIG: Реализован модуль конфигурации на базе `toml++` с поддержкой векторов и строгой валидацией при запуске.
