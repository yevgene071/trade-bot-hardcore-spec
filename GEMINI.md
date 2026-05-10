# Trade Bot Hardcore Project Instructions

Foundational mandates for architecture, performance, and concurrency. These rules take precedence over general defaults.

## Architecture

- **Tick Resolution:** 100ms (10Hz).
- **Leader-Follower Coordination:**
  - Multi-pass tick logic: Process the leader ticker (e.g., BTC_USDT) first.
  - Broadcast the leader's `FeatureFrame` to all other `TickerController` instances via `on_leader_frame`.
  - Followers then tick, using the latest leader data for lead-lag analysis and feature extraction.
- **Lazy Processing:**
  - `TickerController` should only perform feature extraction (`extractor->extract()`) if market data has changed (dirty flags set by `on_trade` or `on_book_update`) or a 500ms stale-check timeout is reached.
- **Signal Bus:**
  - Synchronous, single-threaded bus. Use reentrancy guards in `publish()` to prevent deadlocks from recursive calls.

## Performance Mandates

- **Memory Management:**
  - **Zero-Allocation Hot Path:** Avoid heap allocations in `on_trade`, `on_book_update`, and `on_frame`. Reuse member containers (e.g., `absl::flat_hash_map`, `std::vector`) by calling `.clear()` instead of re-instantiating.
  - Use Arena allocators for non-copyable/non-movable market data objects where appropriate.
- **Order Book Optimization:**
  - Parallel processing (`std::execution::par_unseq`) should be used for batch updates of 64 levels or more.
  - Pre-partition update batches by side (Buy/Sell) to avoid map contention.
  - Perform depth and range volume calculations on raw ticks first; apply floating-point increments only once at the end.
- **Numeric Efficiency:**
  - **Feature Extraction:** Cache `log(price)` in history buffers to avoid repeated `std::log()` calls during volatility/dynamics calculations.
  - Use `FixedPoint` types (`PriceTick`, `SizeFix`) for internal map keys to avoid precision drift and simplify range queries.
- **Routing:**
  - `StrategyEngine` must route signals and frames by `Ticker` using an efficient lookup (e.g., `unordered_map`) rather than broadcasting to all strategies.

## Concurrency & Thread Safety

- **Transport Layer:**
  - `BeastWsClient`: Protect the WebSocket stream pointer and status with `m_ws_mutex` to ensure thread-safety during concurrent send/reconnect operations.
  - `MarketDataFeed`: Use `std::shared_ptr<const ListenerList>` for dispatching to listeners. This allows thread-safe iteration even if listeners are added/removed during the dispatch loop.
- **Controller State:**
  - `TickerController` operations must be protected by `mtx_` as market data arrives asynchronously from the feed thread, while `tick()` is called from the main loop thread.

## Engineering Standards

- **UTC Consistency:** All timestamps and trading day boundaries must use UTC.
- **Validation:** reproduce all bugs with tests before fixing. All performance optimizations must be backed by architectural rationale or benchmarks.
- **Formatting:** Adhere to `.clang-format` and `.clang-tidy`.
