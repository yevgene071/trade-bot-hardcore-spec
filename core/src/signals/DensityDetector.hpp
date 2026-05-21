#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "numeric/Dema.hpp"
#include "universe/TickerUniverse.hpp"

#include <absl/container/flat_hash_map.h>
#include <chrono>
#include <vector>

namespace trade_bot {

/**
 * T2-DENSITY: Detects large limit orders (densities) and their consumption.
 */
class DensityDetector : public IDetector {
public:
    struct Config {
        double min_size_vs_avg{10.0};
        double min_size_usd{50000.0};
        double min_distance_bps{5.0};
        double max_distance_bps{150.0};
        std::chrono::milliseconds sticky_duration{2000};
        std::chrono::milliseconds fake_threshold{300};
        
        // Eating sub-detector
        std::chrono::milliseconds eating_window{3000};
        double eating_ratio_threshold{0.5};
        int eating_min_prints{5};
        
        int dema_period{100}; // avg level size period
    };

    DensityDetector(Ticker ticker,
                   SignalBus& bus,
                   const OrderBook& book,
                   const TickerUniverse& universe,
                   Config cfg);

    DensityDetector(Ticker ticker,
                   SignalBus& bus,
                   const OrderBook& book,
                   const TickerUniverse& universe);

    void on_frame(const FeatureFrame& frame) override;
    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBookUpdate& update) override;
    
    const char* perf_stage_name() const noexcept override { return "density_eval_us"; }

private:
    struct LevelMeta {
        Side      side;
        double    initial_size;
        std::chrono::system_clock::time_point first_seen;
        bool      emitted{false};
        
        // Eating tracking
        double    eaten_volume{0.0};
        int       print_count{0};
        std::chrono::system_clock::time_point last_hit;
        bool      eating_emitted{false}; // prevents re-firing DensityEating every trade
    };

    void check_sticky_levels_(std::chrono::system_clock::time_point now);

    Ticker          ticker_;
    SignalBus&      bus_;
    const OrderBook& book_;
    const TickerUniverse& universe_;
    Config          cfg_;

    Dema<double>    avg_level_size_;
    
    // Tracking levels by price tick
    absl::flat_hash_map<PriceTick, LevelMeta> tracked_;
};

} // namespace trade_bot
