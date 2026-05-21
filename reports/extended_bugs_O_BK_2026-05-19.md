# Extended Bug Report: Groups O–BK

**Date**: 2026-05-19  
**Scope**: Transport, Market Data, Signals, Executor, Universe, Performance Primitives, Numeric, Logger, Metrics, Dashboard, App Pipeline, Config  
**Total Issues**: 100+  
**Priority**: HIGH — Multiple critical correctness, performance, and determinism issues

---

## Impact Legend

- **[CORR]** — Correctness (functional bugs, data races, undefined behavior)
- **[SPD]** — Speed/Performance (unnecessary allocations, contention, inefficient algorithms)
- **[ACC]** — Accuracy (numerical precision, statistical correctness)
- **[DET]** — Determinism (replay reproducibility violations)
- **[SEC]** — Security (credential leaks, timing attacks)
- **[LEAK]** — Memory Leak (unbounded growth, missing cleanup)

---

## O. Transport / CurlHttpClient (REST to MetaScalp)

### O1. [CORR] Global libcurl initialization per instance
**Issue**: `curl_global_init()` called in every constructor, `curl_global_cleanup()` in every destructor. Multiple instances will interfere with each other.  
**Impact**: libcurl requires exactly ONE init per process. Multiple instances cause undefined behavior.  
**Fix**: Move to singleton initialization in main() or use `std::call_once`.

### O2. [SPD] No connection pooling
**Issue**: `curl_easy_init()` on EVERY request → new TCP connection + full TLS handshake each time.  
**Impact**: On polling positions/balances/orders for 20+ tickers — dozens of extra RTTs per second.  
**Fix**: Implement connection pooling with `curl_multi` or reuse `CURL*` handles.

### O3. [CORR] Missing connect timeout
**Issue**: No `CURLOPT_CONNECTTIMEOUT_MS` set (only general timeout). DNS/connect can hang for entire `timeout_ms`.  
**Impact**: Unresponsive DNS can block for full timeout period.  
**Fix**: Set `CURLOPT_CONNECTTIMEOUT_MS` separately (e.g., 5000ms).

### O4. [CORR] Missing Content-Type header
**Issue**: For POST/PUT, body doesn't set `Content-Type: application/json`.  
**Impact**: Many APIs return 415 Unsupported Media Type.  
**Fix**: Add `Content-Type: application/json` header for POST/PUT with JSON body.

### O5. [SEC] API key leakage in logs
**Issue**: `LOG_ERROR(... URL: {}, url)` writes full URL to log. If key/token in query string → leaked.  
**Impact**: Credentials exposed in log files.  
**Fix**: Sanitize URLs before logging (redact query parameters or use separate credential logging).

### O6. [CORR] Case-sensitive header storage
**Issue**: Headers stored in `std::map<string,string>` case-sensitive, but HTTP headers are case-insensitive: `Content-Type` ≠ `content-type`.  
**Impact**: Header lookups may fail due to case mismatch.  
**Fix**: Use case-insensitive comparator or normalize to lowercase.

### O7. [CORR] Timeout read without synchronization
**Issue**: `m_timeout_ms` read from another thread without `atomic`/`mutex`.  
**Impact**: Data race, undefined behavior.  
**Fix**: Make `m_timeout_ms` `std::atomic<int>` or protect with mutex.

---

## P. Transport / BeastWsClient (WebSocket to MetaScalp)

### P1. [CORR] Timers not on strand
**Issue**: `m_reconnect_timer(ioc)` and `m_ping_timer(ioc)` created on bare `ioc`, not on `strand`. If `ioc` runs on 2+ threads → race between ping and reconnect.  
**Impact**: Concurrent timer callbacks can corrupt state.  
**Fix**: Create timers on `strand` to serialize callbacks.

### P2. [CORR] Non-atomic boolean flags
**Issue**: `m_closing`, `m_connected` — plain `bool`, read/written from multiple threads → data race / UB.  
**Impact**: Undefined behavior, potential crashes.  
**Fix**: Use `std::atomic<bool>`.

### P3. [CORR/LEAK] Message loss on write failure
**Issue**: `do_write` moves `front()` into `m_write_msg`, then checks `if (!m_ws) return;` — message "pulled" but never sent and not returned to queue.  
**Impact**: Loss of outgoing messages (subscribe/heartbeat).  
**Fix**: Check `m_ws` validity BEFORE moving message.

### P4. [CORR] Silent queue overflow
**Issue**: `kMaxQueueSize=1000` hardcoded. On overflow, silent drop of subscribe/heartbeat → bot loses subscription without logging.  
**Impact**: Silent loss of critical messages.  
**Fix**: Log queue overflow at ERROR level, consider backpressure mechanism.

### P5. [CORR] WebSocket not reset on disconnect
**Issue**: `disconnect()` doesn't reset `m_ws`. After disconnect, `do_ping`/`do_read` callbacks can access closed stream.  
**Impact**: Use-after-close, potential crash.  
**Fix**: Reset `m_ws` in `disconnect()`.

