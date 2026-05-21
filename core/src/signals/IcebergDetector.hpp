#pragma once

#include "IDetector.hpp"
#include "SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "universe/TickerUniverse.hpp"
#include "utils/CircularBuffer.hpp"

#include <absl/container/flat_hash_map.h>
#include <chrono>
#include <optional>

namespace trade_bot {

/**
 * T2-ICEBERG: Detects hidden liquidity (icebergs) using Bayesian evidence.
 */
class IcebergDetector : public IDetector {
public:
    struct Config {
        double prior{0.05};
        double likelihood_iceberg{0.85};
        double likelihood_not_iceberg{0.10};
        double posterior_threshold{0.80};
        
        int    evidence_count_min{3};
        std::chrono::seconds evidence_window{30};
        double iceberg_min_size_usd{100000.0};
        double size_retention_ratio{0.5};
        
        std::chrono::milliseconds event_join_window{100};
    };

    IcebergDetector(Ticker ticker,
                   SignalBus& bus,
                   const OrderBook& book,
                   const TickerUniverse& universe,
                   const Config& cfg);

    IcebergDetector(Ticker ticker,
                   SignalBus& bus,
                   const OrderBook& book,
                   const TickerUniverse& universe);

    void on_frame(const FeatureFrame& frame) override;
    void on_trade(const Trade& trade) override;
    void on_book_update(const OrderBookUpdate& update) override;
    
    const char* perf_stage_name() const noexcept override { return "iceberg_eval_us"; }

private:
    struct LevelStats {
        double posterior{0.0};
        int    refill_count{0};
        double total_eaten_vol{0.0};
        std::chrono::system_clock::time_point last_refill;
        bool   emitted{false};
    };

    struct BookLevelState {
        std::chrono::system_clock::time_point ts;
        double price;
        double size;
        Side   side;
    };

    void process_refill_(double price, Side side, double trade_size, double size_before, double size_after, std::chrono::system_clock::time_point ts);
    void update_bayesian_(LevelStats& stats, bool is_refill);

    Ticker          ticker_;
    SignalBus&      bus_;
    const OrderBook& book_;
    const TickerUniverse& universe_;
    Config          cfg_;

    // To track "before" state. Map price -> size.
    absl::flat_hash_map<PriceTick, double> last_sizes_;
    
    // Per-level evidence
    absl::flat_hash_map<PriceTick, LevelStats> levels_;
    
    // For trade-book synchronization: buffer recent book updates
    // Actually, a simpler way: since trades and updates are sequential in one thread (usually),
    // we can just use the last known size. 
    // BUT the spec says "event-join window 100ms through ring-buffer".
    // This implies trades might arrive before/after the book update that reflects them.
    
    struct TradeEvent {
        std::chrono::system_clock::time_point ts;
        double price;
        double size;
        Side   side;
    };
    
    struct BookEvent {
        std::chrono::system_clock::time_point ts;
        double price;
        double size;
        Side   side;
    };

    static constexpr size_t kMaxHistory = 1024;
    CircularBuffer<TradeEvent, kMaxHistory> trade_history_;
    CircularBuffer<BookEvent,  kMaxHistory> book_history_;
    std::chrono::system_clock::time_point last_prune_;

    // T4-PERF: Persistent maps for temporary volume calculation to avoid allocations (#159)
    absl::flat_hash_map<PriceTick, double> buy_vol_by_tick_;
    absl::flat_hash_map<PriceTick, double> sell_vol_by_tick_;
};

} // namespace trade_bot
