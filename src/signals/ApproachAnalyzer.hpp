#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "signals/LevelDetector.hpp"
#include "numeric/Hmm.hpp"
#include "numeric/ZigZag.hpp"

#include <deque>
#include <vector>

namespace trade_bot {

/**
 * T2-APPROACH: Classifies how price approaches a level (Impulse, Slow, Consolidation).
 */
class ApproachAnalyzer : public IDetector {
public:
    struct Config {
        std::chrono::seconds window{30};
        double pullback_min_bps{5.0};
        
        // HMM params (learned or default)
        ApproachHmm::Params hmm_params;
    };

    ApproachAnalyzer(Ticker ticker,
                    SignalBus& bus,
                    const OrderBook& book,
                    const LevelDetector& level_detector,
                    Config cfg);

    ApproachAnalyzer(Ticker ticker,
                    SignalBus& bus,
                    const OrderBook& book,
                    const LevelDetector& level_detector);

    void on_frame(const FeatureFrame& frame) override;
    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBookUpdate& update) override;

    enum class ApproachType { Impulse, Slow, Consolidation, Unknown };
    
    struct Analysis {
        ApproachType type;
        double       impulse_prob;
        double       slow_prob;
        double       consolidation_prob;
        int          pullbacks;
        double       speed_bps_sec;
    };

    Analysis analyze(double level_price, std::chrono::system_clock::time_point now) const;

private:
    Ticker          ticker_;
    SignalBus&      bus_;
    const OrderBook& book_;
    const LevelDetector& level_detector_;
    Config          cfg_;

    std::deque<std::pair<std::chrono::system_clock::time_point, double>> history_;
    
    ApproachHmm     hmm_;
};

} // namespace trade_bot
