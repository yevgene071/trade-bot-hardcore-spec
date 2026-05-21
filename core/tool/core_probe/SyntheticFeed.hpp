#pragma once

#include "domain/Types.hpp"
#include "transport/MarketDataFeed.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace trade_bot::probe {

/// Deterministic synthetic market-data feed for `core_probe synth`.
///
/// Generates `OrderBookSnapshot` / `OrderBookUpdate` / `Trade` events from
/// one of three hardcoded scenarios, with a stable RNG seed and stable
/// timestamps.  Same scenario name → byte-identical event stream → same PnL.
///
/// Each scenario operates on a single ticker.  Between every dispatched
/// event, the per-event callback `on_tick` is invoked so the runner can
/// drive feature extraction / strategy ticks at a known rate.
class SyntheticFeed {
public:
    using Listener = trade_bot::IMarketDataListener;
    using TickCallback = std::function<void(const Ticker& ticker,
                                            std::chrono::system_clock::time_point ts)>;

    explicit SyntheticFeed(const std::string& scenario_name,
                           const Ticker& ticker);

    /// Returns the list of supported scenarios.
    static std::vector<std::string> available_scenarios();

    /// Returns true if `name` is a known scenario.
    static bool is_known_scenario(const std::string& name);

    void add_listener(Listener* l) { listeners_.push_back(l); }
    void set_tick_callback(TickCallback cb) { tick_cb_ = std::move(cb); }

    struct RunStats {
        uint64_t messages_dispatched{0};
        uint64_t snapshots{0};
        uint64_t updates{0};
        uint64_t trades{0};
    };

    /// Run the scenario through to completion.  Returns dispatch counts.
    RunStats run();

private:
    void dispatch_snapshot(const OrderBookSnapshot& s);
    void dispatch_update(const OrderBookUpdate& u);
    void dispatch_trade(const Trade& t);
    void notify_tick(std::chrono::system_clock::time_point ts);

    // Scenario implementations
    void run_density_appears(RunStats& stats);
    void run_density_eaten_then_breakout(RunStats& stats);
    void run_leader_moves_alt_lags(RunStats& stats);

    std::string scenario_;
    Ticker      ticker_;
    std::vector<Listener*> listeners_;
    TickCallback tick_cb_;
};

} // namespace trade_bot::probe
