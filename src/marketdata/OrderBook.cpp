#include "OrderBook.hpp"

#include "perf/SimdOps.hpp"

#include <array>
#include <cmath>
#include <execution>
#include <algorithm>

namespace trade_bot {

OrderBook::OrderBook(Ticker ticker,
                     double price_increment,
                     double size_increment,
                     std::size_t /*reserve_levels*/)
    : ticker_(std::move(ticker))
    , price_increment_(price_increment)
    , size_increment_(size_increment)
    , inv_price_increment_(1.0 / price_increment)
    , inv_size_increment_(1.0 / size_increment) {
    // absl::btree_map allocates per-node. reserve_levels is ignored here 
    // but kept in API for future PMR/Arena swap-in as per ARCH § 2.3.
}

void OrderBook::apply_snapshot(const OrderBookSnapshot& snap) {
    bids_.clear();
    asks_.clear();
    for (const auto& level : snap.bids) {
        if (level.size <= 0.0) continue;
        const auto tick = PriceTick::from_price_inv(level.price, inv_price_increment_);
        const auto size = SizeFix::from_double_inv(level.size, inv_size_increment_);
        bids_.insert_or_assign(tick, size);
    }
    for (const auto& level : snap.asks) {
        if (level.size <= 0.0) continue;
        const auto tick = PriceTick::from_price_inv(level.price, inv_price_increment_);
        const auto size = SizeFix::from_double_inv(level.size, inv_size_increment_);
        asks_.insert_or_assign(tick, size);
    }
    refresh_top_of_book_();
    top_dirty_ = false;
    ++update_count_;
}

void OrderBook::apply_update(const OrderBookUpdate& upd) {
    for (const auto& change : upd.changes) {
        apply_change_(change.side, change.price, change.size);
    }
    if (top_dirty_) {
        refresh_top_of_book_();
        top_dirty_ = false;
    }
    ++update_count_;
}

void OrderBook::apply_update_batch(const std::vector<PriceLevel>& changes) {
    // THRESHOLD ADJUSTMENT: Threshold raised to 64 to avoid parallel overhead on small batches.
    if (changes.size() >= 64) {
        // ARCH § 2.3: Parallel processing for large batches. 
        // We split by side to avoid race conditions on individual maps.
        std::array<Side, 2> sides = {Side::Buy, Side::Sell};
        std::for_each(std::execution::par_unseq, sides.begin(), sides.end(), [&](Side side) {
            for (const auto& c : changes) {
                if (c.side == side) {
                    apply_change_(c.side, c.price, c.size);
                }
            }
        });
    } else {
        for (const auto& c : changes) {
            apply_change_(c.side, c.price, c.size);
        }
    }
    
    if (top_dirty_) {
        refresh_top_of_book_();
        top_dirty_ = false;
    }
    ++update_count_;
}

void OrderBook::apply_change_(Side side, double price, double size) {
    // Branchless-style hint: updates are more likely than deletes in a busy book
    if (side == Side::None) [[unlikely]] return;

    const auto tick = PriceTick::from_price_inv(price, inv_price_increment_);
    const auto sizefix = SizeFix::from_double_inv(size, inv_size_increment_);

    if (size > 0.0) [[likely]] {
        if (side == Side::Buy) bids_.insert_or_assign(tick, sizefix);
        else                   asks_.insert_or_assign(tick, sizefix);
    } else {
        if (side == Side::Buy) bids_.erase(tick);
        else                   asks_.erase(tick);
    }
    top_dirty_ = true;
}

void OrderBook::refresh_top_of_book_() noexcept {
    best_bid_tick_ = bids_.empty() ? std::nullopt : std::optional<PriceTick>{bids_.begin()->first};
    best_ask_tick_ = asks_.empty() ? std::nullopt : std::optional<PriceTick>{asks_.begin()->first};
}

std::optional<double> OrderBook::best_bid() const noexcept {
    if (!best_bid_tick_) return std::nullopt;
    return best_bid_tick_->to_price(price_increment_);
}

std::optional<double> OrderBook::best_ask() const noexcept {
    if (!best_ask_tick_) return std::nullopt;
    return best_ask_tick_->to_price(price_increment_);
}

std::optional<double> OrderBook::spread() const noexcept {
    if (!best_bid_tick_ || !best_ask_tick_) return std::nullopt;
    return best_ask_tick_->to_price(price_increment_) -
           best_bid_tick_->to_price(price_increment_);
}

std::optional<double> OrderBook::mid() const noexcept {
    if (!best_bid_tick_ || !best_ask_tick_) return std::nullopt;
    return 0.5 * (best_bid_tick_->to_price(price_increment_) +
                  best_ask_tick_->to_price(price_increment_));
}

double OrderBook::bid_depth(int n_levels) const noexcept {
    if (n_levels <= 0 || bids_.empty()) return 0.0;
    
    // Fix for #150: For small N, direct loop is faster than SIMD overhead
    double total = 0.0;
    int count = 0;
    for (const auto& [_, sz] : bids_) {
        total += sz.to_double(size_increment_);
        if (++count >= n_levels) break;
    }
    return total;
}

double OrderBook::ask_depth(int n_levels) const noexcept {
    if (n_levels <= 0 || asks_.empty()) return 0.0;
    
    // Fix for #150: For small N, direct loop is faster than SIMD overhead
    double total = 0.0;
    int count = 0;
    for (const auto& [_, sz] : asks_) {
        total += sz.to_double(size_increment_);
        if (++count >= n_levels) break;
    }
    return total;
}

double OrderBook::volume_at_range(double lo, double hi) const noexcept {
    if (lo > hi) std::swap(lo, hi);
    const auto lo_tick = PriceTick::from_price_inv(lo, inv_price_increment_);
    const auto hi_tick = PriceTick::from_price_inv(hi, inv_price_increment_);
    double total = 0.0;
    
    // Bids (std::greater): higher prices first. 
    // lower_bound(hi_tick) -> first level <= hi_tick
    for (auto it = bids_.lower_bound(hi_tick); it != bids_.end(); ++it) {
        if (it->first < lo_tick) break;
        total += it->second.to_double(size_increment_);
    }
    
    // Asks (std::less): lower prices first.
    // lower_bound(lo_tick) -> first level >= lo_tick
    for (auto it = asks_.lower_bound(lo_tick); it != asks_.end(); ++it) {
        if (it->first > hi_tick) break;
        total += it->second.to_double(size_increment_);
    }
    return total;
}

std::size_t OrderBook::bid_levels() const noexcept { return bids_.size(); }
std::size_t OrderBook::ask_levels() const noexcept { return asks_.size(); }

bool OrderBook::is_consistent(const OrderBookSnapshot& snap, int max_diff) const noexcept {
    int diffs = 0;

    auto check_side = [&](const std::vector<PriceLevel>& snap_levels, const auto& local_map) {
        auto it = local_map.begin();
        for (const auto& sl : snap_levels) {
            if (it == local_map.end()) {
                diffs++;
            } else {
                const auto tick = PriceTick::from_price_inv(sl.price, inv_price_increment_);
                const auto size = SizeFix::from_double_inv(sl.size, inv_size_increment_);
                if (it->first != tick || it->second != size) {
                    diffs++;
                }
                ++it;
            }
            if (diffs > max_diff) return;
        }
    };

    check_side(snap.bids, bids_);
    if (diffs > max_diff) return false;
    check_side(snap.asks, asks_);
    
    return diffs <= max_diff;
}

}  // namespace trade_bot
