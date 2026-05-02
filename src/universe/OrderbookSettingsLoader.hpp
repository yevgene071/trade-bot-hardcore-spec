#pragma once

#include "transport/IHttpClient.hpp"
#include "domain/Types.hpp"

#include <optional>
#include <string>

namespace trade_bot {

/**
 * REST client for fetching orderbook settings from MetaScalp.
 */
class OrderbookSettingsLoader {
public:
    OrderbookSettingsLoader(IHttpClient& http, std::string base_url, int connection_id);

    std::optional<OrderbookSettings> fetch(const Ticker& ticker);

private:
    IHttpClient& http_;
    std::string  base_url_;
    int          connection_id_;
};

} // namespace trade_bot
