#pragma once

#include "IClock.hpp"
#include "MarketDataFeed.hpp"   // IMarketDataListener

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace trade_bot {

/**
 * T0-REPLAY: deterministic replay of a recorded MetaScalp WebSocket session.
 *
 * Input format (one JSON object per line, NDJSON):
 *   {"recv_ts_ns": <int>, "message": <raw WS JSON object>}
 *
 * `recv_ts_ns` is the wall-clock timestamp (nanoseconds since UNIX epoch) at
 * which the bot received the message during recording. ReplayFeed reproduces
 * the inter-arrival timing scaled by `speed_multiplier`:
 *   - 1.0  → real-time
 *   - 0.0  → as-fast-as-possible (no sleeps)
 *   - 2.0  → 2× faster
 *
 * Each parsed event is routed to all registered IMarketDataListener subscribers
 * using the same dispatcher table as MarketDataFeed, but driven from the file.
 */
class ReplayFeed {
public:
    struct Stats {
        size_t messages_read{0};
        size_t messages_dispatched{0};
        size_t parse_errors{0};
    };

    ReplayFeed(std::string ndjson_path,
               std::shared_ptr<IClock> clock,
               double speed_multiplier = 1.0);

    void add_listener(IMarketDataListener* listener);
    void remove_listener(IMarketDataListener* listener);

    /// Block until file exhausted or `stop()` called. Returns final Stats.
    Stats run();

    void stop();

    bool   running() const { return running_.load(); }
    Stats  stats()   const;

private:
    void dispatch_message_(const std::string& raw_message, int64_t recv_ts_ns);

    std::vector<IMarketDataListener*> snapshot_listeners_();

    std::string                       path_;
    std::shared_ptr<IClock>           clock_;
    double                            speed_;
    std::vector<IMarketDataListener*> listeners_;
    mutable std::recursive_mutex      mtx_;
    std::atomic<bool>                 running_{false};
    Stats                             stats_;
    int64_t                           system_time_offset_ns_{0};
};

}  // namespace trade_bot
