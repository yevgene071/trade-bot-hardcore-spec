#include "OrderGateway.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "utils/UrlEncoder.hpp"
#include "utils/TickerSymbol.hpp"
#include <nlohmann/json.hpp>
#include <cctype>

namespace trade_bot {

namespace {
    // Case-insensitive array lookup: checks exact key, then with flipped first-letter case.
    // This handles MetaScalp API returning both "Tickers" and "tickers" variants.
    const nlohmann::json* find_array(const nlohmann::json& j, const std::string& key) {
        if (j.is_array()) return &j;
        if (j.contains(key) && j[key].is_array()) return &j[key];
        std::string alt = key;
        if (!alt.empty()) {
            if (std::isupper(static_cast<unsigned char>(alt[0]))) alt[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(alt[0])));
            else alt[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(alt[0])));
            if (j.contains(alt) && j[alt].is_array()) return &j[alt];
        }
        return nullptr;
    }
}



OrderGateway::OrderGateway(std::shared_ptr<IHttpClient> http_client)
    : m_http_client(std::move(http_client)) {}

std::vector<ConnectionInfo> OrderGateway::get_connections() {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections";
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get connections: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    const nlohmann::json* arr = find_array(j, "Connections");

    std::vector<ConnectionInfo> result;
    if (arr) {
        result.resize(arr->size());
        std::transform(arr->begin(), arr->end(), result.begin(), [](const auto& item) {
            return MetaScalpCodec::parse_connection_info(item);
        });
    }
    return result;
}

ConnectionInfo OrderGateway::get_connection(int id) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + std::to_string(id);
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get connection " + std::to_string(id) + ": status " + std::to_string(response.status));
    }
    return MetaScalpCodec::parse_connection_info(nlohmann::json::parse(response.body));
}

std::vector<TickerInfo> OrderGateway::get_tickers(int connection_id, bool refresh) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + 
                      std::to_string(connection_id) + "/tickers?Refresh=" + (refresh ? "true" : "false");
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get tickers: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    const nlohmann::json* arr = find_array(j, "Tickers");

    std::vector<TickerInfo> result;
    if (arr) {
        result.resize(arr->size());
        std::transform(arr->begin(), arr->end(), result.begin(), [](const auto& item) {
            return MetaScalpCodec::parse_ticker_info(item);
        });
    }
    return result;
}

OrderBookSnapshot OrderGateway::get_orderbook_snapshot(int connection_id,
                                                       const Ticker& ticker,
                                                       int zoom_index,
                                                       std::optional<int> depth_levels,
                                                       std::optional<double> depth_percent) {
    const std::string wire_ticker = to_metascalp_symbol(ticker);
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) +
                      "/api/connections/" + std::to_string(connection_id) +
                      "/orderbook-snapshot?Ticker=" + url_encode(wire_ticker);
    if (zoom_index > 0) {
        url += "&ZoomIndex=" + std::to_string(zoom_index);
    }
    if (depth_levels) {
        url += "&DepthLevels=" + std::to_string(*depth_levels);
    }
    if (depth_percent) {
        url += "&DepthPercent=" + std::to_string(*depth_percent);
    }

    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get orderbook snapshot: status " + std::to_string(response.status));
    }

    auto j = nlohmann::json::parse(response.body);
    Ticker response_ticker = MetaScalpCodec::normalize_ticker(
        MetaScalpCodec::get_val<std::string>(j, api::fields::kTicker, ticker));
    auto snap = MetaScalpCodec::parse_orderbook_snapshot(j, response_ticker.empty() ? ticker : response_ticker);
    if (!snap) throw CodecError(snap.error());
    return *snap;
}

std::vector<RestOrder> OrderGateway::get_open_orders(int connection_id, const Ticker& ticker) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders?Ticker=" + url_encode(to_metascalp_symbol(ticker));
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get orders: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    const nlohmann::json* arr = find_array(j, "Orders");

    std::vector<RestOrder> result;
    if (arr) {
        result.resize(arr->size());
        std::transform(arr->begin(), arr->end(), result.begin(), [](const auto& item) {
            return MetaScalpCodec::parse_rest_order(item);
        });
    }
    return result;
}

std::vector<PositionUpdate> OrderGateway::get_positions(int connection_id) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + 
                      std::to_string(connection_id) + "/positions";
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get positions: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    const nlohmann::json* arr = find_array(j, "Positions");

    std::vector<PositionUpdate> result;
    if (arr) {
        result.resize(arr->size());
        std::transform(arr->begin(), arr->end(), result.begin(), [](const auto& item) {
            auto pos = MetaScalpCodec::parse_position_update(item);
            if (!pos) throw CodecError(pos.error());
            return *pos;
        });
    }
    return result;
}

BalanceUpdate OrderGateway::get_balance(int connection_id) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + 
                      std::to_string(connection_id) + "/balance";
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get balance: status " + std::to_string(response.status));
    }
    auto bal = MetaScalpCodec::parse_balance_update(nlohmann::json::parse(response.body));
    if (!bal) throw CodecError(bal.error());
    return *bal;
}

PlaceOrderResult OrderGateway::place_order(int connection_id, const PlaceOrderRequest& request) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders";
    
    nlohmann::json body;
    body["Ticker"] = to_metascalp_symbol(request.ticker);
    body["Side"] = (request.side == Side::Buy ? 1 : 2);
    body["Price"] = request.price;
    body["Size"] = request.size;
    body["Type"] = static_cast<int>(request.type);
    body["ReduceOnly"] = request.reduce_only;

    auto response = m_http_client->post(url, body.dump());
    if (response.status != 200) {
        throw CodecError("Failed to place order: status " + std::to_string(response.status) + " " + response.body);
    }
    return MetaScalpCodec::parse_place_order_result(nlohmann::json::parse(response.body));
}

void OrderGateway::cancel_order(int connection_id, int64_t order_id, const Ticker& ticker) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders/cancel";
    nlohmann::json body;
    body["Ticker"] = to_metascalp_symbol(ticker);
    body["OrderId"] = order_id;
    body["Type"] = 0;  // 0 = cancel regardless of order type
    
    auto response = m_http_client->post(url, body.dump());
    if (response.status != 200) {
        throw CodecError("Failed to cancel order: status " + std::to_string(response.status));
    }
}

void OrderGateway::cancel_all_orders(int connection_id, const Ticker& ticker) {
    std::string url = get_base_url() + ":" + std::to_string(m_port.load(std::memory_order_relaxed)) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders/cancel-all";
    nlohmann::json body;
    body["Ticker"] = to_metascalp_symbol(ticker);
    
    auto response = m_http_client->post(url, body.dump());
    if (response.status != 200) {
        throw CodecError("Failed to cancel all orders: status " + std::to_string(response.status));
    }
}

} // namespace trade_bot
