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
        bus_.subscribe([this](const Signal& s) {
            if (s.kind == SignalKind::LevelFormed) {
                on_level_formed(s.ticker, s.price);
            }
        });
    }
}

SignalLevelBridge::SignalLevelBridge(SignalLevelGateway& gateway,
                                   SignalBus& bus)
    : SignalLevelBridge(gateway, bus, Config{}) {}

void SignalLevelBridge::on_level_formed(const Ticker& ticker, double price, double current_mid) {
    if (!cfg_.enabled) return;

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
                gateway_.remove(victim->first);
                active_levels_.erase(victim);
            }
        }
    }

    int id = gateway_.create(ticker, price);
    if (id > 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        active_levels_[id] = {id, ticker, price, std::chrono::system_clock::now()};
        LOG_INFO("SignalLevelBridge: created server-side level {} for {} at {}", id, ticker, price);
    }
}

void SignalLevelBridge::on_server_trigger(int id, const Ticker& ticker, double price) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = active_levels_.find(id);
        if (it != active_levels_.end()) it->second.triggered = true;
    }

    Signal s {
        .kind = SignalKind::LevelBreak, // or Approach, but spec says non-polling
        .timestamp = std::chrono::system_clock::now(),
        .ticker = ticker,
        .price = price,
        .confidence = 1.0,
        .payload = {
            .id = FixedString<16>::format("%d", id),
            .source = "server"
        }
    };
    bus_.publish(s);
    LOG_INFO("SignalLevelBridge: server level {} triggered for {}", id, ticker);
}

} // namespace trade_bot
