#include "Config.hpp"
#include "logger/Logger.hpp"
#include <filesystem>

#include <memory>

namespace trade_bot {

std::unique_ptr<Config> Config::s_instance;

Config& Config::instance() {
    if (!s_instance) {
        s_instance = std::make_unique<Config>();
    }
    return *s_instance;
}

void Config::load_file(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        throw ConfigError("Config file not found: " + path);
    }

    try {
        data_ = toml::parse_file(path);
        validate();
    } catch (const toml::parse_error& err) {
        throw ConfigError("Failed to parse config: " + std::string(err.description()));
    }
}

void Config::load(const std::string& path) {
    instance().load_file(path);
}

void Config::validate() {
    // App
    get_val<std::string>("app.version");
    get_val<std::string>("app.name");

    // Logger
    get_val<std::string>("logger.level");
    get_val<std::string>("logger.path");

    // Network
    auto http_timeout = get_val<int64_t>("network.http_timeout_ms");
    if (http_timeout < 100 || http_timeout > 60000) {
        throw ConfigError("network.http_timeout_ms out of range [100, 60000]");
    }

    // Risk Management (Core)
    auto daily_loss = get_val<double>("risk.max_daily_loss_pct");
    if (daily_loss < 0.0 || daily_loss > 100.0) {
        throw ConfigError("risk.max_daily_loss_pct out of range [0, 100]");
    }
    
    auto per_trade_risk = get_val<double>("risk.max_per_trade_risk_pct");
    if (per_trade_risk < 0.0 || per_trade_risk > 10.0) {
        throw ConfigError("risk.max_per_trade_risk_pct out of range [0, 10]");
    }

    // Clock Monitoring
    if (has_val("clock.sources")) {
        get_val<std::vector<std::string>>("clock.sources");
    }
}

toml::node_view<const toml::node> Config::find_node_internal(std::string_view dotted_path) const {
    return data_.at_path(dotted_path);
}

} // namespace trade_bot
