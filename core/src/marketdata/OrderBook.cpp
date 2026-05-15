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

OrderBook::OrderBook(OrderBook&& other) noexcept
    : ticker_(std::move(other.ticker_))
    , price_increment_(other.price_increment_)
    , size_increment_(other.size_increment_)
    , inv_price_increment_(other.inv_price_increment_)
    , inv_size_increment_(other.inv_size_increment_)
    , bids_(std::move(other.bids_))
    , asks_(std::move(other.asks_))
    , best_bid_tick_(other.best_bid_tick_)
    , best_ask_tick_(other.best_ask_tick_)
    , update_count_(other.update_count_)
    , top_dirty_(other.top_dirty_.load()) {}

OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
    if (this != &other) {
        ticker_ = std::move(other.ticker_);
        price_increment_ = other.price_increment_;
        size_increment_ = other.size_increment_;
        inv_price_increment_ = other.inv_price_increment_;
        inv_size_increment_ = other.inv_size_increment_;
        bids_ = std::move(other.bids_);
        asks_ = std::move(other.asks_);
        best_bid_tick_ = other.best_bid_tick_;
        best_ask_tick_ = other.best_ask_tick_;
        update_count_ = other.update_count_;
        top_dirty_ = other.top_dirty_.load();
    }
    return *this;
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
    // T4-PERF: Threshold raised to 64. Partitioning avoids O(2N) work in parallel branch (#158).
    if (changes.size() >= 64) {
        std::vector<std::pair<PriceTick, SizeFix>> buy_work, sell_work;
        buy_work.reserve(changes.size());
        sell_work.reserve(changes.size());

        for (const auto& c : changes) {
            const auto tick = PriceTick::from_price_inv(c.price, inv_price_increment_);
            const auto size = SizeFix::from_double_inv(c.size, inv_size_increment_);
            if (c.side == Side::Buy) buy_work.emplace_back(tick, size);
            else if (c.side == Side::Sell) sell_work.emplace_back(tick, size);
        }

        auto update_map = [&](auto& map, const std::vector<std::pair<PriceTick, SizeFix>>& work) {
            for (const auto& [tick, size] : work) {
                if (size.raw > 0) map.insert_or_assign(tick, size);
                else map.erase(tick);
            }
        };

        std::array<std::function<void()>, 2> tasks = {
            [&]() { update_map(bids_, buy_work); },
            [&]() { update_map(asks_, sell_work); }
        };
        std::for_each(std::execution::par_unseq, tasks.begin(), tasks.end(), [](auto& f) { f(); });
        top_dirty_ = true;
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
    
    int64_t total_raw = 0;
    int count = 0;
    for (const auto& [_, sz] : bids_) {
        total_raw += sz.raw;
        if (++count >= n_levels) break;
    }
    return static_cast<double>(total_raw) * size_increment_;
}

double OrderBook::ask_depth(int n_levels) const noexcept {
    if (n_levels <= 0 || asks_.empty()) return 0.0;
    
    int64_t total_raw = 0;
    int count = 0;
    for (const auto& [_, sz] : asks_) {
        total_raw += sz.raw;
        if (++count >= n_levels) break;
    }
    return static_cast<double>(total_raw) * size_increment_;
}

double OrderBook::volume_at_range(double lo, double hi) const noexcept {
    if (lo > hi) std::swap(lo, hi);
    const auto lo_tick = PriceTick::from_price_inv(lo, inv_price_increment_);
    const auto hi_tick = PriceTick::from_price_inv(hi, inv_price_increment_);
    int64_t total_raw = 0;
    
    // Bids (std::greater): higher prices first. 
    // lower_bound(hi_tick) -> first level <= hi_tick
    for (auto it = bids_.lower_bound(hi_tick); it != bids_.end(); ++it) {
        if (it->first < lo_tick) break;
        total_raw += it->second.raw;
    }
    
    // Asks (std::less): lower prices first.
    // lower_bound(lo_tick) -> first level >= lo_tick
    for (auto it = asks_.lower_bound(lo_tick); it != asks_.end(); ++it) {
        if (it->first > hi_tick) break;
        total_raw += it->second.raw;
    }
    return static_cast<double>(total_raw) * size_increment_;
}

std::size_t OrderBook::bid_levels() const noexcept { return bids_.size(); }
std::size_t OrderBook::ask_levels() const noexcept { return asks_.size(); }

std::pair<std::vector<ObLevel>, std::vector<ObLevel>>
OrderBook::get_top_levels(int n) const noexcept {
    std::vector<ObLevel> bids, asks;
    if (n <= 0) return {bids, asks};

    bids.reserve(static_cast<std::size_t>(n));
    int count = 0;
    for (const auto& [tick, sz] : bids_) {
        bids.push_back(ObLevel{
            tick.to_price(price_increment_),
            static_cast<double>(sz.raw) * size_increment_
        });
        if (++count >= n) break;
    }

    asks.reserve(static_cast<std::size_t>(n));
    count = 0;
    for (const auto& [tick, sz] : asks_) {
        asks.push_back(ObLevel{
            tick.to_price(price_increment_),
            static_cast<double>(sz.raw) * size_increment_
        });
        if (++count >= n) break;
    }

    return {std::move(bids), std::move(asks)};
}

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
