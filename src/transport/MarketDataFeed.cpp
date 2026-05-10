#include "MarketDataFeed.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <algorithm>
#include <chrono>

namespace trade_bot {

// MetaScalp uses "BTC_USDT" format. Config uses the same format, so internal
// and wire formats are identical. to_ms/from_ms are kept for safety (handles
// both "BTCUSDT" and "BTC_USDT" input gracefully).
static std::string to_ms(const std::string& t) {
    if (t.find('_') != std::string::npos) return t;  // already BTC_USDT
    for (const char* q : {"USDT", "USDC", "BTC", "ETH", "BNB", "BUSD"}) {
        std::size_t qlen = std::strlen(q);
        if (t.size() > qlen && t.substr(t.size() - qlen) == q)
            return t.substr(0, t.size() - qlen) + "_" + q;
    }
    return t;
}

static std::string from_ms(const std::string& t) {
    return t;  // internal format == MetaScalp format (both BTC_USDT)
}

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
        const std::string ms_ticker = to_ms(ticker);
        LOG_INFO("Subscribing to {}", ticker);
        auto send_sub = [&](const char* type) {
            m_ws_client->send(nlohmann::json{
                {"Type", type},
                {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ms_ticker}, {"ZoomIndex", 0}}}
            }.dump());
        };
        send_sub("trade_subscribe");
        send_sub("orderbook_subscribe");
        send_sub("funding_subscribe");
        send_sub("mark_price_subscribe");
    }
}

void MarketDataFeed::unsubscribe_ticker(const Ticker& ticker) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribed_tickers.erase(ticker);
    }

    if (m_ws_client->is_connected()) {
        const std::string ms_ticker = to_ms(ticker);
        auto send_unsub = [&](const char* type) {
            m_ws_client->send(nlohmann::json{
                {"Type", type},
                {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ms_ticker}}}
            }.dump());
        };
        send_unsub("trade_unsubscribe");
        send_unsub("orderbook_unsubscribe");
        send_unsub("funding_unsubscribe");
        send_unsub("mark_price_unsubscribe");
    }
}

std::optional<FundingData> MarketDataFeed::get_funding(const Ticker& ticker) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_funding_cache.find(ticker);
    if (it == m_funding_cache.end()) return std::nullopt;
    return it->second;
}

double MarketDataFeed::get_mark_price(const Ticker& ticker) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_mark_price_cache.find(ticker);
    return it != m_mark_price_cache.end() ? it->second : 0.0;
}

std::unordered_map<Ticker, double> MarketDataFeed::get_all_mark_prices() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mark_price_cache;
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
        const std::string ms_ticker = to_ms(ticker);
        auto send_sub = [&](const char* type) {
            m_ws_client->send(nlohmann::json{
                {"Type", type},
                {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ms_ticker}, {"ZoomIndex", 0}}}
            }.dump());
        };
        send_sub("trade_subscribe");
        send_sub("orderbook_subscribe");
        send_sub("funding_subscribe");
        send_sub("mark_price_subscribe");
    }
}

std::shared_ptr<const MarketDataFeed::ListenerList> MarketDataFeed::get_target_listeners(const Ticker& ticker) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_merged_cache.find(ticker);
    if (it != m_merged_cache.end()) {
        return it->second;
    }

    auto targets = std::make_shared<ListenerList>();
    // Specific listeners first (e.g. TickerController which updates the book)
    if (!ticker.empty()) {
        auto tit = m_ticker_listeners.find(ticker);
        if (tit != m_ticker_listeners.end()) {
            for (auto* l : tit->second) {
                targets->push_back(l);
            }
        }
    }
    
    // Then global listeners (e.g. Executor)
    for (auto* l : m_listeners) {
        if (std::find(targets->begin(), targets->end(), l) == targets->end()) {
            targets->push_back(l);
        }
    }
    
    auto shared_targets = std::shared_ptr<const ListenerList>(std::move(targets));
    m_merged_cache[ticker] = shared_targets;
    return shared_targets;
}

