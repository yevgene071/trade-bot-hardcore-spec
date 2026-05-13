#pragma once

#include "domain/Types.hpp"
#include <vector>

namespace trade_bot {

/**
 * Interface for order management and account data.
 */
class IOrderGateway {
public:
    virtual ~IOrderGateway() = default;

    virtual std::vector<ConnectionInfo> get_connections() = 0;
    virtual ConnectionInfo get_connection(int id) = 0;
    
    virtual std::vector<TickerInfo> get_tickers(int connection_id, bool refresh = false) = 0;
    virtual std::vector<RestOrder> get_open_orders(int connection_id, const Ticker& ticker) = 0;
    virtual std::vector<PositionUpdate> get_positions(int connection_id) = 0;
    virtual BalanceUpdate get_balance(int connection_id) = 0;
    
    virtual PlaceOrderResult place_order(int connection_id, const PlaceOrderRequest& request) = 0;
    virtual void cancel_order(int connection_id, int64_t order_id, const Ticker& ticker) = 0;
    virtual void cancel_all_orders(int connection_id, const Ticker& ticker) = 0;
};

} // namespace trade_bot
