#include "OrderbookSettingsLoader.hpp"
#include "transport/MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "utils/UrlEncoder.hpp"
#include "utils/TickerSymbol.hpp"

#include <nlohmann/json.hpp>
#include <sstream>

namespace trade_bot {

OrderbookSettingsLoader::OrderbookSettingsLoader(IHttpClient& http,
                                               std::string base_url,
                                               int connection_id)
    : http_(http)
    , base_url_(std::move(base_url))
    , connection_id_(connection_id) {}

std::optional<OrderbookSettings> OrderbookSettingsLoader::fetch(const Ticker& ticker) {
    std::stringstream ss;
    ss << base_url_ << "/api/connections/" << connection_id_ << "/orderbook-settings"
       << "?Ticker=" << url_encode(to_metascalp_symbol(ticker));

    try {
        auto resp = http_.get(ss.str());
        if (resp.status != 200) {
            if (resp.status == 400 || resp.status == 404) {
                LOG_WARN("OrderbookSettingsLoader: no settings for {} (HTTP {}), using defaults",
                         ticker, resp.status);
            } else {
                LOG_ERROR("OrderbookSettingsLoader: failed to fetch for {}: HTTP {}",
                          ticker, resp.status);
            }
            return std::nullopt;
        }

        auto j = nlohmann::json::parse(resp.body);
        return MetaScalpCodec::parse_orderbook_settings(j);
    } catch (const std::exception& ex) {
        LOG_ERROR("OrderbookSettingsLoader: error fetching {}: {}", ticker, ex.what());
        return std::nullopt;
    }
}

} // namespace trade_bot
