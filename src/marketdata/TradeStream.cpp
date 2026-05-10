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
    if (count_ < kMaxTrades) {
        count_++;
    } else {
        // We just overwrote the oldest trade in the entire buffer.
        // If any tail index was pointing to it, we MUST advance it.
        // But normally tail indices are much closer to head than kMaxTrades.
        // For safety:
        if (tail_1s_ == head_) tail_1s_ = (tail_1s_ + 1) % kMaxTrades;
        if (tail_5s_ == head_) tail_5s_ = (tail_5s_ + 1) % kMaxTrades;
        if (tail_30s_ == head_) tail_30s_ = (tail_30s_ + 1) % kMaxTrades;
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
    if (last_hawkes_update_ != std::chrono::system_clock::time_point{}) {
        auto delta = std::chrono::duration<double>(now - last_hawkes_update_).count();
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
            // Need to cast to non-const for merge (mutable-like behavior)
            auto& mutable_dist = const_cast<TDigest&>(size_distribution_);
            s.q99_size = mutable_dist.quantile(0.99);
            
            const_cast<TradeStream*>(this)->cached_q99_ = s.q99_size;
            const_cast<TradeStream*>(this)->last_merge_weight_ = static_cast<size_t>(total_w);
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