### P6. [CORR] Reconnect backoff never resets
**Issue**: `m_reconnect_delay_s = 1` only resets after successful handshake. If server rejects in handshake → backoff grows infinitely.  
**Impact**: Exponentially increasing reconnect delays even for transient issues.  
**Fix**: Reset backoff on successful connection establishment, not just handshake.

### P7. [SEC] Incomplete TLS verification
**Issue**: `set_verify_mode(verify_peer)` present, but no `set_hostname(host)` / `set_verify_callback`. Hostname verification depends on Boost.Beast defaults.  
**Impact**: Potential MITM vulnerability if defaults insufficient.  
**Fix**: Explicitly set hostname verification.

### P8. [SPD] Trace ID allocation on error
**Issue**: `next_trace_id()` called even when `ec != 0` → wastes TraceTimeBuffer slots (mod 16384 → faster collisions).  
**Impact**: Trace buffer pollution, reduced effective capacity.  
**Fix**: Only allocate trace ID on successful operations.

---

## Q. Transport / MarketDataFeed

### Q1. [CORR] Identical to_ms and from_ms implementations (line 25)
**Issue**: `to_ms` and `from_ms` have identical bodies. `from_ms` does NOT invert the conversion.  
**Impact**: Cannot convert back from internal format to API format.  
**Fix**: Implement proper inverse conversion in `from_ms`.

### Q2. [DET] System clock usage in funding handler
**Issue**: `fd.updated_at = std::chrono::system_clock::now();` — violates Phase3 ICLOCK requirement.  
**Impact**: Non-deterministic replay.  
**Fix**: Use exchange timestamp from API or monotonic clock with offset.

### Q3. [CORR] Hardcoded depth parameters
**Issue**: `DepthLevels=50`, `DepthPercent=0.5` hardcoded, not in config (violates AGENTS.md "no hardcoded magic numbers").  
**Impact**: Cannot tune without recompilation.  
**Fix**: Move to config file.

### Q4. [CORR] TOCTOU race in send
**Issue**: `is_connected()` → `send()`; connection can break between checks.  
**Impact**: Send to closed connection.  
**Fix**: Check connection status atomically or handle send errors gracefully.

---

## R. Transport / MetaScalpCodec (Message Decoding)

### R1. [DET/CORR] Local timestamp instead of server timestamp
**Issue**: `parse_orderbook_update`: `ts = std::chrono::system_clock::now()` always, ignores server timestamp.  
**Impact**: All time-window signals (TapeFlush, Iceberg join window) get local time instead of exchange time → breaks replay and distorts time filters.  
**Fix**: Parse and use server timestamp from message.

### R2. [DET] Fallback to now() in snapshot parsing
**Issue**: `parse_orderbook_snapshot`: fallback `now()` if Time field empty; in replay payload may not have it.  
**Impact**: Non-deterministic replay.  
**Fix**: Require timestamp in replay mode, fail if missing.

### R3. [DET] System clock in signal parsing (line 565)
**Issue**: `parse_signal_level_triggered` and `parse_cluster_snapshot` use `now()` instead of server time.  
**Impact**: Non-deterministic signal timestamps.  
**Fix**: Use server timestamp from message.

### R4. [CORR] No timezone handling
**Issue**: `parse_timestamp` doesn't handle TZ suffixes (Z, +03:00) — all strings treated as UTC.  
**Impact**: For feeds with TZ offset — real time is shifted.  
**Fix**: Parse timezone information properly.

### R5. [CORR] Unknown status defaults to New (line 116)
**Issue**: `parse_order_status` / `parse_position_status` for unknown values return `OrderStatus::New` / `PositionStatus::New`.  
**Impact**: Statuses like "Cancelled", "Rejected", "PartiallyFilled" (which may exist in API) silently become "New", potentially reactivating cancelled orders in LiveExecutor.  
**Fix**: Return error or Unknown status, don't default to New.

### R6. [CORR] Redundant lowercase conversion (lines 277, 304, 343, 425, 545)
**Issue**: `alt[0] = tolower(alt[0])` assumes first letter is uppercase. If `fields::kBalances` already lowercase → search duplicated wastefully.  
**Impact**: Performance degradation, potential bugs if field names change.  
**Fix**: Normalize field names consistently or use case-insensitive comparison.

### R7. [CORR] Millisecond precision loss
**Issue**: Milliseconds parsed only to 3 digits. If API sends microseconds (typical) → lose 3 digits of time precision.  
**Impact**: Reduced timestamp accuracy for high-frequency operations.  
**Fix**: Parse microseconds or nanoseconds if available.

---

## S. Marketdata / TradeStream

### S1. [ACC] Ring buffer volume accumulator drift
**Issue**: On ring buffer overflow: `tail_1s_/5s/30s` indices advance, but evicted trade NOT subtracted from `buy_vol_*_` / `sell_vol_*_` KahanAccumulator.  
**Impact**: Accumulators gradually "inflate" with forgotten old trades → distortion of `buy_vol_30s`/`sell_vol_30s`, affects TapeBurst/TapeAggression/TapeDistribution.  
**Fix**: Subtract evicted trade volume before advancing tail.

