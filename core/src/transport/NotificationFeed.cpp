#include "NotificationFeed.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "metrics/MetricsRegistry.hpp"
#include "signals/SignalLevelBridge.hpp"

namespace trade_bot {

namespace {

const nlohmann::json* signal_levels_array(const nlohmann::json& data) {
    if (data.is_array()) return &data;
    if (data.is_object()) {
        if (data.contains(api::fields::kSignalLevels) &&
            data[api::fields::kSignalLevels].is_array()) {
            return &data[api::fields::kSignalLevels];
        }
        if (data.contains("signalLevels") && data["signalLevels"].is_array()) {
            return &data["signalLevels"];
        }
    }
    return nullptr;
}

std::vector<SignalLevel> parse_signal_levels(const nlohmann::json& data) {
    std::vector<SignalLevel> levels;
    const auto* arr = signal_levels_array(data);
    if (!arr) return levels;
    levels.reserve(arr->size());
    for (const auto& item : *arr) {
        levels.push_back(MetaScalpCodec::parse_signal_level(item));
    }
    return levels;
}

} // namespace

NotificationFeed::NotificationFeed(std::shared_ptr<IWsClient> ws_client,
                                   TickerUniverse& universe,
                                   Config cfg,
                                   std::function<void()> on_first_coin_callback,
                                   SignalLevelBridge* signal_level_bridge)
    : ws_client_(std::move(ws_client))
    , universe_(universe)
    , cfg_(cfg)
    , on_first_coin_callback_(std::move(on_first_coin_callback))
    , signal_level_bridge_(signal_level_bridge) {}

void NotificationFeed::start() {
    const bool subscribe_signal_levels = signal_level_bridge_ && cfg_.signal_levels_subscribe;
    if (!cfg_.subscribe && !subscribe_signal_levels) return;
    
    active_ = true;
    ws_client_->set_on_message([this](const nlohmann::json& j, uint64_t /*recv_ns*/, TraceId /*tid*/) {
        if (!active_) return;
        handle_message_(j);
    });

    if (cfg_.subscribe) {
        nlohmann::json sub = {
            {"Type", "notification_subscribe"},
            {"Data", nlohmann::json::object()}
        };
        ws_client_->send(sub.dump());
        LOG_INFO("NotificationFeed: subscribed to app-wide notifications");
    }
    if (subscribe_signal_levels) {
        ws_client_->send(nlohmann::json{
            {"Type", "signal_level_subscribe"},
            {"Data", nlohmann::json::object()}
        }.dump());
        LOG_INFO("NotificationFeed: subscribed to signal level updates");
    }
}

void NotificationFeed::stop() {
    active_ = false;
}

void NotificationFeed::handle_message_(const nlohmann::json& j) {
    const std::string type = j.value("Type", "");
    static const nlohmann::json kEmptyData = nlohmann::json::object();
    const auto data_it = j.find("Data");
    const nlohmann::json& data = data_it != j.end() ? *data_it : kEmptyData;

    // notification_subscribed is just an ack — no data to process
    if (type == "notification_subscribed") {
        LOG_INFO("NotificationFeed: subscription confirmed");
        return;
    }
    if (type == "signal_level_subscribed") {
        LOG_INFO("NotificationFeed: signal level subscription confirmed");
        return;
    }

    if (type == "signal_levels_snapshot") {
        if (signal_level_bridge_) {
            signal_level_bridge_->on_server_snapshot(parse_signal_levels(data));
        }
        return;
    }

    if (type == "signal_level_placed") {
        if (signal_level_bridge_ && data.is_object()) {
            signal_level_bridge_->on_server_level_placed(
                MetaScalpCodec::parse_signal_level(data));
        }
        return;
    }

    if (type == "signal_level_triggered") {
        if (signal_level_bridge_ && data.is_object()) {
            const auto level = MetaScalpCodec::parse_signal_level(data);
            signal_level_bridge_->on_server_trigger(
                level.id, level.ticker, level.price, level.created_at);
        }
        return;
    }

    if (type == "signal_level_removed") {
        if (signal_level_bridge_ && data.is_object()) {
            signal_level_bridge_->on_server_removed(
                MetaScalpCodec::get_val<int64_t>(data, api::fields::kId, 0L));
        }
        return;
    }

    if (type == "signal_levels_removed_all") {
        if (signal_level_bridge_) {
            signal_level_bridge_->on_server_removed_all(
                MetaScalpCodec::normalize_ticker(
                    MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, "")));
        }
        return;
    }

