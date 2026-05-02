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
    head_ = (head_ + 1) % kMaxTrades;
    if (count_ < kMaxTrades) count_++;

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

    recalculate_volumes_(now);
}

void TradeStream::recalculate_volumes_(std::chrono::system_clock::time_point now) {
    buy_vol_1s_ = {}; buy_vol_5s_ = {}; buy_vol_30s_ = {};
    sell_vol_1s_ = {}; sell_vol_5s_ = {}; sell_vol_30s_ = {};

    size_t i = 0;
    size_t current = (head_ + kMaxTrades - 1) % kMaxTrades;
    
    while (i < count_) {
        const auto& t = trades_[current];
        auto age = std::chrono::duration<double>(now - t.timestamp).count();
        
        if (age > 30.0) break;

        if (t.side == Side::Buy) {
            if (age <= 1.0) buy_vol_1s_.add(t.size);
            if (age <= 5.0) buy_vol_5s_.add(t.size);
            buy_vol_30s_.add(t.size);
        } else if (t.side == Side::Sell) {
            if (age <= 1.0) sell_vol_1s_.add(t.size);
            if (age <= 5.0) sell_vol_5s_.add(t.size);
            sell_vol_30s_.add(t.size);
        }

        current = (current + kMaxTrades - 1) % kMaxTrades;
        i++;
    }
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
    
    auto& dist = const_cast<TDigest&>(size_distribution_);
    s.q99_size = dist.quantile(0.99);

    s.hawkes_intensity_buy = hawkes_intensity_buy_;
    s.hawkes_intensity_sell = hawkes_intensity_sell_;
    s.hawkes_intensity_total = hawkes_intensity_buy_ + hawkes_intensity_sell_;
    
    // Prints per sec approximated by total intensity
    s.prints_per_sec = s.hawkes_intensity_total;

    return s;
}

} // namespace trade_bot