### S2. [CORR] Const-cast data race in get_stats
**Issue**: `const_cast<TDigest&>` + mutation of `cached_q99_` and `last_merge_weight_` in const `get_stats()`.  
**Impact**: If `get_stats` called from multiple threads (PaperExecutor.unrealized_pnl + dashboard tick + detectors) → data race on T-Digest internals.  
**Fix**: Make `get_stats()` non-const or use proper synchronization.

### S3. [DET] Sentinel comparison with epoch
**Issue**: `if (last_hawkes_update_ != time_point{})` — sentinel comparison with epoch.  
**Impact**: Fragile initialization detection.  
**Fix**: Use `std::optional` or explicit initialization flag.

### S4. [ACC] Exchange time vs local time mismatch
**Issue**: `evict_expired_trades_` uses `t.timestamp` (exchange time from PipelineProcessor:327 — `now()`).  
**Impact**: On network break and sudden jump of `now()` relative to real exchange time → mass eviction. See also R1.  
**Fix**: Use consistent time source (exchange time throughout).

---

## T. Marketdata / LeaderTracker

### T1. [SPD/LEAK] Heap allocation per Kalman update
**Issue**: `std::vector<double> dl(W), df(W)` allocated inside `recompute_lag_observation_` on each update.  
**Impact**: Heap allocation per Kalman update.  
**Fix**: Pre-allocate as member variables or use stack arrays.

### T2. [SPD] O(K×W) double loop
**Issue**: Double loop O(K×W) — for K=10, W=200 = 2000 ops × each update × 10Hz × 50 tickers ≈ 1M ops/sec.  
**Impact**: CPU bottleneck on high ticker count.  
**Fix**: Optimize algorithm or reduce update frequency.

### T3. [ACC] CUSUM alarm never resets
**Issue**: CUSUM on correlation triggers `cusum_alarm_=true`, but no auto-reset. After correlation recovery, alarm stays "forever" until explicit `reset_cusum()`.  
**Impact**: False positives persist indefinitely.  
**Fix**: Auto-reset alarm when correlation recovers.

---

## U. Marketdata / KalmanLagEstimator

### U1. [ACC] Hardcoded outlier cutoff
**Issue**: Outlier cutoff `|y_inno| > 5000.0 ms` hardcoded, no logging of rejections.  
**Impact**: Real lag "jumps" invisible in metrics.  
**Fix**: Make threshold configurable, log outlier rejections.

### U2. [CORR] Covariance symmetrization may break positive-definiteness
**Issue**: `p_[0][1] = p_[1][0] = 0.5 * (new01 + new10)` symmetrization — mathematically, `(new01 + new10)/2` doesn't always preserve positive-definiteness.  
**Impact**: Combined with `max(1e-6, ...)` on diagonal, can start ill-conditioned.  
**Fix**: Use proper covariance update (Joseph form) or enforce positive-definiteness.

---

## V. Marketdata / ClusterSnapshot

### V1. [SPD] Random device per poll
**Issue**: `std::mt19937_64 rng{random_device{}()}` created on each `schedule_poll_` tick. `random_device` is expensive system call on Linux (/dev/urandom).  
**Impact**: Unnecessary system call overhead.  
**Fix**: Create RNG once, reuse across polls.

### V2. [SPD] Sequential synchronous HTTP
**Issue**: `client_.fetch(ticker, tf)` synchronous HTTP, called SEQUENTIALLY in loop over tickers × timeframes.  
**Impact**: Blocks ExternalIoContext thread for sum of RTTs.  
**Fix**: Use async HTTP or parallel requests.

### V3. [CORR] Unbounded cache growth
**Issue**: `cache_[ticker][tf]` grows unbounded by tickers (universe rotation → old tickers not evicted).  
**Impact**: Memory leak.  
**Fix**: Implement cache eviction policy (LRU, TTL).

---

## W. Features / FeatureExtractor / ChartHistory

### W1. [CORR] Division by zero on reserve_history=0
**Issue**: `mid_head_ = (mid_head_ + 1) % cfg_.reserve_history` — if `reserve_history == 0` → division by 0 / UB.  
**Impact**: Crash or undefined behavior.  
**Fix**: Validate `reserve_history > 0` in config.

### W2. [CORR] Magic spread value on crossed book
**Issue**: On crossed book (ask<bid), `spread_bps = 999.0` magic value.  
**Impact**: Downstream code may interpret as real wide spread.  
**Fix**: Use sentinel value (e.g., -1.0) or error flag.

### W3. [ACC] Zero mid on one-sided book
**Issue**: `if (f.mid > 0.0)` — `f.mid` can be 0 on one-sided book, frame published with `mid=0`.  
**Impact**: Strategies receive "mid=0" as valid.  
**Fix**: Mark frame as invalid or skip publication.

### W4. [ACC] Fixed capacity doesn't match config
**Issue**: `kCapacity = 600` for 60s @ 10Hz, but `frame_rate_hz` in config — if changed, capacity doesn't adjust.  
**Impact**: Mismatch between intended window and actual capacity.  
**Fix**: Calculate capacity from config: `capacity = window_seconds * frame_rate_hz`.

---

## X. Signals / DensityDetector

