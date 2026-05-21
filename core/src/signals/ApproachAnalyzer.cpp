#include "ApproachAnalyzer.hpp"
#include <numeric>

namespace trade_bot {

namespace {
ApproachHmm::Params get_default_params() {
    ApproachHmm::Params p;
    p.start_probs = {0.33, 0.33, 0.34};
    p.trans_matrix = {{
        {0.95, 0.025, 0.03}, 
        {0.025, 0.95, 0.025},
        {0.03, 0.025, 0.95}
    }};
    
    // Units: speed (bps/sec), pullbacks (count/window), dist (bps)
    // 0: Impulse. 
    p.emissions[0] = { .means = {3.0, 0.1, 100.0}, .stds = {2.0, 0.5, 50.0} };
    // 1: Slow. 
    p.emissions[1] = { .means = {0.5, 3.0, 50.0},  .stds = {0.3, 1.0, 20.0} };
    // 2: Consolidation. 
    p.emissions[2] = { .means = {0.0, 5.0, 5.0},   .stds = {0.1, 2.0, 5.0} };
    
    return p;
}
}

ApproachAnalyzer::ApproachAnalyzer(Ticker ticker,
                                 SignalBus& bus,
                                 const OrderBook& book,
                                 const LevelDetector& level_detector,
                                 Config cfg)
    : ticker_(std::move(ticker))
    , bus_(bus)
    , book_(book)
    , level_detector_(level_detector)
    , cfg_(cfg)
    , hmm_(cfg.hmm_use_default_params ? get_default_params() : cfg.hmm_params) {} // AB3

ApproachAnalyzer::ApproachAnalyzer(Ticker ticker,
                                 SignalBus& bus,
                                 const OrderBook& book,
                                 const LevelDetector& level_detector)
    : ApproachAnalyzer(std::move(ticker), bus, book, level_detector, Config{}) {}

void ApproachAnalyzer::on_frame(const FeatureFrame& frame) {
    if (frame.ticker != ticker_) return;
    if (!frame.valid) return;
    
    history_.push_back({frame.timestamp, frame.mid});
}

void ApproachAnalyzer::on_trade(const Trade& /*trade*/) {}
void ApproachAnalyzer::on_book_update(const OrderBookUpdate& /*update*/) {}

ApproachAnalyzer::Analysis ApproachAnalyzer::analyze(double level_price,
                                                   std::chrono::system_clock::time_point now) const {
    (void)level_price;
    const auto cutoff = now - cfg_.window;

    // Collect only entries within the configured approach window
    std::vector<double> prices;
    prices.reserve(history_.size());
    std::chrono::system_clock::time_point first_ts{};
    for (size_t i = 0; i < history_.size(); ++i) {
        const auto& [ts, price] = history_[i];
        if (ts < cutoff) continue;
        if (first_ts == std::chrono::system_clock::time_point{}) first_ts = ts;
        prices.push_back(price);
    }

    if (prices.size() < 10) return {ApproachType::Unknown, 0, 0, 0, 0, 0};

    auto peaks = ZigZag::calculate(prices, cfg_.pullback_min_bps);
    // For a downward approach (from above), pullbacks are temporary rallies → High pivots.
    // For an upward approach (from below), pullbacks are temporary dips → Low pivots.
    bool approaching_from_above = prices.front() > prices.back();
    int pullbacks = static_cast<int>(std::count_if(peaks.begin(), peaks.end(),
        [approaching_from_above](const auto& p) {
            return approaching_from_above ? p.is_high : !p.is_high;
        }));
    double duration = std::chrono::duration<double>(now - first_ts).count();
    double dist_bps = std::abs(prices.back() - prices.front()) / prices.front() * kBpsBase;
    double speed = dist_bps / std::max(0.1, duration);

    // AB1: single observation — repeating the same sample inflates posterior confidence
    std::vector<std::array<double, 3>> obs(1, {speed, static_cast<double>(pullbacks), dist_bps});
    auto probs = hmm_.predict(obs);

    Analysis a;
    a.impulse_prob = probs[0];
    a.slow_prob = probs[1];
    a.consolidation_prob = probs[2];
    a.pullbacks = pullbacks;
    a.speed_bps_sec = speed;

    if (a.impulse_prob > a.slow_prob && a.impulse_prob > a.consolidation_prob) a.type = ApproachType::Impulse;
    else if (a.slow_prob > a.impulse_prob && a.slow_prob > a.consolidation_prob) a.type = ApproachType::Slow;
    else a.type = ApproachType::Consolidation;

    return a;
}

} // namespace trade_bot
