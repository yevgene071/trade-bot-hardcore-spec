#include "FinresHandler.hpp"
#include "perf/TraceContext.hpp" // Берем контекст пакета, если часы не привязаны
#include <numeric>
#include <algorithm>
#include <chrono>

namespace trade_bot {

FinresHandler::FinresHandler(std::shared_ptr<IClock> clock) : clock_(std::move(clock)) {}

void FinresHandler::handle_update(const FinresUpdate& update) {
    std::lock_guard lock(mutex_);
    auto& state = states_[update.connection_id];
    
    double total_result = std::accumulate(update.finreses.begin(), update.finreses.end(), 0.0, 
        [](double sum, const auto& entry) {
            return sum + entry.result;
        });

    // ФИКС P0-DETERMINISM: Приоритет часам симулятора, затем trace-контексту, и только потом системному времени
    int64_t ts = 0;
    if (clock_) {
        ts = std::chrono::duration_cast<std::chrono::seconds>(clock_->now().time_since_epoch()).count();
    } else if (current_trace_context().recv_ns != 0) {
        ts = current_trace_context().recv_ns / 1'000'000'000; // конвертируем наносекунды в секунды
    } else {
        ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    const int64_t current_day_start = (ts / 86400) * 86400;

    state.current_result = total_result;
    if (!state.initialized || current_day_start > state.last_day_start_ts) {
        state.day_start_result = total_result;
        state.last_day_start_ts = current_day_start;
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
        return Snapshot{
            .balance = it->second.current_result,
            .equity = it->second.current_result
        };
    }
    return std::nullopt;
}

} // namespace trade_bot
