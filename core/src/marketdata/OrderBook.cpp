#include "OrderBook.hpp"

#include "perf/SimdOps.hpp"

#include <cmath>
#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace trade_bot {

OrderBook::OrderBook(Ticker ticker,
                     double price_increment,
                     double size_increment,
                     std::size_t /*reserve_levels*/)
    : ticker_(std::move(ticker))
    , price_increment_(price_increment)
    , size_increment_(size_increment) {
    // B1-FIX: Validate increments before computing inverses to prevent division by zero
    if (price_increment <= 0.0 || size_increment <= 0.0) {
        throw std::invalid_argument(
            "OrderBook: price_increment and size_increment must be positive (got price=" +
            std::to_string(price_increment) + ", size=" + std::to_string(size_increment) + ")");
    }
    
    inv_price_increment_ = 1.0 / price_increment;
    inv_size_increment_ = 1.0 / size_increment;
    
    // absl::btree_map allocates per-node. reserve_levels is ignored here 
    // but kept in API for future PMR/Arena swap-in as per ARCH § 2.3.
}

OrderBook::OrderBook(OrderBook&& other) noexcept {
    // shared_mutex is not movable — the new object gets a fresh unlocked mutex.
    std::unique_lock<std::shared_mutex> lk(other.mtx_);
    ticker_              = std::move(other.ticker_);
    price_increment_     = other.price_increment_;
    size_increment_      = other.size_increment_;
    inv_price_increment_ = other.inv_price_increment_;
    inv_size_increment_  = other.inv_size_increment_;
    bids_                = std::move(other.bids_);
    asks_                = std::move(other.asks_);
    best_bid_tick_       = other.best_bid_tick_;
    best_ask_tick_       = other.best_ask_tick_;
    update_count_.store(other.update_count_.load(std::memory_order_relaxed));
    top_dirty_           = other.top_dirty_;
}

OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
    if (this != &other) {
        std::unique_lock<std::shared_mutex> lk1(mtx_, std::defer_lock);
        std::unique_lock<std::shared_mutex> lk2(other.mtx_, std::defer_lock);
        std::lock(lk1, lk2);
        ticker_              = std::move(other.ticker_);
        price_increment_     = other.price_increment_;
        size_increment_      = other.size_increment_;
        inv_price_increment_ = other.inv_price_increment_;
        inv_size_increment_  = other.inv_size_increment_;
        bids_                = std::move(other.bids_);
        asks_                = std::move(other.asks_);
        best_bid_tick_       = other.best_bid_tick_;
        best_ask_tick_       = other.best_ask_tick_;
        update_count_.store(other.update_count_.load(std::memory_order_relaxed));
        top_dirty_           = other.top_dirty_;
    }
    return *this;
}

void OrderBook::apply_snapshot(const OrderBookSnapshot& snap) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
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
    m_synced = true;
    update_count_.fetch_add(1, std::memory_order_relaxed);
}

void OrderBook::apply_update(const OrderBookUpdate& upd) noexcept {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    if (!m_synced) {
        m_synced = true;
    }
    for (const auto& change : upd.changes) {
        apply_change_(change.side, change.price, change.size);
    }
    if (top_dirty_) {
        refresh_top_of_book_();
        top_dirty_ = false;
    }
    update_count_.fetch_add(1, std::memory_order_relaxed);
}

void OrderBook::apply_update_batch(const std::vector<PriceLevel>& changes) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    if (!m_synced) {
        m_synced = true;
    }
    // Exclusive lock is held throughout; parallel tasks safely modify separate maps
    // because no concurrent readers can enter during exclusive ownership.
    if (changes.size() >= 64) {
        std::vector<std::pair<PriceTick, SizeFix>> buy_work, sell_work;
        buy_work.reserve(changes.size());
        sell_work.reserve(changes.size());

        for (const auto& c : changes) {
            if (c.side != Side::Buy && c.side != Side::Sell) {
                apply_change_(c.side, c.price, c.size);
                continue;
            }
            const auto tick = PriceTick::from_price_inv(c.price, inv_price_increment_);
            const auto size = SizeFix::from_double_inv(c.size, inv_size_increment_);
            if (c.side == Side::Buy) buy_work.emplace_back(tick, size);
            else sell_work.emplace_back(tick, size);
        }

        auto update_map = [](auto& map, const std::vector<std::pair<PriceTick, SizeFix>>& work) {
            for (const auto& [tick, size] : work) {
                if (size.raw > 0) map.insert_or_assign(tick, size);
                else map.erase(tick);
            }
        };

        top_dirty_ = true;
        update_map(bids_, buy_work);
        update_map(asks_, sell_work);

        // Find maximum new bid and minimum new ask with size > 0 to resolve overlaps
        std::optional<PriceTick> max_new_bid;
        std::optional<PriceTick> min_new_ask;

        for (const auto& [tick, size] : buy_work) {
            if (size.raw > 0) {
                if (!max_new_bid || tick > *max_new_bid) {
                    max_new_bid = tick;
                }
            }
        }
        for (const auto& [tick, size] : sell_work) {
            if (size.raw > 0) {
                if (!min_new_ask || tick < *min_new_ask) {
                    min_new_ask = tick;
                }
            }
        }

        // Clean up crossed levels on opposite sides
        if (max_new_bid) {
            while (!asks_.empty() && asks_.begin()->first <= *max_new_bid) {
                asks_.erase(asks_.begin());
            }
        }
        if (min_new_ask) {
            while (!bids_.empty() && bids_.begin()->first >= *min_new_ask) {
                bids_.erase(bids_.begin());
            }
        }
    } else {
        for (const auto& c : changes) {
            apply_change_(c.side, c.price, c.size);
        }
    }

    if (top_dirty_) {
        refresh_top_of_book_();
        top_dirty_ = false;
    }
    update_count_.fetch_add(1, std::memory_order_relaxed);
}

