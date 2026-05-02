#include "Config.hpp"
#include "logger/Logger.hpp"
#include <filesystem>

namespace trade_bot {

toml::table Config::s_data;

void Config::load(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        throw ConfigError("Config file not found: " + path);
    }

    try {
        s_data = toml::parse_file(path);
        LOG_INFO("Config loaded from {}", path);
        validate();
    } catch (const toml::parse_error& err) {
        throw ConfigError("Failed to parse config: " + std::string(err.description()));
    }
}

void Config::validate() {
    // App
    get<std::string>("app.version");
    get<std::string>("app.name");

    // Logger
    get<std::string>("logger.level");
    get<std::string>("logger.path");

    // Network
    get<int64_t>("network.http_timeout_ms");

    // Trading
    get<std::vector<std::string>>("trading.symbols");

    // Risk Management (Core)
    get<double>("risk.max_daily_loss_pct");
    get<double>("risk.max_per_trade_risk_pct");
    get<int64_t>("risk.max_concurrent_positions");
    get<double>("risk.max_leverage");
    
    // Clock Monitoring (T0-CLOCK)
    if (has("clock.sources")) {
        get<std::vector<std::string>>("clock.sources");
    }
    get<int64_t>("clock.check_interval_sec");
    get<int64_t>("clock.warn_drift_ms");
    get<int64_t>("clock.max_clock_drift_ms");

    LOG_INFO("Config validation successful");
}

toml::node_view<toml::node> Config::find_node(std::string_view dotted_path) {
    return s_data.at_path(dotted_path);
}

} // namespace trade_bot