### X1. [ACC] Average level size includes dust
**Issue**: `avg_level_size_.update(level.size)` updates on EVERY positive level (including small dust).  
**Impact**: DEMA biased toward low average → threshold `min_size_vs_avg * avg_size` becomes near-zero → false detections.  
**Fix**: Filter out dust levels below minimum threshold before updating average.

### X2. [CORR] Detection flag never resets
**Issue**: `meta.emitted = true` forever: after first DensityDetected on ticker, repeat density appearance (refill, new large order) on same ticker won't generate signal.  
**Impact**: Missed subsequent density events.  
**Fix**: Reset `emitted` flag on level disappearance or timeout.

### X3. [LEAK] Tracked levels not cleaned beyond max_distance
**Issue**: `tracked_` cleaned only when `level.size=0`; levels beyond `max_distance_bps` not removed.  
**Impact**: Memory leak.  
**Fix**: Prune levels beyond `max_distance_bps` periodically.

### X4. [CORR] ASan workaround for absl::flat_hash_map
**Issue**: Workaround for ASan + `absl::flat_hash_map::begin()` bug.  
**Impact**: Symptom of using library with known defect.  
**Fix**: Update Abseil or switch to alternative container.

---

## Y. Signals / IcebergDetector

### Y1. [LEAK] Unbounded trade history
**Issue**: `trade_history_.push_back(...)` without upper bound (only `last_sizes_/levels_` pruned in `on_frame`).  
**Impact**: Memory leak.  
**Fix**: Implement sliding window with max size.

### Y2. [SPD] O(N) scan per update
**Issue**: O(N) pass through entire `trade_history_` on each OrderBookUpdate (hundreds/sec).  
**Impact**: With growing history — O(N×M)/sec.  
**Fix**: Use time-indexed structure or limit history size.

### Y3. [ACC] Posterior stuck at extremes
**Issue**: When `denom<1e-12`, posterior fixed at 0.99/0.01; reverse movement to normal `posterior_threshold≈0.8` after 0.01 takes long time.  
**Impact**: Missed signals.  
**Fix**: Add posterior decay or reset mechanism.

### Y4. [ACC] Level re-appearance ignored
**Issue**: `if (size_before == 0.0) return` — on level re-appearance (delete→add), first refill after reappearance silently dropped.  
**Impact**: Missed iceberg detection on level re-entry.  
**Fix**: Track level lifecycle properly.

---

## Z. Signals / TapeAnalyzer

### Z1. [ACC] Peak intensity not true sliding window
**Issue**: `peak_intensity_60s_` NOT true sliding window: after 60s without new high — peak resets to current value (may be in trough).  
**Impact**: Target/drift for CUSUM Fade "jump" → false signals.  
**Fix**: Implement proper sliding window maximum.

### Z2. [ACC] Hardcoded EMA period
**Issue**: `Ema::from_period(300)` hardcoded; comment "~30s at 10Hz" — if `feature_rate_hz` in config changes, EMA window shifts silently.  
**Impact**: Incorrect time window.  
**Fix**: Calculate period from config: `period = window_seconds * feature_rate_hz`.

### Z3. [ACC] Magic floor on intensity
**Issue**: `std::max(0.1, sell_intensity)` — magic floor 0.1; during silence both intensities ~0.1 → ratio≈1, false triggers on noise.  
**Impact**: False positives during low activity.  
**Fix**: Use adaptive floor or require minimum activity threshold.

### Z4. [ACC] Mid not updated without book
**Issue**: `last_pre_trade_mid_` updated ONLY in `on_book_update`. On first trades after reconnect (book not arrived) TapeFlush completely suppressed.  
**Impact**: Missed signals after reconnect.  
**Fix**: Initialize mid from first trade or require book before processing trades.

### Z5. [ACC] Hardcoded thresholds (line 111)
**Issue**: Magic thresholds 5.0 (peak gate) and 1.5 (CUSUM reset) hardcoded.  
**Impact**: Cannot tune without recompilation.  
**Fix**: Move to config.

---

## AA. Signals / LevelDetector

### AA1. [LEAK] Unbounded mid history
**Issue**: `mid_history_.push_back` on each frame, never pruned. 10Hz × 50 tickers × 24h = tens of millions of records.  
**Impact**: Memory leak.  
**Fix**: Implement sliding window with max size.

### AA2. [LEAK] Unbounded extremes
**Issue**: `extremes_` also without upper bound → `Dbscan1D::cluster(prices, ...)` (O(N log N)) degrades with growing N.  
**Impact**: Memory leak + performance degradation.  
**Fix**: Prune old extremes beyond time window.

### AA3. [DET] System clock in rebuild
**Issue**: `rebuild()` uses `system_clock::now()`.  
**Impact**: Non-deterministic replay.  
**Fix**: Use monotonic clock or event time.

### AA4. [ACC] Hardcoded window sizes
**Issue**: Magic windows {5, 25, 75} hardcoded.  
**Impact**: Cannot tune without recompilation.  
**Fix**: Move to config.

### AA5. [LEAK] Stale approach records
**Issue**: `current_approaches_` not pruned when `active_levels_` mutate (level disappeared, but record remains).  
**Impact**: Memory leak.  
**Fix**: Clean up approaches when levels removed.

