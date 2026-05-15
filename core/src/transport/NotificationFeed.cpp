#include "NotificationFeed.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "metrics/MetricsRegistry.hpp"

namespace trade_bot {

NotificationFeed::NotificationFeed(std::shared_ptr<IWsClient> ws_client,
                                   TickerUniverse& universe,
                                   Config cfg)
    : ws_client_(std::move(ws_client))
    , universe_(universe)
    , cfg_(cfg) {}

void NotificationFeed::start() {
    if (!cfg_.subscribe) return;
    
    active_ = true;
    ws_client_->set_on_message([this](const nlohmann::json& j) {
        if (!active_) return;
        handle_message_(j);
    });

    // Send subscription message
    nlohmann::json sub = {
        {"Type", "notification_subscribe"},
        {"Data", nlohmann::json::object()}
    };
    ws_client_->send(sub.dump());
    LOG_INFO("NotificationFeed: subscribed to app-wide notifications");
}

void NotificationFeed::stop() {
    active_ = false;
}

void NotificationFeed::handle_message_(const nlohmann::json& j) {
    const std::string type = j.value("Type", "");

    // notification_subscribed is just an ack — no data to process
    if (type == "notification_subscribed") {
        LOG_INFO("NotificationFeed: subscription confirmed");
        return;
    }

    if (type == "notification_snapshot" || type == "notification_update") {
        const auto& data = j["Data"];

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
            universe_.on_screener_new_coin(n.ticker);
            break;
        default:
            // Trade and SignalLevel are ignored or routed elsewhere
            break;
    }
}

} // namespace trade_bot
