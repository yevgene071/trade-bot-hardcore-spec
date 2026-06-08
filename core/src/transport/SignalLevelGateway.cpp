#include "SignalLevelGateway.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "utils/UrlEncoder.hpp"
#include "utils/TickerSymbol.hpp"

#include <nlohmann/json.hpp>
#include <cmath>
#include <sstream>

namespace trade_bot {

SignalLevelGateway::SignalLevelGateway(IHttpClient& http,
                                     std::string base_url,
                                     int connection_id)
    : http_(http)
    , base_url_(std::move(base_url))
    , connection_id_(connection_id) {}

std::vector<SignalLevel> SignalLevelGateway::get_all(const Ticker& ticker) {
    std::stringstream ss;
    ss << base_url_ << "/api/connections/" << connection_id_
       << "/signal-levels?Ticker=" << url_encode(to_metascalp_symbol(ticker));
    try {
        auto resp = http_.get(ss.str());
        if (resp.status != 200) return {};
        auto j = nlohmann::json::parse(resp.body);
        const nlohmann::json* arr = nullptr;
        if (j.is_array()) {
            arr = &j;
        } else if (j.contains(api::fields::kSignalLevels) && j[api::fields::kSignalLevels].is_array()) {
            arr = &j[api::fields::kSignalLevels];
        } else if (j.contains("signalLevels") && j["signalLevels"].is_array()) {
            arr = &j["signalLevels"];
        }
        std::vector<SignalLevel> out;
        if (arr) {
            out.resize(arr->size());
            std::transform(arr->begin(), arr->end(), out.begin(), [](const auto& item) {
                return MetaScalpCodec::parse_signal_level(item);
            });
        }
        return out;
    } catch (const std::exception& ex) {
        LOG_WARN("SignalLevelGateway::get_all({}) failed: {}", ticker, ex.what());
        return {};
    }
}

int64_t SignalLevelGateway::create(const Ticker& ticker, double price) {
    std::stringstream ss;
    ss << base_url_ << "/api/connections/" << connection_id_ << "/signal-levels";
    nlohmann::json body = {{"Ticker", to_metascalp_symbol(ticker)}, {"Price", price}};
    try {
        auto resp = http_.post(ss.str(), body.dump());
        if (resp.status != 200) return -1;
        auto j = nlohmann::json::parse(resp.body);
        auto id = j.value("Id", -1LL);
        if (id > 0) return id;

        // SDK v1.0.7 documents place response as {"Status":"ok"} without Id.
        // Recover the server id from the list endpoint so the bridge can later
        // remove/mark this level deterministically.
        constexpr double kPriceEps = 1e-9;
        for (const auto& level : get_all(ticker)) {
            if (!level.triggered && std::abs(level.price - price) <= kPriceEps) {
                return level.id;
            }
        }
        return -1;
    } catch (const std::exception& ex) {
        LOG_WARN("SignalLevelGateway::create({}, {}) failed: {}", ticker, price, ex.what());
        return -1;
    }
}

void SignalLevelGateway::remove(int64_t id) {
    std::stringstream ss;
    ss << base_url_ << "/api/connections/" << connection_id_ << "/signal-levels/" << id;
    http_.del(ss.str());
}

void SignalLevelGateway::remove_all(const Ticker& ticker) {
    std::stringstream ss;
    ss << base_url_ << "/api/connections/" << connection_id_
       << "/signal-levels?Ticker=" << url_encode(to_metascalp_symbol(ticker));
    http_.del(ss.str());
}

void SignalLevelGateway::cleanup_triggered() {
    std::stringstream ss;
    ss << base_url_ << "/api/signal-levels/triggered";
    http_.del(ss.str());
}

} // namespace trade_bot
