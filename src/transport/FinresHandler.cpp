#include "FinresHandler.hpp"

namespace trade_bot {

void FinresHandler::handle_update(const FinresUpdate& update) {
    std::lock_guard lock(mutex_);
    auto& state = states_[update.connection_id];
    
    double total_result = 0.0;
    for (const auto& entry : update.finreses) {
        // In MetaScalp, there's usually one primary currency result we care about (e.g. USDT)
        // For simplicity, we sum all results if multiple currencies exist, 
        // but typically it's just one entry.
        total_result += entry.result;
    }

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

} // namespace trade_bot
