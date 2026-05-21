#include "TradeStream.hpp"
#include <cmath>
#include <algorithm>

namespace trade_bot {

TradeStream::TradeStream(Ticker ticker, double hawkes_alpha, double hawkes_beta)
    : ticker_(std::move(ticker))
    , hawkes_alpha_(hawkes_alpha)
    , hawkes_beta_(hawkes_beta) {
    trades_.resize(kMaxTrades);
}

void TradeStream::on_trade(const Trade& trade) {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    // S1: save the evicted trade before overwriting (only matters when buffer is full)
    const bool full = (count_ >= kMaxTrades);
    const size_t old_head = head_;
    const Trade evicted = full ? trades_[head_] : Trade{};

    // Add to ring buffer
    trades_[head_] = trade;

    // Incremental volume addition
    if (trade.side == Side::Buy) {
        buy_vol_1s_.add(trade.size);
        buy_vol_5s_.add(trade.size);
        buy_vol_30s_.add(trade.size);
    } else if (trade.side == Side::Sell) {
        sell_vol_1s_.add(trade.size);
        sell_vol_5s_.add(trade.size);
        sell_vol_30s_.add(trade.size);
    }

    head_ = (head_ + 1) % kMaxTrades;
    if (!full) {
        count_++;
    } else {
        // S1: subtract evicted trade from any window whose tail was still pointing at it
        auto sub_if_evicted = [&](size_t& tail, KahanAccumulator<double>& buy_acc, KahanAccumulator<double>& sell_acc) {
            if (tail == old_head) {
                if (evicted.side == Side::Buy)        buy_acc.add(-evicted.size);
                else if (evicted.side == Side::Sell)  sell_acc.add(-evicted.size);
                tail = (tail + 1) % kMaxTrades;
            }
        };
        sub_if_evicted(tail_1s_,  buy_vol_1s_,  sell_vol_1s_);
        sub_if_evicted(tail_5s_,  buy_vol_5s_,  sell_vol_5s_);
        sub_if_evicted(tail_30s_, buy_vol_30s_, sell_vol_30s_);
    }

    // Incremental stats
    size_stats_.update(trade.size);
    size_distribution_.add(trade.size);

    // Hawkes jump
    if (trade.side == Side::Buy) {
        hawkes_intensity_buy_ += hawkes_alpha_;
    } else if (trade.side == Side::Sell) {
        hawkes_intensity_sell_ += hawkes_alpha_;
    }
}

void TradeStream::update(std::chrono::system_clock::time_point now) {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    if (last_hawkes_update_.has_value()) {
        auto delta = std::chrono::duration<double>(now - *last_hawkes_update_).count();
        // Decay
        double decay = std::exp(-hawkes_beta_ * delta);
        hawkes_intensity_buy_ *= decay;
        hawkes_intensity_sell_ *= decay;
    }
    last_hawkes_update_ = now;

    evict_expired_trades_(now);
}
void TradeStream::evict_expired_trades_(std::chrono::system_clock::time_point now) {
    auto evict = [&](size_t& tail, double seconds, KahanAccumulator<double>& buy_acc, KahanAccumulator<double>& sell_acc) {
        while (tail != head_) {
            const auto& t = trades_[tail];
            auto age = std::chrono::duration<double>(now - t.timestamp).count();
            if (age <= seconds) break;

            if (t.side == Side::Buy) buy_acc.add(-t.size);
            else if (t.side == Side::Sell) sell_acc.add(-t.size);
            
            tail = (tail + 1) % kMaxTrades;
        }
    };

    evict(tail_1s_, 1.0, buy_vol_1s_, sell_vol_1s_);
    evict(tail_5s_, 5.0, buy_vol_5s_, sell_vol_5s_);
    evict(tail_30s_, 30.0, buy_vol_30s_, sell_vol_30s_);
}


TradeStream::Stats TradeStream::get_stats() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    Stats s;
    s.avg_size = size_stats_.mean();
    s.stdev_size = size_stats_.stdev();
    
    s.buy_vol_1s = buy_vol_1s_.sum();
    s.buy_vol_5s = buy_vol_5s_.sum();
    s.buy_vol_30s = buy_vol_30s_.sum();
    
    s.sell_vol_1s = sell_vol_1s_.sum();
    s.sell_vol_5s = sell_vol_5s_.sum();
    s.sell_vol_30s = sell_vol_30s_.sum();

    // quantile(0.99) triggers TDigest::merge() which calls std::sort.
    // Calling it every 100ms for every ticker is expensive.
    // T4-PERF: Only re-merge if we have significant new data (e.g. 50 new prints)
    double total_w = size_distribution_.total_weight();
    if (total_w > 0) {
        if (total_w > static_cast<double>(last_merge_weight_ + 50)) {
            s.q99_size = size_distribution_.quantile(0.99);
            cached_q99_ = s.q99_size;
            last_merge_weight_ = static_cast<size_t>(total_w);
        } else {
            s.q99_size = cached_q99_;
        }
    } else {
        s.q99_size = 0.0;
    }

    s.hawkes_intensity_buy = hawkes_intensity_buy_;
    s.hawkes_intensity_sell = hawkes_intensity_sell_;
    s.hawkes_intensity_total = hawkes_intensity_buy_ + hawkes_intensity_sell_;
    
    // Prints per sec approximated by total intensity
    s.prints_per_sec = s.hawkes_intensity_total;

    return s;
}

} // namespace trade_bot