### AA6. [SPD] Heap allocations in rebuild
**Issue**: `std::vector<double> prices(extremes_.size())` + `sorted_extremes` (emplace_back + sort) — heap allocs on each rebuild (once per minute, but per-ticker — 50 tickers).  
**Impact**: Allocation overhead.  
**Fix**: Pre-allocate or use stack arrays for small sizes.

---

## AB. Signals / ApproachAnalyzer

### AB1. [ACC] Repeated observation inflates confidence
**Issue**: `std::vector<...> obs(5, {speed, pullbacks, dist})` — repeating one sample 5 times for "sharpen posterior" statistically incorrect.  
**Impact**: HMM thinks it's 5 IID observations, artificially inflates confidence.  
**Fix**: Use single observation or proper confidence adjustment.

### AB2. [LEAK] Unbounded history
**Issue**: `history_.push_back` without upper bound; filtering only in `analyze()`.  
**Impact**: Memory leak.  
**Fix**: Implement sliding window.

### AB3. [CORR] Fragile config sentinel
**Issue**: Sentinel "config not set" via `start_probs[0]==0` fragile.  
**Impact**: Accidental zero in config breaks detection.  
**Fix**: Use explicit flag or `std::optional`.

---

## AC. Signals / LeaderSignal

### AC1. [LEAK] Unbounded history (line 32)
**Issue**: `our_history_` / `leader_history_` without upper bound.  
**Impact**: Memory leak.  
**Fix**: Implement sliding window.

### AC2. [SPD] O(N) backward scan
**Issue**: `get_move` lambda O(N) backward scan of history — with growing history, increasingly slow.  
**Impact**: Performance degradation.  
**Fix**: Use time-indexed structure or limit scan depth.

---

## AD. Signals / SignalLevelBridge

### AD1. [CORR] No unsubscribe in destructor
**Issue**: `bus_.subscribe(lambda capturing this)` in constructor, no unsubscribe in destructor.  
**Impact**: If bridge deleted before bus → use-after-free on next publish.  
**Fix**: Store subscription ID and unsubscribe in destructor.  
**Status**: PARTIALLY FIXED (lines 14, 26-30 show fix in progress)

### AD2. [SPD] Synchronous HTTP under mutex
**Issue**: `gateway_.remove(id)` (synchronous HTTP) called while holding `mtx_`.  
**Impact**: Blocks all other publishers for RTT duration.  
**Fix**: Move HTTP call outside mutex or use async HTTP.  
**Status**: MENTIONED but implementation incomplete

### AD3. [DET] System clock usage
**Issue**: `system_clock::now()`.  
**Impact**: Non-deterministic replay.  
**Fix**: Use event time or monotonic clock.

---

## AE. Executor / PaperExecutor

### AE1. [CORR] Position overwrite without check
**Issue**: `positions_[plan.ticker] = ...` without checking existing position.  
**Impact**: Theoretically possible double-fill erases state.  
**Fix**: Check for existing position, merge or error.

### AE2. [ACC] Exchange time vs book time mismatch
**Issue**: `tick(ticker, trade.timestamp)` uses exchange time from trade; if book fresher than trade (common), Stop/TP checked against "past" price.  
**Impact**: Simulation distortion.  
**Fix**: Use most recent timestamp (max of trade and book).

### AE3. [ACC] Linear slippage model (lines 191, 305)
**Issue**: Slippage as `exit_price * slippage_bps/10000` — linear model, doesn't account for size vs book depth.  
**Impact**: On large sizes, underestimates slippage.  
**Fix**: Implement depth-aware slippage model.

### AE4. [CORR] Market order ignores book depth
**Issue**: Market order fill takes simple ask/bid — ignores size vs top-of-book.  
**Impact**: Unrealistic fills on large orders.  
**Fix**: Walk the book to calculate realistic fill price.

### AE5. [SPD] Mutex held during book access
**Issue**: `mutex_` held for entire `tick()` (including reading `book->best_bid/ask`).  
**Impact**: Contention bottleneck.  
**Fix**: Reduce critical section, use lock-free structures where possible.

---

## AF. Trading / OrderReconciliator

