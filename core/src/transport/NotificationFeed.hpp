#pragma once

#include "IWsClient.hpp"
#include "universe/TickerUniverse.hpp"
#include "domain/Types.hpp"

#include <memory>
#include <mutex>
#include <atomic>

namespace trade_bot {

class SignalLevelBridge; // Forward declaration

/**
 * T1-NOTIF: Handles app-wide notification WS stream.
 */
class NotificationFeed {
public:
    struct Config {
        bool subscribe{true};
        int  exchange_id{0};
        int  market_type{0};
    };

    NotificationFeed(std::shared_ptr<IWsClient> ws_client,
                     TickerUniverse& universe,
                     Config cfg,
                     std::function<void()> on_first_coin_callback = nullptr,
                     SignalLevelBridge* signal_level_bridge = nullptr);

    void start();
    void stop();

    uint64_t dropped_wrong_connection_total() const noexcept {
        return dropped_wrong_connection_.load();
    }

private:
    void handle_message_(const nlohmann::json& j);
    void route_notification_(const Notification& n);

    std::shared_ptr<IWsClient> ws_client_;
    TickerUniverse&           universe_;
    Config                    cfg_;
    std::function<void()>     on_first_coin_callback_;
    SignalLevelBridge*        signal_level_bridge_{nullptr};
    
    std::atomic<bool>         active_{false};
    std::atomic<bool>         first_coin_fired_{false};
    std::atomic<uint64_t>     dropped_wrong_connection_{0};
};

} // namespace trade_bot
