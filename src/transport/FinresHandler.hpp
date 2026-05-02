#pragma once

#include "domain/Types.hpp"
#include <map>
#include <mutex>

namespace trade_bot {

class FinresHandler {
public:
    struct State {
        double current_result{0.0};
        double day_start_result{0.0};
        bool   initialized{false};
    };

    void handle_update(const FinresUpdate& update);
    void reset_day_start();

    double get_realized_pnl(int connection_id) const;
    bool   is_ready(int connection_id) const;

private:
    mutable std::mutex mutex_;
    std::map<int, State> states_;
};

} // namespace trade_bot