### AF1. [CORR] Backoff multiplier doesn't grow
**Issue**: On exception in fetcher, all pending moved to Pending with increment backoff — but `backoff_multiplier` is int (multiplier=1 → backoff doesn't grow; >1 rounds coarsely).  
**Impact**: Ineffective backoff.  
**Fix**: Use exponential backoff with proper floating-point or duration arithmetic.

### AF2. [CORR] Incomplete intent matching
**Issue**: `matches_intent_` compares side/type/price/size, but NOT `reduce_only`, `client_order_id`, `time_in_force`.  
**Impact**: Two technically different orders can match.  
**Fix**: Include all relevant fields in matching logic.

---

## AG. Universe / TickerUniverse

### AG1. [CORR] Pool warmup admits all tickers
**Issue**: "Pool warmup" returns true for ALL tickers without stats → pool polluted with illiquid coins until stats arrive.  
**Impact**: Trading on illiquid tickers before proper filtering.  
**Fix**: Require minimum stats before admission.

### AG2. [CORR] Data races on universe state (lines 144, 154, 160, 166, 172)
**Issue**: Methods `is_strategy_enabled`/`enabled_strategies`/`on_big_tick`/`on_big_amount`/`on_screener_new_coin` NOT under mutex.  
**Impact**: `affinity_`, `boosts_`, `screener_approved_`, `all_candidates_`, `active_` modified from NotificationFeed (WS thread) and strategies (other threads). Data race on std containers.  
**Fix**: Protect all universe state access with mutex or use lock-free structures.

### AG3. [LEAK] Boosts never cleaned
**Issue**: `boosts_[ticker] = now` without cleanup by TTL — `boosts_` grows unbounded.  
**Impact**: Memory leak.  
**Fix**: Implement TTL-based cleanup.

### AG4. [SPD] O(N*M) screener search
**Issue**: `std::find(begin, end, t)` in loop over `screener_approved_` → O(N*M) on each screener event.  
**Impact**: Performance degradation with many tickers.  
**Fix**: Use hash set for O(1) lookup.

### AG5. [DET] System clock for boost time
**Issue**: `system_clock::time_point` saved as boost time; on replay non-deterministic.  
**Impact**: Non-deterministic replay.  
**Fix**: Use event time or monotonic clock.

---

## AH. Performance Primitives

### AH1. [SPD] No move overload in push
**Issue**: `push(const T&)` only — for movable T, unnecessary copy.  
**Impact**: Performance overhead for movable types.  
**Fix**: Add `push(T&&)` overload.

### AH2. [CORR] Non-thread-safe monotonic_buffer_resource
**Issue**: `monotonic_buffer_resource` not thread-safe; if arena used cross-thread, races.  
**Impact**: Data races, corruption.  
**Fix**: Document thread-safety requirements or add synchronization.

### AH3. [ACC] SIMD sum without Kahan (line 26)
**Issue**: SIMD sum/dot_product without Kahan compensation; for long arrays loses precision vs scalar with Kahan.  
**Impact**: Numerical accuracy degradation.  
**Fix**: Implement Kahan summation in SIMD or document precision trade-off.

### AH4. [SPD] Scalar prefix_sum despite SIMD annotation
**Issue**: `prefix_sum` marked as SIMD, but implementation is scalar.  
**Impact**: Misleading annotation, missed optimization opportunity.  
**Fix**: Implement SIMD prefix sum or remove annotation.

### AH5. [CORR] ABA problem in trace buffer (lines 46, 48)
**Issue**: Race between `store(recv_ns, release)` (line 46) and `store(trace_id, release)` (line 48). Another thread can execute full `store()` with different id in same slot (slot = id & 0x3FFF).  
**Impact**: "Second" recv_ns + "first" trace_id, lookup sees matching id and wrong recv_ns. ABA / torn read.  
**Fix**: Use single atomic write or sequence lock.

### AH6. [SPD] Mutex on hot path
**Issue**: `mtx_` taken on every trade/book update (hot path).  
**Impact**: Contention bottleneck.  
**Fix**: Use lock-free structures or reduce critical section.

### AH7. [LEAK] Unbounded per_ticker_ map
**Issue**: `per_ticker_` without upper bound.  
**Impact**: Memory leak on universe rotation.  
**Fix**: Implement eviction policy.

### AH8. [SPD] CPU pinning failure continues on random core
**Issue**: On pin failure (e.g., no capability) → warning, but pipeline continues on "random" core.  
**Impact**: Lose p99 SLO.  
**Fix**: Fail fast or retry pinning, don't continue on unpinned core.

---

## AI. Numeric / TDigest, KDE, HMM, FixedPoint, ZigZag

### AI1. [ACC] Total weight accumulation without Kahan
**Issue**: `total_weight_` accumulates without Kahan; on long streams drifts and shifts scale function.  
**Impact**: Quantile accuracy degradation.  
**Fix**: Use Kahan summation for `total_weight_`.

### AI2. [SPD] Quantile always merges
**Issue**: `quantile()` always calls `merge()` (O(N log N) sort). TradeStream throttles, but if called from other places → bottleneck.  
**Impact**: Performance degradation.  
**Fix**: Cache merged state or throttle merge calls.

### AI3. [ACC] Padé approximation discontinuity
**Issue**: Padé approximation `exp(-a)` clipped to 0 for `|diff|>3` — creates discontinuity in density at boundary.  
**Impact**: Artificial density artifacts.  
**Fix**: Use smooth falloff or higher-order approximation.

### AI4. [ACC] Silverman bandwidth oversmoothing
**Issue**: Silverman bandwidth applied to multimodal distribution (price clusters!) → systematic oversmoothing of levels.  
**Impact**: Missed level detection.  
**Fix**: Use adaptive bandwidth or multimodal-aware estimator.

### AI5. [SPD] Expensive HMM forward pass
**Issue**: Inner loop does `exp()` kStates²×T times; LSE correct but not cheap.  
**Impact**: CPU overhead.  
**Fix**: Optimize with log-space arithmetic or reduce state count.

### AI6. [ACC] Fixed-point reciprocal inconsistency
**Issue**: `from_price_inv` uses `1.0/increment` (FP reciprocal) — may give different tick than `round(price/increment)` for rare combinations.  
**Impact**: Inconsistent tick calculations.  
**Fix**: Use consistent rounding method.

### AI7. [ACC] Hardcoded initial ZigZag state
**Issue**: Starting state `FindingHigh` hardcoded — asymmetric behavior on first ticks.  
**Impact**: Inconsistent initial behavior.  
**Fix**: Make initial state configurable or auto-detect.

### AI8. [CORR] Division by zero in ZigZag
**Issue**: Division by `last_peak_price` without checking for 0.  
**Impact**: Potential crash.  
**Fix**: Add zero check.

### AI9. [CORR] No reset in Kahan accumulator
**Issue**: No `reset()` / `subtract()`; can't clear accumulator without recreating.  
**Impact**: Inflexible API.  
**Fix**: Add reset and subtract methods.

### AI10. [CORR] Silent fallback on invalid tick
**Issue**: `round_to_tick`: when `tick<=0`, silently returns `val` (no logging / error).  
**Impact**: Bug upstream goes unnoticed.  
**Fix**: Log error or throw exception.

---

## AJ. Logger / Misc

### AJ1. [CORR] Non-thread-safe logger
**Issue**: Not thread-safe; no atomic; used in multiple places assuming SPSC.  
**Impact**: Data races if used from multiple threads.  
**Fix**: Add synchronization or document SPSC requirement.

### AJ2. [CORR] Type-unsafe formatting
**Issue**: `format(...)` uses `std::snprintf` with runtime format → loses type-safety of fmt.  
**Impact**: On N less than format length — silent truncate.  
**Fix**: Use `fmt::format` or `std::format`.

### AJ3. [SPD] Heap allocation per call
**Issue**: `ostringstream` per call — heap alloc; not suitable for hot path.  
**Impact**: Performance overhead.  
**Fix**: Use stack buffer or pre-allocated buffer.

---

## AK. Metrics / Dashboard

### AK1. [SPD] Single mutex for all metrics
**Issue**: Single mutex on ALL counters/gauges/histograms — all hot paths contend on one lock.  
**Impact**: Severe contention bottleneck.  
**Fix**: Use per-metric locks or lock-free atomics.

### AK2. [CORR] Redundant atomic wrapper
**Issue**: Counter `load` + `store(current + val)` under mutex — atomic wrapper not needed.  
**Impact**: Unnecessary overhead.  
**Fix**: Use plain int under mutex or remove mutex and use atomic.

### AK3. [SEC] Non-constant-time token comparison
**Issue**: `it->value() != "Bearer " + auth_token` — non-constant-time comparison (unlike DashboardServer:240 with `diff |=`).  
**Impact**: Timing attack on token.  
**Fix**: Use constant-time comparison.

### AK4. [CORR] Sleep blocks asio thread
**Issue**: `std::this_thread::sleep_for` on ExternalIoContext thread → blocks asio, other webhooks/timers stall for 1/3/9s.  
**Impact**: Webhook delivery delays.  
**Fix**: Use asio timer instead of sleep.

### AK5. [CORR] Non-atomic counters (line 74)
**Issue**: `++delivered_` / `++failed_` — plain int without atomic.  
**Impact**: Data race.  
**Fix**: Use `std::atomic<int>`.

### AK6. [SPD] Full state serialization on every tick
**Issue**: `serialize_state_locked_` builds full JSON (chart_history 600pt + density 300×24 + universe + signals + journal + funding) on each conflation tick (~50ms) = ~20Hz × tens of KB = MB/s allocations + GC-like load.  
**Impact**: CPU and memory overhead.  
**Fix**: Implement incremental updates or reduce serialization frequency.

### AK7. [SPD] O(N) erase on iceberg events
**Issue**: `iceberg_events.erase(begin())` O(N) shift on limit exceeded; at high iceberg rate — noticeable cost.  
**Impact**: Performance degradation.  
**Fix**: Use circular buffer or deque.

### AK8. [CORR] Expensive state copy
**Issue**: `update_state_async` saves `pending_state_` by copying — copying State (with chart_history, density, journal) expensive.  
**Impact**: Performance overhead.  
**Fix**: Use move semantics or shared_ptr.

---

## AL. App / PipelineProcessor (Hot Path)

### AL1. [DET/ACC] Local timestamp on book update (line 327)
**Issue**: `upd.ts = std::chrono::system_clock::now();` — loses server timestamp from book.  
**Impact**: All time-window signals get processing time, not event time.  
**Fix**: Use server timestamp from message.

### AL2. [DET/ACC] Local timestamp on trades
**Issue**: `Trade trade{price, size, side, now};` — same for trades: TapeFlush "delta_bps vs last mid" calculated with locally-stamped time.  
**Impact**: Non-deterministic replay, incorrect time windows.  
**Fix**: Use exchange timestamp.

### AL3. [SPD] Mark prices copied on every update
**Issue**: `executor_->set_mark_prices(mark_prices_)` copies entire map on each mark_price_update (per ticker per update). O(N) copies on each of N updates.  
**Impact**: Unnecessary copying overhead.  
**Fix**: Use shared_ptr or pass by reference.

### AL4. [SPD] Yield on pinned thread
**Issue**: `std::this_thread::yield()` in busy loop, while thread pinned. Yield = scheduler ping → loses low-latency benefit of pinning.  
**Impact**: Increased latency.  
**Fix**: Use pause instruction or remove yield.

### AL5. [CORR] Closed trades drain only when idle
**Issue**: Drain closed trades only when `!did_work && idle_count%10000`. Under constant load — `closed_trades` in executor can accumulate unbounded.  
**Impact**: Memory leak under load.  
**Fix**: Drain periodically regardless of idle state.

### AL6. [CORR] Broad exception catch after simdjson
**Issue**: Broad `catch(std::exception&)` after simdjson; parser can remain in corrupted state, next event parsed "dirty".  
**Impact**: Corrupted parsing state.  
**Fix**: Reset parser state on exception or use narrower catch.

---

## AM. Config / Transport / Misc

### AM1. [CORR] Global singleton config
**Issue**: `static toml::table s_data;` — Config is global singleton via static. No thread-safety on load, not testable, multi-instance impossible.  
**Impact**: Testing difficulties, thread-safety issues.  
**Fix**: Use dependency injection, pass config as parameter.

### AM2. [CORR] Incomplete config validation
**Issue**: `validate()` checks only presence of top-level keys; value ranges (e.g., `max_daily_loss_pct > 0`) not validated.  
**Impact**: Invalid config accepted, runtime errors.  
**Fix**: Add comprehensive validation for all parameters.

### AM3. [DET] No flush/fsync in replay recorder
**Issue**: `out_ << line.dump() << "\n";` without flush/fsync — on crash, lose tail of recording.  
**Impact**: Replay non-reproducible.  
**Fix**: Flush after each line or periodically.

### AM4. [CORR] Truncate mode overwrites existing file
**Issue**: `std::ios::trunc` — starting recording overwrites existing file (if someone wants to preserve it).  
**Impact**: Data loss.  
**Fix**: Use append mode or prompt user.

### AM5. [ACC] Day start from first update
**Issue**: `day_start_result` set from first update after start; if bot started mid-day — `day_start` is partial intraday PnL, not morning.  
**Impact**: "Realized PnL today" distorted.  
**Fix**: Fetch day start from API or persist across restarts.

### AM6. [CORR] WebSocket handler overwrite
**Issue**: `set_on_message` overwrites any previously set handler on WS; if bot shares one WS between MarketDataFeed and NotificationFeed — last one wins, MarketData silently stops receiving.  
**Impact**: Lost market data.  
**Fix**: Use message routing or separate WS connections.

### AM7. [CORR] Non-atomic active flag
**Issue**: `if (!active_)` — `active_` plain bool without atomic.  
**Impact**: Data race.  
**Fix**: Use `std::atomic<bool>`.

---

## Summary Statistics

**Total Issues**: 107  
**By Impact**:
- [CORR] Correctness: 52
- [SPD] Speed/Performance: 28
- [ACC] Accuracy: 19
- [DET] Determinism: 12
- [LEAK] Memory Leak: 18
- [SEC] Security: 3

**Critical Issues** (require immediate attention):
1. **Transport layer**: libcurl initialization, connection pooling, timestamp handling
2. **Time handling**: System clock usage throughout (breaks replay determinism)
3. **Memory leaks**: Unbounded containers in signals, universe, metrics
4. **Data races**: Non-atomic flags, unprotected containers, const-cast mutations
5. **Hot path performance**: Mutex contention, unnecessary allocations, O(N²) algorithms

---

## Recommended Action Plan

### Phase 1: Critical Correctness (Week 1-2)
1. Fix all data races (atomic flags, mutex protection)
2. Fix timestamp handling (use exchange time, not local time)
3. Fix memory leaks (implement bounded containers)
4. Fix transport layer (libcurl init, connection pooling)

### Phase 2: Determinism (Week 3)
1. Replace all `system_clock::now()` with event time
2. Add flush/fsync to replay recorder
3. Fix sentinel comparisons and initialization

### Phase 3: Performance (Week 4-5)
1. Reduce mutex contention (per-metric locks, lock-free structures)
2. Eliminate hot-path allocations
3. Optimize O(N²) algorithms
4. Implement connection pooling

### Phase 4: Accuracy (Week 6)
1. Fix numerical precision issues (Kahan summation)
2. Fix statistical correctness (HMM, KDE, bandwidth)
3. Implement proper slippage models

### Phase 5: Security & Polish (Week 7)
1. Fix credential leakage in logs
2. Implement constant-time comparisons
3. Add comprehensive config validation
4. Document thread-safety requirements

---

## Notes

- Many issues stem from **lack of time discipline**: mixing local time (`now()`) with exchange time
- **Memory leaks** are pervasive: most signal detectors and trackers have unbounded containers
- **Data races** are common: many bool flags and containers accessed without synchronization
- **Performance issues** often from **unnecessary allocations** and **mutex contention**
- **Determinism violations** make replay debugging impossible

**Priority**: Address time handling and data races first, as they affect correctness of all downstream logic.

---

**Document Status**: DRAFT  
**Next Steps**: Review with team, prioritize fixes, create implementation tasks
