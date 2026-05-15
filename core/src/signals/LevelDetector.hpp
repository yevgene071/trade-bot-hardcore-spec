#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "numeric/Clustering.hpp"
#include "numeric/Kde.hpp"
#include "numeric/Dema.hpp"
#include "utils/CircularBuffer.hpp"
#include "utils/FixedString.hpp"

#include <absl/container/flat_hash_map.h>

#include <vector>

namespace trade_bot {

class ApproachAnalyzer;

/**
 * T2-LEVEL: Detects horizontal support/resistance levels.
 */
class LevelDetector : public IDetector {
public:
    struct Config {
        std::chrono::minutes lookback{60};
        double min_reversal_bps{15.0};
        double cluster_tolerance_bps{10.0};
        size_t touches_min{2};
        std::chrono::seconds level_min_age{300};
        
        double approach_trigger_bps{10.0};
        
        // KDE
        double kde_smoothness{1.0};
        double cluster_min_volume_pct{0.05};
    };

    struct Level {
        double price;
        int    touches;
        double kde_score;
        std::chrono::system_clock::time_point last_touch;
        std::chrono::system_clock::time_point created_at;
        FixedString<16> source; // "extreme", "cluster", "both"
        
        double confidence;
    };

    LevelDetector(Ticker ticker,
                 SignalBus& bus,
                 const OrderBook& book,
                 const ClusterSnapshotManager& cluster_mgr,
                 const Config& cfg);

    LevelDetector(Ticker ticker,
                 SignalBus& bus,
                 const OrderBook& book,
                 const ClusterSnapshotManager& cluster_mgr);

    void on_frame(const FeatureFrame& frame) override;
    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBookUpdate& update) override;

    void rebuild();

    void set_approach_analyzer(const ApproachAnalyzer* analyzer) { analyzer_ = analyzer; }

    [[nodiscard]] const std::vector<Level>& levels() const { return active_levels_; }

private:
    struct Extreme {
        double price;
        std::chrono::system_clock::time_point ts;
        bool is_high;
    };

    void update_extremes_(const FeatureFrame& frame);
    void rebuild_levels_(std::chrono::system_clock::time_point now);
    void check_approaches_(const FeatureFrame& frame);

    Ticker          ticker_;
    SignalBus&      bus_;
    const OrderBook& book_;
    const ClusterSnapshotManager& cluster_mgr_;
    Config          cfg_;

    const ApproachAnalyzer* analyzer_{nullptr};

    CircularBuffer<std::pair<std::chrono::system_clock::time_point, double>, 4096> mid_history_;
    CircularBuffer<Extreme, 1024> extremes_;
    std::chrono::system_clock::time_point last_rebuild_{};
    
    std::vector<Level> active_levels_;
    
    // To track current approach state
    struct ApproachState {
        double level_price;
        std::chrono::system_clock::time_point start_ts;
        bool above_level;  // was price above level when approach started
        double entry_mid;  // mid price at approach start
    };
    absl::flat_hash_map<PriceTick, ApproachState> current_approaches_;
};

} // namespace trade_bot
