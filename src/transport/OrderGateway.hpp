#pragma once

#include "domain/Types.hpp"
#include "IHttpClient.hpp"
#include <memory>
#include <vector>

namespace trade_bot {

class OrderGateway {
public:
    explicit OrderGateway(std::shared_ptr<IHttpClient> http_client);

    std::vector<ConnectionInfo> get_connections();
    ConnectionInfo get_connection(int id);
    
    std::vector<TickerInfo> get_tickers(int connection_id, bool refresh = false);
    std::vector<RestOrder> get_open_orders(int connection_id, const Ticker& ticker);
    std::vector<PositionUpdate> get_positions(int connection_id);
    BalanceUpdate get_balance(int connection_id);
    
    PlaceOrderResult place_order(int connection_id, const PlaceOrderRequest& request);
    void cancel_order(int connection_id, int64_t order_id);
    void cancel_all_orders(int connection_id, const Ticker& ticker);

    void set_port(int port) { m_port = port; }

private:
    std::string get_base_url() const { return "http://127.0.0.1"; } // Port will be appended or found via discovery
    std::shared_ptr<IHttpClient> m_http_client;
    int m_port = 17845; // Default, should be set via discovery
};

} // namespace trade_bot
