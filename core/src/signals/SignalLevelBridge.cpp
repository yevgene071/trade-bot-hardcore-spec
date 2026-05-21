#include "SignalLevelBridge.hpp"
#include "logger/Logger.hpp"
#include <cmath>

namespace trade_bot {

SignalLevelBridge::SignalLevelBridge(SignalLevelGateway& gateway,
                                   SignalBus& bus,
                                   Config cfg)
    : gateway_(gateway)
    , bus_(bus)
    , cfg_(cfg) {
    if (cfg_.enabled) {
        subscription_id_ = bus_.subscribe([this](const Signal& s) { // AD1: store ID for unsubscribe
            if (s.kind == SignalKind::LevelFormed) {
                on_level_formed(s.ticker, s.price, s.timestamp); // AD3: pass event timestamp
            }
        });
    }
}

SignalLevelBridge::SignalLevelBridge(SignalLevelGateway& gateway,
                                   SignalBus& bus)
    : SignalLevelBridge(gateway, bus, Config{}) {}

SignalLevelBridge::~SignalLevelBridge() { // AD1: unsubscribe so stale lambda can't fire after dtor
    if (cfg_.enabled) {
        bus_.unsubscribe(subscription_id_);
    }
}

void SignalLevelBridge::on_level_formed(const Ticker& ticker, double price, 
                                       std::chrono::system_clock::time_point timestamp,
                                       double current_mid) {
    if (!cfg_.enabled) return;

    int64_t evict_id = -1;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (active_levels_.size() >= static_cast<size_t>(cfg_.max_server_levels)) {
            // Eviction priority: triggered > farthest from mid > oldest
            auto victim = active_levels_.end();

            // 1. Prefer any triggered level
            for (auto it = active_levels_.begin(); it != active_levels_.end(); ++it) {
                if (it->second.triggered) { victim = it; break; }
            }

            // 2. Else: farthest from current_mid (if mid available)
            if (victim == active_levels_.end() && current_mid > 0.0) {
                double max_dist = -1.0;
                for (auto it = active_levels_.begin(); it != active_levels_.end(); ++it) {
                    double d = std::abs(it->second.price - current_mid) / current_mid;
                    if (d > max_dist) { max_dist = d; victim = it; }
                }
            }

            // 3. Else: oldest by created_at
            if (victim == active_levels_.end()) {
                for (auto it = active_levels_.begin(); it != active_levels_.end(); ++it) {
                    if (victim == active_levels_.end() ||
                        it->second.created_at < victim->second.created_at) {
                        victim = it;
                    }
                }
            }

            if (victim != active_levels_.end()) {
                evict_id = victim->first; // AD2: defer HTTP call to outside the lock
                active_levels_.erase(victim);
            }
        }
    }

    if (evict_id >= 0) {
        gateway_.remove(evict_id); // AD2: synchronous HTTP outside mtx_ — no deadlock for other publishers
    }

    int64_t id = gateway_.create(ticker, price);
    if (id > 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        active_levels_[id] = {id, ticker, price, timestamp}; // AD3: use event timestamp
        LOG_INFO("SignalLevelBridge: created server-side level {} for {} at {}", id, ticker, price);
    }
}

void SignalLevelBridge::on_server_trigger(int64_t id, const Ticker& ticker, double price,
                                         std::chrono::system_clock::time_point timestamp) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = active_levels_.find(id);
        if (it != active_levels_.end()) it->second.triggered = true;
    }

    Signal s {
        .kind = SignalKind::LevelBreak, // or Approach, but spec says non-polling
        .timestamp = timestamp, // AD3: use event timestamp from server notification
        .ticker = ticker,
        .price = price,
        .confidence = 1.0,
        .payload = {
            .id = FixedString<16>::format("%lld", id),
            .source = "server"
        }
    };
    bus_.publish(s);
    LOG_INFO("SignalLevelBridge: server level {} triggered for {}", id, ticker);
}

} // namespace trade_bot