void OrderBook::apply_change_(Side side, double price, double size) noexcept {
    // Some MetaScalp orderbook_update encodings don't carry an explicit,
    // parseable side. Rather than silently drop the level (which leaves the
    // book stuck at the thin initial snapshot), infer it from price vs. the
    // current touch: at/below best-bid → bid, at/above best-ask → ask.
    if (side == Side::None) [[unlikely]] {
        // K4: ensure cached top is fresh before using it for side inference
        if (top_dirty_) { refresh_top_of_book_(); top_dirty_ = false; }
        // Access tick cache directly (avoid recursive lock via public best_bid()/best_ask())
        const auto bb = best_bid_tick_ ? std::optional<double>{best_bid_tick_->to_price(price_increment_)} : std::nullopt;
        const auto ba = best_ask_tick_ ? std::optional<double>{best_ask_tick_->to_price(price_increment_)} : std::nullopt;
        if (bb && ba)       side = (price <= 0.5 * (*bb + *ba)) ? Side::Buy : Side::Sell;
        else if (bb)        side = (price <= *bb) ? Side::Buy : Side::Sell;
        else if (ba)        side = (price >= *ba) ? Side::Sell : Side::Buy;
        else                return; // empty book, no reference yet — snapshot seeds it
    }

    const auto tick = PriceTick::from_price_inv(price, inv_price_increment_);
    const auto sizefix = SizeFix::from_double_inv(size, inv_size_increment_);

    if (size > 0.0) [[likely]] {
        // Hot path: update an existing level or insert a new one in a live book.
        if (side == Side::Buy) {
            bids_.insert_or_assign(tick, sizefix);
            while (!asks_.empty() && asks_.begin()->first <= tick) {
                asks_.erase(asks_.begin());
            }
        } else {
            asks_.insert_or_assign(tick, sizefix);
            while (!bids_.empty() && bids_.begin()->first >= tick) {
                bids_.erase(bids_.begin());
            }
        }
    } else [[unlikely]] {
        // Cold path: level removal (size == 0) is comparatively rare.
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
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (!best_bid_tick_) return std::nullopt;
    return best_bid_tick_->to_price(price_increment_);
}

std::optional<double> OrderBook::best_ask() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (!best_ask_tick_) return std::nullopt;
    return best_ask_tick_->to_price(price_increment_);
}

std::optional<double> OrderBook::spread() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (!best_bid_tick_ || !best_ask_tick_) return std::nullopt;
    return best_ask_tick_->to_price(price_increment_) -
           best_bid_tick_->to_price(price_increment_);
}

std::optional<double> OrderBook::mid() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (!best_bid_tick_ || !best_ask_tick_) return std::nullopt;
    return 0.5 * (best_bid_tick_->to_price(price_increment_) +
                  best_ask_tick_->to_price(price_increment_));
}

double OrderBook::bid_depth(int n_levels) const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (n_levels <= 0 || bids_.empty()) return 0.0;
    // K5: accumulate as double to avoid int64_t overflow on large books
    double total = 0.0;
    int count = 0;
    for (const auto& [_, sz] : bids_) {
        total += static_cast<double>(sz.raw) * size_increment_;
        if (++count >= n_levels) break;
    }
    return total;
}

double OrderBook::ask_depth(int n_levels) const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (n_levels <= 0 || asks_.empty()) return 0.0;
    // K5: accumulate as double to avoid int64_t overflow on large books
    double total = 0.0;
    int count = 0;
    for (const auto& [_, sz] : asks_) {
        total += static_cast<double>(sz.raw) * size_increment_;
        if (++count >= n_levels) break;
    }
    return total;
}

double OrderBook::volume_at_range(double lo, double hi) const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
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

std::size_t OrderBook::bid_levels() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return bids_.size();
}
std::size_t OrderBook::ask_levels() const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return asks_.size();
}

std::pair<std::vector<ObLevel>, std::vector<ObLevel>>
OrderBook::get_top_levels(int n) const noexcept {
    std::vector<ObLevel> bids, asks;
    get_top_levels(n, bids, asks);
    return {std::move(bids), std::move(asks)};
}

void OrderBook::get_top_levels(int n, std::vector<ObLevel>& bids, std::vector<ObLevel>& asks) const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    bids.clear();
    asks.clear();
    if (n <= 0) return;

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
}

bool OrderBook::is_consistent(const OrderBookSnapshot& snap, int max_diff) const noexcept {
    std::shared_lock<std::shared_mutex> lk(mtx_);
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
