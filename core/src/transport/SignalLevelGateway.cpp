#include "SignalLevelGateway.hpp"
#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"

#include <nlohmann/json.hpp>
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
    ss << base_url_ << "/api/connections/" << connection_id_ << "/signal-levels?Ticker=" << ticker;
    try {
        auto resp = http_.get(ss.str());
        if (resp.status != 200) return {};
        auto j = nlohmann::json::parse(resp.body);
        std::vector<SignalLevel> out;
        if (j.is_array()) {
            out.resize(j.size());
            std::transform(j.begin(), j.end(), out.begin(), [](const auto& item) {
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
    nlohmann::json body = {{"Ticker", ticker}, {"Price", price}};
    try {
        auto resp = http_.post(ss.str(), body.dump());
        if (resp.status != 200) return -1;
        auto j = nlohmann::json::parse(resp.body);
        return j.value("Id", -1LL);
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
    ss << base_url_ << "/api/connections/" << connection_id_ << "/signal-levels?Ticker=" << ticker;
    http_.del(ss.str());
}

void SignalLevelGateway::cleanup_triggered() {
    std::stringstream ss;
    ss << base_url_ << "/api/signal-levels/triggered";
    http_.del(ss.str());
}

} // namespace trade_bot
