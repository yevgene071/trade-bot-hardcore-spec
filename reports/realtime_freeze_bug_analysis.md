# Real-Time Freeze Bug Analysis

**Date**: 2026-05-18  
**Issue**: Dashboard feels like it "freezes" - not updating in real-time

## Root Cause Analysis

### Current Update Flow

1. **BotAppRun.cpp:408** - Main tick loop (1Hz):
   ```cpp
   if (events_occurred || (now - last_dash_update_ > std::chrono::milliseconds(10))) {
       if (dashboard_->session_count() > 0) {
           const bool slow_tick = events_occurred ||
               (now - last_slow_dash_update_ > std::chrono::milliseconds(100));
           if (slow_tick) {
               rebuild_slow_dash_cache_(now);
               last_slow_dash_update_ = now;
           }
           send_dashboard_state_(now, active, upnl_sum, leader_ticker, all_marks, slow_tick);
       }
       last_dash_update_ = now;
   }
   ```

2. **Problem**: The condition `(now - last_dash_update_ > 10ms)` will NEVER trigger because:
   - Main tick loop runs at 1Hz (1000ms interval)
   - By the time next tick arrives, `(now - last_dash_update_)` is ~1000ms
   - So this condition is always true, but it doesn't matter because we only tick once per second!

3. **The Real Issue**: We're only ticking once per second (1Hz), but trying to send dashboard updates every 10ms. This is impossible!

### The Actual Bug

The main `tick()` function is scheduled at 1Hz (line 43 in BotAppRun.cpp):
```cpp
timer_.expires_after(std::chrono::milliseconds(1000));  // 1 second!
```

This means:
- Dashboard updates can only happen once per second (at most)
- The `10ms` check is meaningless - we never get there that fast
- Charts update at 1Hz, not 10Hz or 100Hz
- This explains the "freeze" feeling - updates are too slow!

### Expected Behavior

For real-time feel, we need:
- Fast ticks (10-50ms) for dashboard updates
- Slow ticks (1s) for account sync, persistence, etc.

### Solution Options

**Option 1**: Separate timers (recommended)
- Keep 1Hz timer for slow operations (account sync, persistence)
- Add separate 50ms timer for dashboard updates only
- This matches the 50ms conflation timer in DashboardServer

**Option 2**: Increase main tick rate
- Change main timer to 50ms or 100ms
- Risk: More CPU usage for account sync operations

**Option 3**: Event-driven dashboard updates
- Update dashboard on every OrderBook/Trade event
- Use conflation timer (50ms) to batch updates
- This is what the code INTENDED but isn't implemented

## Current State

- Main tick: 1Hz (1000ms)
- Dashboard conflation: 50ms (works correctly)
- Chart rebuild: 100ms (now fixed, but only triggers once per second!)
- Result: Dashboard updates at 1Hz maximum, feels frozen

## Recommendation

Implement Option 1: Add a separate 50ms timer for dashboard updates that calls `send_dashboard_state_()` independently of the main tick loop. Keep the 1Hz timer for slow operations.
