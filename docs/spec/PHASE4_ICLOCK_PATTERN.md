# Phase 4: IClock Pattern for Deterministic Replay

## Overview

**P0 Determinism Fix**: All strategies must use injected `IClock` instead of `std::chrono::system_clock::now()` to enable deterministic replay.

## Implementation Status

### ✅ Completed
- Created `core/src/utils/IClock.hpp` with:
  - `IClock` interface
  - `SystemClock` (production)
  - `ReplayClock` (replay mode with injectable timestamp)
- Updated `IStrategy` interface with `set_clock()` method
- Updated `BounceFromDensity.hpp` as reference implementation

### 🔄 Remaining Work

All strategies must be updated to use `clock_` instead of direct `std::chrono::system_clock::now()` calls:

1. **BreakoutEatThrough** (2 occurrences in .cpp)
2. **LeaderLag** (1 occurrence in .cpp)
3. **FlushReversal** (2 occurrences in .cpp)
4. **BounceFromDensity** (2 occurrences in .cpp) - header updated, .cpp needs changes

## Pattern

### 1. Header File (.hpp)

```cpp
#include "IStrategy.hpp"
#include "utils/IClock.hpp"  // Add this include

class MyStrategy : public IStrategy {
public:
    // ... existing methods ...
    
    void set_clock(const IClock* clock) override { clock_ = clock; }
    
private:
    const IClock* clock_{nullptr};  // Add this member
};
```

### 2. Implementation File (.cpp)

Replace all occurrences of:
```cpp
auto now = std::chrono::system_clock::now();
```

With:
```cpp
auto now = clock_ ? clock_->now() : std::chrono::system_clock::now();
```

**Rationale**: Fallback to `system_clock` ensures backward compatibility if `set_clock()` is not called (e.g., in unit tests).

### 3. Strategy Instantiation (BotApp)

After creating each strategy, inject the clock:

```cpp
// Production
SystemClock system_clock;
strategy->set_clock(&system_clock);

// Replay
ReplayClock replay_clock(initial_timestamp);
strategy->set_clock(&replay_clock);
// Update replay_clock.set_time() on each event
```

## Files to Update

### Strategy Headers (add clock member + set_clock)
- ✅ `core/src/strategy/BounceFromDensity.hpp`
- ⏳ `core/src/strategy/BreakoutEatThrough.hpp`
- ⏳ `core/src/strategy/LeaderLag.hpp`
- ⏳ `core/src/strategy/FlushReversal.hpp`

### Strategy Implementations (replace system_clock::now)
- ⏳ `core/src/strategy/BounceFromDensity.cpp` (lines 382, 440)
- ⏳ `core/src/strategy/BreakoutEatThrough.cpp` (lines 281, 335)
- ⏳ `core/src/strategy/LeaderLag.cpp` (line 277)
- ⏳ `core/src/strategy/FlushReversal.cpp` (lines 235, 281)

### BotApp Integration
- ⏳ `core/src/app/BotApp.cpp` - inject `SystemClock` into all strategies after creation

## Testing

After implementation:

1. **Unit tests**: Verify strategies work with `ReplayClock`
2. **Integration test**: Run `replay_determinism_test` with full pipeline
3. **Production**: Verify no behavioral changes (SystemClock is default)

## Related

- **Audit Report**: `docs/spec/PHASE3_DETERMINISM_AUDIT.md` § 2 (System Clock Usage)
- **IClock Header**: `core/src/utils/IClock.hpp`
- **IStrategy Interface**: `core/src/strategy/IStrategy.hpp`

---

**Last Updated**: 2026-05-18  
**Status**: Pattern documented, partial implementation complete
