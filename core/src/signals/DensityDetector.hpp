#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "numeric/Dema.hpp"
#include "universe/TickerUniverse.hpp"

#include <absl/container/btree_map.h>
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

        // DensityStack sub-detector
        int stack_min_levels{2};
        double stack_max_width_bps{20.0};
        double stack_min_size_usd{100000.0};
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
    void check_density_stacks_(std::chrono::system_clock::time_point now,
                               double price_inc, double mid);

    Ticker          ticker_;
    SignalBus&      bus_;
    const OrderBook& book_;
    const TickerUniverse& universe_;
    Config          cfg_;

    Dema<double>    avg_level_size_;
    
    // Tracking levels by price tick
    absl::btree_map<PriceTick, LevelMeta> tracked_;

    // DensityStack duplicate suppression: remember the last emitted stack
    // so we do not re-fire for the same cluster of unchanged levels.
    struct StackSignature {
        Side   side{Side::Buy};
        int    first_tick{0};
        int    last_tick{0};
        double total_size_usd{0.0};
        bool   valid{false};

        bool matches(Side s, int ft, int lt, double tsu) const {
            return valid && side == s && first_tick == ft &&
                   last_tick == lt &&
                   std::abs(total_size_usd - tsu) < 1.0;
        }
    };
    StackSignature last_stack_;
};

} // namespace trade_bot
