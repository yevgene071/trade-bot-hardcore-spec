#pragma once

#include "AccountState.hpp"
#include "trading/ActiveTrade.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace trade_bot {

/**
 * T4-RISK: Persists account state and active trades to disk.
 */
class AccountStatePersister {
public:
    struct PersistedData {
        AccountState account_state;
        std::vector<ActiveTrade> active_trades;
        std::string last_reset_day_utc;
        bool kill_switch_triggered;
        std::string kill_switch_reason;
    };

    explicit AccountStatePersister(std::string path = "journal/account_state.json");

    void save(const PersistedData& data);
    std::optional<PersistedData> load();

private:
    std::string path_;
};

// JSON serialization helpers
void to_json(nlohmann::json& j, const AccountState& s);
void from_json(const nlohmann::json& j, AccountState& s);
void to_json(nlohmann::json& j, const TradePlan& p);
void from_json(const nlohmann::json& j, TradePlan& p);
void to_json(nlohmann::json& j, const ActiveTrade& t);
void from_json(const nlohmann::json& j, ActiveTrade& t);
void to_json(nlohmann::json& j, const Signal& s);
void from_json(const nlohmann::json& j, Signal& s);

} // namespace trade_bot
