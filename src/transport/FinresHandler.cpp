#include "FinresHandler.hpp"
#include <numeric>

namespace trade_bot {

void FinresHandler::handle_update(const FinresUpdate& update) {
    std::lock_guard lock(mutex_);
    auto& state = states_[update.connection_id];
    
    double total_result = std::accumulate(update.finreses.begin(), update.finreses.end(), 0.0, [](double sum, const auto& entry) {
        return sum + entry.result;
    });

    state.current_result = total_result;
    if (!state.initialized) {
        state.day_start_result = total_result;
        state.initialized = true;
    }
}

void FinresHandler::reset_day_start() {
    std::lock_guard lock(mutex_);
    for (auto& [id, state] : states_) {
        state.day_start_result = state.current_result;
    }
}

double FinresHandler::get_realized_pnl(int connection_id) const {
    std::lock_guard lock(mutex_);
    auto it = states_.find(connection_id);
    if (it != states_.end() && it->second.initialized) {
        return it->second.current_result - it->second.day_start_result;
    }
    return 0.0;
}

bool FinresHandler::is_ready(int connection_id) const {
    std::lock_guard lock(mutex_);
    auto it = states_.find(connection_id);
    return it != states_.end() && it->second.initialized;
}

std::optional<FinresHandler::Snapshot> FinresHandler::get_snapshot(int connection_id) const {
    std::lock_guard lock(mutex_);
    auto it = states_.find(connection_id);
    if (it != states_.end() && it->second.initialized) {
        // MetaScalp 'result' is total account value (equity). 
        // For simplicity, we use it for both equity and free balance until 
        // separate BalanceUpdate/PositionUpdate logic is fully integrated.
        return Snapshot{
            .balance = it->second.current_result,
            .equity = it->second.current_result
        };
    }
    return std::nullopt;
}

} // namespace trade_bot
