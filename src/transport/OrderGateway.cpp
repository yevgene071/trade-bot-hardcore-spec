#include "OrderGateway.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <nlohmann/json.hpp>

namespace trade_bot {

OrderGateway::OrderGateway(std::shared_ptr<IHttpClient> http_client)
    : m_http_client(std::move(http_client)) {}

std::vector<ConnectionInfo> OrderGateway::get_connections() {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections";
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get connections: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    std::vector<ConnectionInfo> result;
    if (j.is_array()) {
        for (const auto& item : j) {
            result.push_back(MetaScalpCodec::parse_connection_info(item));
        }
    }
    return result;
}

ConnectionInfo OrderGateway::get_connection(int id) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + std::to_string(id);
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get connection " + std::to_string(id) + ": status " + std::to_string(response.status));
    }
    return MetaScalpCodec::parse_connection_info(nlohmann::json::parse(response.body));
}

std::vector<TickerInfo> OrderGateway::get_tickers(int connection_id, bool refresh) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + 
                      std::to_string(connection_id) + "/tickers?Refresh=" + (refresh ? "true" : "false");
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get tickers: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    std::vector<TickerInfo> result;
    if (j.is_array()) {
        for (const auto& item : j) {
            result.push_back(MetaScalpCodec::parse_ticker_info(item));
        }
    }
    return result;
}

std::vector<RestOrder> OrderGateway::get_open_orders(int connection_id, const Ticker& ticker) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders?Ticker=" + ticker;
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get orders: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    std::vector<RestOrder> result;
    if (j.is_array()) {
        for (const auto& item : j) {
            result.push_back(MetaScalpCodec::parse_rest_order(item));
        }
    }
    return result;
}

std::vector<PositionUpdate> OrderGateway::get_positions(int connection_id) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + 
                      std::to_string(connection_id) + "/positions";
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get positions: status " + std::to_string(response.status));
    }
    
    auto j = nlohmann::json::parse(response.body);
    std::vector<PositionUpdate> result;
    if (j.is_array()) {
        for (const auto& item : j) {
            result.push_back(MetaScalpCodec::parse_position_update(item));
        }
    }
    return result;
}

BalanceUpdate OrderGateway::get_balance(int connection_id) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + 
                      std::to_string(connection_id) + "/balance";
    auto response = m_http_client->get(url);
    if (response.status != 200) {
        throw CodecError("Failed to get balance: status " + std::to_string(response.status));
    }
    return MetaScalpCodec::parse_balance_update(nlohmann::json::parse(response.body));
}

PlaceOrderResult OrderGateway::place_order(int connection_id, const PlaceOrderRequest& request) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders";
    
    nlohmann::json body;
    body["Ticker"] = request.ticker;
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

void OrderGateway::cancel_order(int connection_id, int64_t order_id) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders/cancel";
    nlohmann::json body;
    body["OrderId"] = order_id;
    
    auto response = m_http_client->post(url, body.dump());
    if (response.status != 200) {
        throw CodecError("Failed to cancel order: status " + std::to_string(response.status));
    }
}

void OrderGateway::cancel_all_orders(int connection_id, const Ticker& ticker) {
    std::string url = get_base_url() + ":" + std::to_string(m_port) + "/api/connections/" + 
                      std::to_string(connection_id) + "/orders/cancel-all";
    nlohmann::json body;
    body["Ticker"] = ticker;
    
    auto response = m_http_client->post(url, body.dump());
    if (response.status != 200) {
        throw CodecError("Failed to cancel all orders: status " + std::to_string(response.status));
    }
}

} // namespace trade_bot
