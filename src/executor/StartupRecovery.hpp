#pragma once

#include "domain/Types.hpp"
#include "transport/IOrderGateway.hpp"
#include "risk/AccountStatePersister.hpp"
#include "trading/ActiveTrade.hpp"

#include <vector>
#include <string>
#include <memory>

namespace trade_bot {

/**
 * T4-RECOVERY: Startup recovery logic.
 * Reconciles persisted state with server state at startup.
 */
class StartupRecovery {
public:
    struct Config {
        double max_recovery_stop_bps{30.0};
        bool   auto_ack_on_clean{true};
        std::string orphan_cancel_policy{"cancel"};
        double position_drift_coin{1e-8};
    };

    struct Result {
        bool auto_ack{false};
        std::vector<ActiveTrade> recovered_trades;
        std::vector<std::string> log_entries;
    };

    StartupRecovery(int connection_id, IOrderGateway& gateway, AccountStatePersister& persister, Config cfg);

    Result run();

private:
    int connection_id_;
    IOrderGateway& gateway_;
    AccountStatePersister& persister_;
    Config cfg_;
};

} // namespace trade_bot
