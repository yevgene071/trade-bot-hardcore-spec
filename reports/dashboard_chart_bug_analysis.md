# Dashboard Chart Bug Analysis

**Date**: 2026-05-18  
**Issue**: Dashboard shows "waiting for data" - `chart_history` and `density_history` are empty

## Root Cause

Found critical bug in `core/src/app/BotAppRun.cpp` lines 405-407:

```cpp
const bool slow_tick = events_occurred ||
    (now - last_slow_dash_update_ > std::chrono::milliseconds(100));
if (slow_tick) rebuild_slow_dash_cache_(now);
```

**The Problem**: `last_slow_dash_update_` is updated INSIDE `rebuild_slow_dash_cache_()` at line 245:

```cpp
void BotApp::rebuild_slow_dash_cache_(std::chrono::system_clock::time_point now) {
    // ... code ...
    last_slow_dash_update_ = now;  // Line 245
    // ... more code including chart_snapshot() and density_snapshot() ...
}
```

**Why This Breaks**:
1. First tick: `slow_tick = true` (default-initialized timestamp)
2. Calls `rebuild_slow_dash_cache_(now)` which updates `last_slow_dash_update_ = now`
3. **Next ticks**: The condition `(now - last_slow_dash_update_ > 100ms)` will NEVER be true again because:
   - The timestamp was just updated to `now`
   - On next tick, `now - last_slow_dash_update_` will be ~1ms (tick interval)
   - This is always < 100ms
4. Result: `chart_snapshot()` and `density_snapshot()` are only called ONCE at startup, then never again (unless `events_occurred` is true)

## Impact

- `cached_chart_history_` and `cached_density_history_` are populated only once at startup
- If bot just started, these arrays are empty (no historical data yet)
- Dashboard receives empty arrays and shows "waiting for data"
- Charts never update because `slow_tick` never triggers again

## Fix

Move `last_slow_dash_update_ = now` to AFTER the `rebuild_slow_dash_cache_()` call:

```cpp
const bool slow_tick = events_occurred ||
    (now - last_slow_dash_update_ > std::chrono::milliseconds(100));
if (slow_tick) {
    rebuild_slow_dash_cache_(now);
    last_slow_dash_update_ = now;  // Move here
}
```

Or remove line 245 from `rebuild_slow_dash_cache_()` and update the timestamp in the caller.

## Files to Modify

1. `core/src/app/BotAppRun.cpp` - Line 245 (remove) and line 407 (add timestamp update)
2. Alternatively: Keep line 245 but remove it from line 407 logic

## Testing

After fix:
1. Start bot
2. Wait 100ms
3. Verify `chart_history` and `density_history` are populated in `/api/state`
4. Verify dashboard charts appear

## Related Code

- `BotAppRun.cpp:164-247` - `rebuild_slow_dash_cache_()` function
- `BotAppRun.cpp:401-412` - Dashboard update logic in `tick()`
- `BotAppRun.cpp:226-227` - Chart/density snapshot calls
- `DashboardServer.cpp:291-293` - Serialization of chart/density data