    if (type == "signal_levels_removed_triggered") {
        if (signal_level_bridge_) {
            signal_level_bridge_->on_server_removed_triggered();
        }
        return;
    }

    if (type == "notification_snapshot" || type == "notification_update") {
        // MetaScalp sends notifications wrapped in {"Notifications": [...]} or {"notifications": [...]}
        const nlohmann::json* notifications_ptr = nullptr;
        if (data.is_object()) {
            if (data.contains("Notifications")) {
                notifications_ptr = &data["Notifications"];
            } else if (data.contains("notifications")) {
                notifications_ptr = &data["notifications"];
            }
        }

        if (notifications_ptr && notifications_ptr->is_array()) {
            for (const auto& item : *notifications_ptr) {
                route_notification_(MetaScalpCodec::parse_notification(item));
            }
        } else if (data.is_object() && !notifications_ptr) {
            // Fallback: bare notification object directly in Data (backward compat)
            route_notification_(MetaScalpCodec::parse_notification(data));
        } else if (data.is_array()) {
            // Some APIs may send a bare array — handle for compatibility
            for (const auto& item : data) {
                route_notification_(MetaScalpCodec::parse_notification(item));
            }
        } else {
            LOG_WARN("NotificationFeed: unexpected {} data format: {}", type, data.dump(0));
        }
        return;
    }

    // Ignore heartbeat/pong/other messages silently (they're frequent)
    if (type != "pong" && type != "heartbeat") {
        LOG_DEBUG("NotificationFeed: unhandled message type: {}", type);
    }
}

void NotificationFeed::route_notification_(const Notification& n) {
    // Filter by exchange/market type only when explicitly configured (non-zero).
    if (cfg_.exchange_id != 0 && n.exchange_id != cfg_.exchange_id) {
        dropped_wrong_connection_++;
        MetricsRegistry::instance().counter_inc("trade_bot_notification_dropped_total");
        return;
    }
    if (cfg_.market_type != 0 && n.market_type != cfg_.market_type) {
        dropped_wrong_connection_++;
        MetricsRegistry::instance().counter_inc("trade_bot_notification_dropped_total");
        return;
    }

    switch (n.kind) {
        case NotificationKind::BigTick:
            universe_.on_big_tick(n.ticker, n.size * n.price, n.timestamp);
            break;
        case NotificationKind::BigOrderBookAmount:
        case NotificationKind::BigOrderBookAmount2:
            universe_.on_big_amount(n.ticker, n.size * n.price, n.timestamp);
            break;
        case NotificationKind::ScreenerNewCoin:
            LOG_INFO("[Universe] NotificationFeed: ScreenerNewCoin for {}", n.ticker);
            universe_.on_screener_new_coin(n.ticker, n.timestamp);
            
            // Fire callback on first coin (start processor after initial pool populated)
            if (on_first_coin_callback_ && !first_coin_fired_.exchange(true)) {
                on_first_coin_callback_();
            }
            break;
        case NotificationKind::SignalLevel:
            // AD3: Route SignalLevel notifications to bridge with event timestamp
            if (signal_level_bridge_) {
                signal_level_bridge_->on_server_trigger(
                    n.level_id, n.ticker, n.price, n.timestamp);
            }
            break;
        default:
            // Trade notifications are ignored or routed elsewhere
            break;
    }
}

} // namespace trade_bot
