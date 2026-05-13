#pragma once

#include "IOrderGateway.hpp"
#include "IHttpClient.hpp"
#include <memory>
#include <vector>

namespace trade_bot {

class OrderGateway : public IOrderGateway {
public:
    explicit OrderGateway(std::shared_ptr<IHttpClient> http_client);

    std::vector<ConnectionInfo> get_connections() override;
    ConnectionInfo get_connection(int id) override;
    
    std::vector<TickerInfo> get_tickers(int connection_id, bool refresh = false) override;
    std::vector<RestOrder> get_open_orders(int connection_id, const Ticker& ticker) override;
    std::vector<PositionUpdate> get_positions(int connection_id) override;
    BalanceUpdate get_balance(int connection_id) override;
    
    PlaceOrderResult place_order(int connection_id, const PlaceOrderRequest& request) override;
    void cancel_order(int connection_id, int64_t order_id, const Ticker& ticker) override;
    void cancel_all_orders(int connection_id, const Ticker& ticker) override;

    void set_port(int port) { m_port = port; }

private:
    static std::string get_base_url() { return "http://127.0.0.1"; } // Port will be appended or found via discovery
    std::shared_ptr<IHttpClient> m_http_client;
    int m_port = 17845; // Default, should be set via discovery
};

} // namespace trade_bot