void MarketDataFeed::handle_message(const nlohmann::json& j) {
    LOG_TRACE("Raw WS: {}", j.dump());
    auto type_it = j.find("Type");
    if (type_it == j.end()) {
        LOG_DEBUG("WS message without Type field: {}", j.dump());
        return;
    }

    // Avoid std::string copy — borrow reference directly from the parsed JSON
    const std::string& type = type_it->get_ref<const std::string&>();

    LOG_DEBUG("WS message: {}", type);

    static const nlohmann::json kEmptyData = nlohmann::json::object();
    auto data_it = j.find("Data");
    const nlohmann::json& data = (data_it != j.end()) ? *data_it : kEmptyData;

    try {
        if (type == "trade_update") {
            auto trades = MetaScalpCodec::parse_trade_update(data);
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, ""));
            auto targets = get_target_listeners(ticker);
            for (auto* l : *targets) l->on_trades(ticker, trades);
        } else if (type == "orderbook_snapshot") {
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, ""));
            auto snap = MetaScalpCodec::parse_orderbook_snapshot(data, ticker);
            auto targets = get_target_listeners(ticker);
            for (auto* l : *targets) l->on_orderbook_snapshot(snap);
        } else if (type == "orderbook_update") {
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, ""));
            auto upd = MetaScalpCodec::parse_orderbook_update(data, ticker);
            auto targets = get_target_listeners(ticker);
            for (auto* l : *targets) l->on_orderbook_update(upd);
        } else if (type == "order_update") {
            auto upd = MetaScalpCodec::parse_order_update(data);
            upd.ticker = from_ms(upd.ticker);
            auto targets = get_target_listeners(upd.ticker);
            for (auto* l : *targets) l->on_order_update(upd);
        } else if (type == "position_update") {
            auto upd = MetaScalpCodec::parse_position_update(data);
            upd.ticker = from_ms(upd.ticker);
            auto targets = get_target_listeners(upd.ticker);
            for (auto* l : *targets) l->on_position_update(upd);
        } else if (type == "balance_update") {
            auto upd = MetaScalpCodec::parse_balance_update(data);
            auto targets = get_target_listeners("");
            for (auto* l : *targets) l->on_balance_update(upd);
        } else if (type == "finres_update") {
            auto upd = MetaScalpCodec::parse_finres_update(data);
            auto targets = get_target_listeners("");
            for (auto* l : *targets) l->on_finres_update(upd);
        } else if (type == "funding_update") {
            // PascalCase fields per MetaScalp API
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, "Ticker", ""));
            if (!ticker.empty()) {
                FundingData fd;
                fd.rate = MetaScalpCodec::get_val<double>(data, "FundingRate", 0.0);
                fd.next_funding_time = MetaScalpCodec::parse_iso8601(
                    MetaScalpCodec::get_val<std::string>(data, "FundingTime", ""));
                fd.updated_at = std::chrono::system_clock::now();
                std::lock_guard<std::mutex> lock(m_mutex);
                m_funding_cache[ticker] = fd;
                LOG_DEBUG("Funding update {}: rate={:.6f}", ticker, fd.rate);
            }
        } else if (type == "mark_price_update") {
            // lowercase fields per MetaScalp API (observed in testing)
            Ticker ticker = from_ms(data.value("ticker", ""));
            double mp = data.value("markPrice", 0.0);
            if (!ticker.empty() && mp > 0.0) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_mark_price_cache[ticker] = mp;
            }
        } else if (type == "error") {
            std::string msg = MetaScalpCodec::get_val<std::string>(data, "Message", "Unknown error");
            LOG_ERROR("WS API error: {}", msg);
            auto targets = get_target_listeners("");
            for (auto* l : *targets) l->on_error(msg);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Error handling WS message {}: {}", type, e.what());
    }
}

void MarketDataFeed::set_record_tap(RawTap tap) {
    m_record_tap = std::move(tap);
}

} // namespace trade_bot
