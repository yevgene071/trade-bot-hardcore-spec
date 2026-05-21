#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/LeaderTracker.hpp"
#include "utils/CircularBuffer.hpp"
#include <chrono>

namespace trade_bot {

/**
 * T2-LEADER: Generates LeaderMove signals based on LeaderTracker data.
 */
class LeaderSignal : public IDetector {
public:
    struct Config {
        double min_correlation{0.6};
        double move_min_pct{0.3};
        double lag_min_pct{0.15};
        std::chrono::milliseconds lag_max_age{3000};
    };

    LeaderSignal(Ticker ticker,
                Ticker leader_ticker,
                SignalBus& bus,
                const LeaderTracker& tracker,
                const Config& cfg);

    LeaderSignal(Ticker ticker,
                Ticker leader_ticker,
                SignalBus& bus,
                const LeaderTracker& tracker);

    void on_frame(const FeatureFrame& frame) override;
    void on_leader_frame(const FeatureFrame& frame);
    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBookUpdate& update) override;
    
    const char* perf_stage_name() const noexcept override { return "leader_eval_us"; }

private:
    void check_signal_(std::chrono::system_clock::time_point now);

    Ticker          ticker_;
    Ticker          leader_ticker_;
    SignalBus&      bus_;
    const LeaderTracker& tracker_;
    Config          cfg_;

    // 60 000 записей = 10 мин при 100 Hz (тик 10 мс).
    // Было 128 (12.8 с при старом тике 100 мс), стало 1280 мс при 10 мс →
    // get_move() не находил 5-секундной давности запись и брал history[0],
    // занижая leader_move в ~3.9× и блокируя эмиссию сигналов.
    // Память: ~960 KB на буфер, ~1.92 MB на инстанс — некритично.
    CircularBuffer<std::pair<std::chrono::system_clock::time_point, double>, 60000> leader_history_;
    CircularBuffer<std::pair<std::chrono::system_clock::time_point, double>, 60000> our_history_;
};

} // namespace trade_bot
