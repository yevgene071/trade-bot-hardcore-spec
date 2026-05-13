#pragma once

#include "domain/Types.hpp"
#include "numeric/FixedPoint.hpp"

#include <absl/container/btree_map.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace trade_bot {

/**
 * Lightweight order book level for dashboard display.
 */
struct ObLevel {
    double price{0.0};
    double size{0.0};
};

/**
 * T1-ORDERBOOK: local replica of the limit order book for a single ticker.
 *
 * Keys are fixed-point `PriceTick` values (PriceIncrement-scaled int64_t)
 * to avoid the precision drift that would happen with double-keyed maps.
 *
 * Storage: absl::btree_map gives cache-friendly contiguous nodes (B-tree
 * layout) and O(log N) ops. Bids use std::greater so begin() is the best
 * (highest) bid; asks use the default less so begin() is the best (lowest)
 * ask. Top-of-book is cached and refreshed on every mutation.
 *
 * Invariants:
 *   - size==0 in an update removes the level
 *   - on apply_snapshot the full state is replaced (no merge)
 *   - update_count_ counts every accepted change for sanity tests
 */
class OrderBook {
public:
    OrderBook(Ticker ticker,
              double price_increment,
              double size_increment,
              std::size_t reserve_levels = 128);

    // OrderBook contains std::atomic, so we need custom move operations
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&& other) noexcept;
    OrderBook& operator=(OrderBook&& other) noexcept;

    void apply_snapshot(const OrderBookSnapshot& snap);
    void apply_update(const OrderBookUpdate& upd);

    /// Apply a heterogeneous batch of changes.
    void apply_update_batch(const std::vector<PriceLevel>& changes);

    [[nodiscard]] std::optional<double> best_bid() const noexcept;
    [[nodiscard]] std::optional<double> best_ask() const noexcept;
    [[nodiscard]] std::optional<double> spread()   const noexcept;
    [[nodiscard]] std::optional<double> mid()      const noexcept;

    /// Sum of sizes for the top `n_levels` of each side.
    [[nodiscard]] double bid_depth(int n_levels) const noexcept;
    [[nodiscard]] double ask_depth(int n_levels) const noexcept;

    /// Sum of sizes whose price falls in the inclusive range [lo, hi].
    [[nodiscard]] double volume_at_range(double lo, double hi) const noexcept;

    [[nodiscard]] std::size_t bid_levels() const noexcept;
    [[nodiscard]] std::size_t ask_levels() const noexcept;

    [[nodiscard]] const Ticker& ticker() const noexcept { return ticker_; }
    [[nodiscard]] int64_t update_count() const noexcept { return update_count_; }
    [[nodiscard]] double  price_increment() const noexcept { return price_increment_; }
    [[nodiscard]] double  size_increment() const noexcept { return size_increment_; }
    [[nodiscard]] double  inv_price_increment() const noexcept { return inv_price_increment_; }
    [[nodiscard]] double  inv_size_increment() const noexcept { return inv_size_increment_; }

    /**
     * Slices the top N levels of each side and compares with a snapshot.
     * Returns true if the number of differing levels (price or size) is <= max_diff.
     */
    [[nodiscard]] bool is_consistent(const OrderBookSnapshot& snap, int max_diff = 3) const noexcept;

    /**
     * Returns the top N levels of each side as ObLevel vectors.
     * Bids are returned in descending price order (best first).
     * Asks are returned in ascending price order (best first).
     */
    [[nodiscard]] std::pair<std::vector<ObLevel>, std::vector<ObLevel>>
    get_top_levels(int n) const noexcept;

private:
    using BidMap = absl::btree_map<PriceTick, SizeFix, std::greater<PriceTick>>;
    using AskMap = absl::btree_map<PriceTick, SizeFix>;

    void apply_change_(Side side, double price, double size);
    void refresh_top_of_book_() noexcept;

    Ticker      ticker_;
    double      price_increment_;
    double      size_increment_;
    double      inv_price_increment_;
    double      inv_size_increment_;
    BidMap      bids_;
    AskMap      asks_;
    std::optional<PriceTick> best_bid_tick_;
    std::optional<PriceTick> best_ask_tick_;
    int64_t             update_count_{0};
    std::atomic<bool>   top_dirty_{false};
};

}  // namespace trade_bot
