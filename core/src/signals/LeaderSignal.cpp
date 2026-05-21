#include "LeaderSignal.hpp"
#include "perf/TraceContext.hpp"
#include <cmath>
#include <algorithm>
#include <print>

namespace trade_bot {

LeaderSignal::LeaderSignal(Ticker ticker,
                           Ticker leader_ticker,
                           SignalBus& bus,
                           const LeaderTracker& tracker,
                           const Config& cfg)
    : ticker_(std::move(ticker)), leader_ticker_(std::move(leader_ticker)), bus_(bus), tracker_(tracker), cfg_(cfg) {}

LeaderSignal::LeaderSignal(Ticker ticker,
                          Ticker leader_ticker,
                          SignalBus& bus,
                          const LeaderTracker& tracker)
    : LeaderSignal(std::move(ticker), std::move(leader_ticker), bus, tracker, Config{}) {}

void LeaderSignal::on_frame(const FeatureFrame& frame) {
    static int our_call_count = 0;
    if (++our_call_count % 500 == 0) {
        std::print(stderr, "[DEBUG on_frame] ticker_={} frame.ticker={} valid={} size_before={}\n",
                   ticker_, frame.ticker, frame.valid, our_history_.size());
    }
    if (frame.ticker != ticker_) return;
    if (!frame.valid) return;
    
    our_history_.push_back({frame.timestamp, frame.mid});
    
    check_signal_(frame.timestamp);
}

void LeaderSignal::on_leader_frame(const FeatureFrame& frame) {
    static int leader_call_count = 0;
    if (++leader_call_count % 500 == 0) {
        std::print(stderr, "[DEBUG on_leader_frame] leader_ticker_={} frame.ticker={} valid={} size_before={}\n",
                   leader_ticker_, frame.ticker, frame.valid, leader_history_.size());
    }
    if (frame.ticker != leader_ticker_) return;
    if (!frame.valid) return;
    
    leader_history_.push_back({frame.timestamp, frame.mid});
}

void LeaderSignal::check_signal_(std::chrono::system_clock::time_point now) {
    static int call_count = 0;
    if (++call_count % 500 == 0) {
        std::print(stderr, "[DEBUG LeaderSignal] ticker={} leader_ticker={} leader_hist={} our_hist={} corr={:.4f} min_corr={:.4f}\n",
                   ticker_, leader_ticker_, leader_history_.size(), our_history_.size(), tracker_.correlation(), cfg_.min_correlation);
    }

    if (leader_history_.size() < 50 || our_history_.size() < 50) return;

    double corr = tracker_.correlation();
    if (std::abs(corr) < cfg_.min_correlation) return;

    // Calculate 5s pct change
    auto get_move = [](const auto& history, auto now) -> double {
        if (history.empty()) return 0.0;
        double cur = history.back().second;
        if (cur == 0.0) return 0.0;
        double prev = 0.0;
        bool found = false;
        
        // AC2-FIX: Binary search instead of O(N) linear scan
        int low = 0;
        int high = static_cast<int>(history.size()) - 1;
        int best_idx = -1;
        const auto target_time = now - std::chrono::seconds(5);
        
        while (low <= high) {
            int mid = low + (high - low) / 2;
            if (history[mid].first <= target_time) {
                best_idx = mid;
                low = mid + 1; // Try to find a later element still <= target_time
            } else {
                high = mid - 1;
            }
        }
        
        if (best_idx != -1) {
            prev = history[best_idx].second;
            found = true;
        }
        if (!found) prev = history[0].second;
        if (prev == 0.0) return 0.0; // guard against division by zero
        return (cur - prev) / prev * 100.0;
    };

    double leader_move = get_move(leader_history_, now);
    double our_move = get_move(our_history_, now);

    // GAP-04 FIX: removed signbit equality filter which blocked all short-direction signals.
    // Signal direction is encoded in lag_pct sign (positive = BUY catch-up, negative = SELL).
    if (std::abs(leader_move) >= cfg_.move_min_pct) {
        double expected_move = leader_move * corr;
        double lag_diff = expected_move - our_move;

        if (std::abs(lag_diff) >= cfg_.lag_min_pct) {
            Signal s {
                .kind = SignalKind::LeaderMove,
                .timestamp = now,
                .ticker = ticker_,
                .price = our_history_.back().second,
                .confidence = tracker_.confidence(),
                .payload = {
                    .lag_pct = lag_diff,
                    .correlation = corr,
                    .leader_move_pct = leader_move,
                    .our_move_pct = our_move,
                    .expected_move_pct = expected_move,
                    .lag_ms = static_cast<double>(tracker_.lag_ms())
                }
            };
            s.trigger_trace_id = current_trace_context().trace_id;
            bus_.publish(s);
        }
    }
}

void LeaderSignal::on_trade(const Trade& /*trade*/) {}
void LeaderSignal::on_book_update(const OrderBookUpdate& /*update*/) {}

} // namespace trade_bot
