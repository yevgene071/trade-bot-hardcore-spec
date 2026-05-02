#include "LeaderSignal.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

LeaderSignal::LeaderSignal(Ticker ticker,
                          Ticker leader_ticker,
                          SignalBus& bus,
                          const LeaderTracker& tracker,
                          Config cfg)
    : ticker_(std::move(ticker))
    , leader_ticker_(std::move(leader_ticker))
    , bus_(bus)
    , tracker_(tracker)
    , cfg_(cfg) {}

LeaderSignal::LeaderSignal(Ticker ticker,
                          Ticker leader_ticker,
                          SignalBus& bus,
                          const LeaderTracker& tracker)
    : LeaderSignal(std::move(ticker), std::move(leader_ticker), bus, tracker, Config{}) {}

void LeaderSignal::on_frame(const FeatureFrame& frame) {
    if (frame.ticker != ticker_) return;
    
    our_history_.push_back({frame.timestamp, frame.mid});
    while (our_history_.size() > 100) our_history_.pop_front();
    
    check_signal_(frame.timestamp);
}

void LeaderSignal::on_leader_frame(const FeatureFrame& frame) {
    if (frame.ticker != leader_ticker_) return;
    
    leader_history_.push_back({frame.timestamp, frame.mid});
    while (leader_history_.size() > 100) leader_history_.pop_front();
}

void LeaderSignal::check_signal_(std::chrono::system_clock::time_point now) {
    if (leader_history_.size() < 50 || our_history_.size() < 50) return;

    double corr = tracker_.correlation();
    if (std::abs(corr) < cfg_.min_correlation) return;

    // Calculate 5s pct change
    auto get_move = [](const auto& history, auto now) -> double {
        if (history.empty()) return 0.0;
        double cur = history.back().second;
        double prev = 0.0;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if ((now - it->first) >= std::chrono::seconds(5)) {
                prev = it->second;
                break;
            }
        }
        if (prev == 0.0) prev = history.front().second;
        return (cur - prev) / prev * 100.0;
    };

    double leader_move = get_move(leader_history_, now);
    double our_move = get_move(our_history_, now);

    if (std::abs(leader_move) >= cfg_.move_min_pct && 
        (std::signbit(leader_move) == std::signbit(corr) || corr == 0)) {
        
        double expected_move = leader_move * corr;
        double lag_diff = expected_move - our_move;

        if (std::abs(lag_diff) >= cfg_.lag_min_pct) {
            Signal s {
                .kind = SignalKind::LeaderMove,
                .timestamp = now,
                .ticker = ticker_,
                .price = our_history_.back().second,
                .confidence = tracker_.confidence(),
                .payload = nlohmann::json{
                    {"leader_ticker", leader_ticker_},
                    {"leader_move_pct", leader_move},
                    {"our_move_pct", our_move},
                    {"expected_move_pct", expected_move},
                    {"lag_pct", lag_diff},
                    {"correlation", corr},
                    {"lag_ms", tracker_.lag_ms()}
                }
            };
            bus_.publish(s);
        }
    }
}

void LeaderSignal::on_trade(const Trade& /*trade*/) {}
void LeaderSignal::on_book_update(const OrderBookUpdate& /*update*/) {}

} // namespace trade_bot
