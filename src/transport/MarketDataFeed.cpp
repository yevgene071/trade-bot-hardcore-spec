#include "MarketDataFeed.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <algorithm>
#include <chrono>

namespace trade_bot {

MarketDataFeed::MarketDataFeed(std::shared_ptr<IWsClient> ws_client, int connection_id)
    : m_ws_client(std::move(ws_client))
    , m_connection_id(connection_id) {
    
    m_ws_client->set_on_message([this](const nlohmann::json& j) {
        if (m_record_tap) {
            using namespace std::chrono;
            auto ts_ns = duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count();
            m_record_tap(j, ts_ns);
        }
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
    invalidate_cache_();
}

void MarketDataFeed::add_listener(const Ticker& ticker, IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ticker_listeners[ticker].push_back(listener);
    invalidate_cache_();
}

void MarketDataFeed::remove_listener(IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
    for (auto it = m_ticker_listeners.begin(); it != m_ticker_listeners.end(); ) {
        it->second.erase(std::remove(it->second.begin(), it->second.end(), listener), it->second.end());
        if (it->second.empty()) {
            it = m_ticker_listeners.erase(it);
        } else {
            ++it;
        }
    }
    invalidate_cache_();
}

void MarketDataFeed::remove_listener(const Ticker& ticker, IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (auto it = m_ticker_listeners.find(ticker); it != m_ticker_listeners.end()) {
        it->second.erase(std::remove(it->second.begin(), it->second.end(), listener), it->second.end());
        if (it->second.empty()) {
            m_ticker_listeners.erase(it);
        }
    }
    invalidate_cache_();
}

void MarketDataFeed::invalidate_cache_() {
    m_merged_cache.clear();
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

std::vector<IMarketDataListener*> MarketDataFeed::get_target_listeners(const Ticker& ticker) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (auto it = m_merged_cache.find(ticker); it != m_merged_cache.end()) {
        return it->second;
    }

    std::vector<IMarketDataListener*> targets = m_listeners;
    if (!ticker.empty()) {
        if (auto it = m_ticker_listeners.find(ticker); it != m_ticker_listeners.end()) {
            for (auto* l : it->second) {
                if (std::find(targets.begin(), targets.end(), l) == targets.end()) {
                    targets.push_back(l);
                }
            }
        }
    }
    
    m_merged_cache[ticker] = targets;
    return targets;
}

void MarketDataFeed::handle_message(const nlohmann::json& j) {
    auto type_it = j.find("Type");
    if (type_it == j.end()) return;

    // Avoid std::string copy — borrow reference directly from the parsed JSON
    const std::string& type = type_it->get_ref<const std::string&>();

    static const nlohmann::json kEmptyData = nlohmann::json::object();
    auto data_it = j.find("Data");
    const nlohmann::json& data = (data_it != j.end()) ? *data_it : kEmptyData;

    try {
        if (type == "trade_update") {
            auto trades = MetaScalpCodec::parse_trade_update(data);
            Ticker ticker = data.value("Ticker", "");
            auto targets = get_target_listeners(ticker);
            for (auto* l : targets) l->on_trades(ticker, trades);
        } else if (type == "orderbook_snapshot") {
            Ticker ticker = data.value("Ticker", "");
            auto snap = MetaScalpCodec::parse_orderbook_snapshot(data, ticker);
            auto targets = get_target_listeners(ticker);
            for (auto* l : targets) l->on_orderbook_snapshot(snap);
        } else if (type == "orderbook_update") {
            Ticker ticker = data.value("Ticker", "");
            auto upd = MetaScalpCodec::parse_orderbook_update(data, ticker);
            auto targets = get_target_listeners(ticker);
            for (auto* l : targets) l->on_orderbook_update(upd);
        } else if (type == "order_update") {
            auto upd = MetaScalpCodec::parse_order_update(data);
            auto targets = get_target_listeners(upd.ticker);
            for (auto* l : targets) l->on_order_update(upd);
        } else if (type == "position_update") {
            auto upd = MetaScalpCodec::parse_position_update(data);
            auto targets = get_target_listeners(upd.ticker);
            for (auto* l : targets) l->on_position_update(upd);
        } else if (type == "balance_update") {
            auto upd = MetaScalpCodec::parse_balance_update(data);
            auto targets = get_target_listeners("");
            for (auto* l : targets) l->on_balance_update(upd);
        } else if (type == "finres_update") {
            auto upd = MetaScalpCodec::parse_finres_update(data);
            auto targets = get_target_listeners("");
            for (auto* l : targets) l->on_finres_update(upd);
        } else if (type == "error") {
            std::string msg = data.value("Message", "Unknown error");
            LOG_ERROR("WS API error: {}", msg);
            auto targets = get_target_listeners("");
            for (auto* l : targets) l->on_error(msg);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Error handling WS message {}: {}", type, e.what());
    }
}

void MarketDataFeed::set_record_tap(RawTap tap) {
    m_record_tap = std::move(tap);
}

} // namespace trade_bot
