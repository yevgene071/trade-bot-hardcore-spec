#include "SignalLevelBridge.hpp"
#include "logger/Logger.hpp"

namespace trade_bot {

SignalLevelBridge::SignalLevelBridge(SignalLevelGateway& gateway,
                                   SignalBus& bus,
                                   Config cfg)
    : gateway_(gateway)
    , bus_(bus)
    , cfg_(cfg) {}

SignalLevelBridge::SignalLevelBridge(SignalLevelGateway& gateway,
                                   SignalBus& bus)
    : SignalLevelBridge(gateway, bus, Config{}) {}

void SignalLevelBridge::on_level_formed(const Ticker& ticker, double price) {
    if (!cfg_.enabled) return;

    // LRU-eviction simplified: if too many, remove first one
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (active_levels_.size() >= static_cast<size_t>(cfg_.max_server_levels)) {
            auto it = active_levels_.begin();
            gateway_.remove(it->first);
            active_levels_.erase(it);
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
        active_levels_.erase(id);
    }

    Signal s {
        .kind = SignalKind::LevelBreak, // or Approach, but spec says non-polling
        .timestamp = std::chrono::system_clock::now(),
        .ticker = ticker,
        .price = price,
        .confidence = 1.0,
        .payload = nlohmann::json{{"id", id}, {"source", "server"}}
    };
    bus_.publish(s);
    LOG_INFO("SignalLevelBridge: server level {} triggered for {}", id, ticker);
}

} // namespace trade_bot
