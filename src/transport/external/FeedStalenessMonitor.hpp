#pragma once

#include "ExternalFeedRegistry.hpp"
#include "control/KillSwitch.hpp"
#include "logger/Logger.hpp"
#include <chrono>

namespace trade_bot {

class FeedStalenessMonitor {
public:
    struct Config {
        std::chrono::seconds warn_age{120};
        std::chrono::seconds kill_age{1000};
        bool kill_on_staleness{false};
    };

    static void check_all(const std::map<std::string, Config>& configs) {
        auto& registry = ExternalFeedRegistry::instance();
        for (const auto& [kind, feed] : registry.all_feeds()) {
            std::string name = feed->name();
            auto it = configs.find(name);
            if (it == configs.end()) continue;

            const auto& cfg = it->second;
            if (feed->is_stale(cfg.warn_age)) {
                LOG_WARN("ExternalFeed: feed {} is STALE", name);
                
                if (cfg.kill_on_staleness && feed->is_stale(cfg.kill_age)) {
                    LOG_CRITICAL("ExternalFeed: feed {} is CRITICALLY STALE, triggering kill-switch", name);
                    KillSwitch::instance().trigger(KillReason::FeedStaleness);
                }
            }
        }
    }
};

} // namespace trade_bot
