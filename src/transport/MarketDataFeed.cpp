#include "MarketDataFeed.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <algorithm>

namespace trade_bot {

MarketDataFeed::MarketDataFeed(std::shared_ptr<IWsClient> ws_client, int connection_id)
    : m_ws_client(std::move(ws_client))
    , m_connection_id(connection_id) {
    
    m_ws_client->set_on_message([this](const nlohmann::json& j) {
        handle_message(j);
    });

    m_ws_client->set_on_connect([this]() {
        if (m_active) {
            resubscribe_all();
        }
    });
}

void MarketDataFeed::add_listener(IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.push_back(listener);
}

void MarketDataFeed::remove_listener(IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
}

void MarketDataFeed::subscribe_ticker(const Ticker& ticker) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribed_tickers.insert(ticker);
    }

    if (m_ws_client->is_connected()) {
        nlohmann::json trade_sub = {
            {"Type", "trade_subscribe"},
            {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ticker}}}
        };
        m_ws_client->send(trade_sub.dump());

        nlohmann::json ob_sub = {
            {"Type", "orderbook_subscribe"},
            {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ticker}}}
        };
        m_ws_client->send(ob_sub.dump());
    }
}

void MarketDataFeed::unsubscribe_ticker(const Ticker& ticker) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribed_tickers.erase(ticker);
    }
    // MetaScalp might not have explicit unsubscribe for trades/orderbook via WS, 
    // usually it's handled by closing connection or just ignoring.
    // But we'll send it if documented.
}

void MarketDataFeed::start() {
    m_active = true;
    if (m_ws_client->is_connected()) {
        resubscribe_all();
    }
}

void MarketDataFeed::stop() {
    m_active = false;
}

void MarketDataFeed::resubscribe_all() {
    LOG_INFO("Resubscribing all tickers and connection updates...");
    
    if (!m_ws_client) {
        LOG_ERROR("m_ws_client is null in resubscribe_all");
        return;
    }

    // Connection level subscribe (orders, positions, balances, finres)
    nlohmann::json conn_sub = {
        {"Type", "subscribe"},
        {"Data", {{"ConnectionId", m_connection_id}}}
    };
    m_ws_client->send(conn_sub.dump());

    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& ticker : m_subscribed_tickers) {
        nlohmann::json trade_sub = {
            {"Type", "trade_subscribe"},
            {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ticker}}}
        };
        m_ws_client->send(trade_sub.dump());

        nlohmann::json ob_sub = {
            {"Type", "orderbook_subscribe"},
            {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ticker}}}
        };
        m_ws_client->send(ob_sub.dump());
    }
}

void MarketDataFeed::handle_message(const nlohmann::json& j) {
    if (!j.contains("Type")) return;

    std::string type = j["Type"];
    nlohmann::json data = j.value("Data", nlohmann::json::object());

    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (type == "trade_update") {
            auto trades = MetaScalpCodec::parse_trade_update(data);
            Ticker ticker = data.value("Ticker", "");
            for (const auto& t : trades) {
                for (auto* l : m_listeners) l->on_trade(ticker, t);
            }
        } else if (type == "orderbook_snapshot") {
            Ticker ticker = data.value("Ticker", "");
            auto snap = MetaScalpCodec::parse_orderbook_snapshot(data, ticker);
            for (auto* l : m_listeners) l->on_orderbook_snapshot(snap);
        } else if (type == "orderbook_update") {
            Ticker ticker = data.value("Ticker", "");
            auto upd = MetaScalpCodec::parse_orderbook_update(data, ticker);
            for (auto* l : m_listeners) l->on_orderbook_update(upd);
        } else if (type == "order_update") {
            auto upd = MetaScalpCodec::parse_order_update(data);
            for (auto* l : m_listeners) l->on_order_update(upd);
        } else if (type == "position_update") {
            auto upd = MetaScalpCodec::parse_position_update(data);
            for (auto* l : m_listeners) l->on_position_update(upd);
        } else if (type == "balance_update") {
            auto upd = MetaScalpCodec::parse_balance_update(data);
            for (auto* l : m_listeners) l->on_balance_update(upd);
        } else if (type == "error") {
            std::string msg = data.value("Message", "Unknown error");
            LOG_ERROR("WS API error: {}", msg);
            for (auto* l : m_listeners) l->on_error(msg);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Error handling WS message {}: {}", type, e.what());
    }
}

} // namespace trade_bot
