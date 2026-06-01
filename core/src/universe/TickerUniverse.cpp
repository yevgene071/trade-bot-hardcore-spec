#include "TickerUniverse.hpp"

#include "logger/Logger.hpp"

#include <absl/container/flat_hash_set.h>
#include <algorithm>

namespace trade_bot {

TickerUniverse::TickerUniverse() : TickerUniverse(Config{}) {}

TickerUniverse::TickerUniverse(Config cfg)
    : cfg_(std::move(cfg))
    , filters_(cfg_.filters) {}

void TickerUniverse::set_stats_lookup(StatsLookup fn) { stats_lookup_ = std::move(fn); }

void TickerUniverse::set_affinity_change_handler(AffinityChange fn) {
    on_change_ = std::move(fn);
}

void TickerUniverse::register_strategy(const std::string& name, AffinityScore fn) {
    strategies_[name] = std::move(fn);
}

bool TickerUniverse::passes_filter_locked(const Ticker& ticker, double& out_volume,
                                          const absl::flat_hash_map<Ticker, TickerStats>& temp_stats) const {
    out_volume = 0.0;
    if (!filters_.accepts(ticker)) return false;

    std::optional<TickerStats> s;
    auto it = temp_stats.find(ticker);
    if (it != temp_stats.end()) {
        s = it->second;
    } else if (stats_lookup_) {
        s = stats_lookup_(ticker);
    }

    // Screener-approved coins bypass the dynamic stats gate — they enter
    // the pool immediately and warm up via live market data.
    const bool screener_override = screener_approved_.count(ticker) > 0;
    if (screener_override) {
        if (s) out_volume = s->volume_24h_usd;
        return true;
    }

    // Exchange-aware volume threshold: scale by exchange multiplier.
    double vol_threshold = cfg_.min_volume_24h_usd * cfg_.exchange_volume_multiplier;

    if (!s) {
        // Bug #2: For non-screener coins, no stats means they can't pass filter yet.
        // But for ANY coin without stats we should still allow them into the pool
        // if they passed static filters — otherwise the pool never warms up.
        // Return true here and let the affinity lambdas gate on real volume/spread
        // when stats eventually arrive. The volume-based ordering will still
        // prioritize coins that do have stats.
        return true;
    }
    if (s->volume_24h_usd < vol_threshold) return false;
    if (s->avg_spread_bps > cfg_.max_avg_spread_bps) return false;
    out_volume = s->volume_24h_usd;
    return true;
}

void TickerUniverse::refresh_pool(const std::vector<Ticker>& available, std::chrono::system_clock::time_point now) {
    absl::flat_hash_map<Ticker, TickerStats> temp_stats;
    if (stats_lookup_) {
        for (const auto& t : available) {
            if (auto s = stats_lookup_(t)) {
                temp_stats[t] = *s;
            }
        }
        std::vector<Ticker> screener_coins;
        {
            std::shared_lock lock(rw_mtx_);
            screener_coins.assign(screener_approved_.begin(), screener_approved_.end());
        }
        for (const auto& t : screener_coins) {
            if (!temp_stats.contains(t)) {
                if (auto s = stats_lookup_(t)) {
                    temp_stats[t] = *s;
                }
            }
        }
    }

    std::unique_lock lock(rw_mtx_);
    refresh_pool_locked(available, now, temp_stats);
}

void TickerUniverse::refresh_pool_locked(const std::vector<Ticker>& available, std::chrono::system_clock::time_point now,
                                         const absl::flat_hash_map<Ticker, TickerStats>& temp_stats) {
    std::vector<std::pair<Ticker, double>> survivors;
    survivors.reserve(available.size());

    for (const auto& t : available) {
        double vol = 0.0;
        if (passes_filter_locked(t, vol, temp_stats)) {
            survivors.emplace_back(t, vol);
        }
    }
    std::sort(survivors.begin(), survivors.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    if (survivors.size() > cfg_.max_pool_size) {
        survivors.resize(cfg_.max_pool_size);
    }

    active_.clear();
    active_.resize(survivors.size());
    std::transform(survivors.begin(), survivors.end(), active_.begin(), [](auto& s) {
        return std::move(s.first);
    });

    // Drop affinity entries for tickers that fell out of the pool.
    const std::set<Ticker> active_set(active_.begin(), active_.end());
    for (auto it = affinity_.begin(); it != affinity_.end();) {
        if (!active_set.count(it->first)) {
            it = affinity_.erase(it);
        } else {
            ++it;
        }
    }

    // FEAT-06: track first-active time for stability check (§0.7 affinity_stable_min).
    // Clear timers for tickers that left the pool; set for new entrants.
    const auto pool_now = now;
    for (auto it = active_since_.begin(); it != active_since_.end();) {
        if (!active_set.count(it->first)) it = active_since_.erase(it);
        else ++it;
    }
    for (const auto& t : active_) {
        active_since_.emplace(t, pool_now); // emplace is no-op if key exists
    }

    // Recompute per-coin scale factors for dynamic thresholds.
    refresh_scale_factors_locked(temp_stats);
}

void TickerUniverse::refresh_affinity() {
    PendingChanges changes;
    {
        std::unique_lock lock(rw_mtx_);
        changes = collect_affinity_changes_();
    }
    fire_affinity_changes_(changes);
}

TickerUniverse::PendingChanges TickerUniverse::collect_affinity_changes_() {
    PendingChanges changes;
    for (const auto& t : active_) {
        auto& tickerMap = affinity_[t];
        for (const auto& [name, fn] : strategies_) {
            const bool enabled = fn ? fn(t) : false;
            const auto it = tickerMap.find(name);
            const bool prev = (it != tickerMap.end()) ? it->second : false;
            if (it == tickerMap.end() || prev != enabled) {
                tickerMap[name] = enabled;
                if (it == tickerMap.end() ? enabled : true) {
                    changes.emplace_back(t, name, enabled);
                }
            }
        }
    }
    return changes;
}

void TickerUniverse::fire_affinity_changes_(PendingChanges& changes) {
    if (on_change_) {
        for (auto& [t, name, enabled] : changes) {
            on_change_(t, name, enabled);
        }
    }
}

void TickerUniverse::override_affinity(const Ticker& ticker,
                                        const std::string& strategy, bool enabled) {
    bool changed = false;
    {
        std::unique_lock lock(rw_mtx_);
        auto& tickerMap = affinity_[ticker];
        const bool prev = tickerMap[strategy];
        tickerMap[strategy] = enabled;
        changed = (prev != enabled);
    }
    if (changed && on_change_) {
        on_change_(ticker, strategy, enabled);
    }
}

const std::vector<Ticker>& TickerUniverse::active() const {
    std::shared_lock lock(rw_mtx_);
    return active_;
}
std::size_t TickerUniverse::pool_size() const noexcept {
    std::shared_lock lock(rw_mtx_);
    return active_.size();
}

bool TickerUniverse::is_in_pool(const Ticker& ticker) const {
    std::shared_lock lock(rw_mtx_);
    return affinity_.contains(ticker);
}

bool TickerUniverse::is_strategy_enabled(const Ticker& ticker,
                                         const std::string& strategy) const {
    std::shared_lock lock(rw_mtx_);
    auto it = affinity_.find(ticker);
    if (it == affinity_.end()) return false;
    auto sit = it->second.find(strategy);
    return sit != it->second.end() && sit->second;
}

std::vector<std::string> TickerUniverse::enabled_strategies(const Ticker& ticker) const {
    std::shared_lock lock(rw_mtx_);
    std::vector<std::string> out;
    auto it = affinity_.find(ticker);
    if (it == affinity_.end()) return out;
    for (const auto& [name, on] : it->second) {
        if (on) out.push_back(name);
    }
    return out;
}

void TickerUniverse::on_big_tick(const Ticker& ticker, double /*size_usd*/,
                                 std::chrono::system_clock::time_point now) {
    PendingChanges changes;
    {
        std::unique_lock lock(rw_mtx_);
        on_big_event_locked(ticker, now);
        changes = collect_affinity_changes_();
    }
    fire_affinity_changes_(changes);
}

void TickerUniverse::on_big_amount(const Ticker& ticker, double /*size_usd*/,
                                   std::chrono::system_clock::time_point now) {
    PendingChanges changes;
    {
        std::unique_lock lock(rw_mtx_);
        on_big_event_locked(ticker, now);
        changes = collect_affinity_changes_();
    }
    fire_affinity_changes_(changes);
}

void TickerUniverse::seed_candidates(const std::vector<Ticker>& candidates) {
    std::unique_lock lock(rw_mtx_);
    all_candidates_ = candidates;
}

void TickerUniverse::on_screener_new_coin(const Ticker& ticker, std::chrono::system_clock::time_point now) {
    PendingChanges changes;
    absl::flat_hash_map<Ticker, TickerStats> temp_stats;
    std::vector<Ticker> pool_candidates;
    bool should_rebuild = false;

    {
        std::shared_lock lock(rw_mtx_);
        if (!filters_.accepts(ticker)) return;
        if (screener_approved_.count(ticker) == 0) {
            should_rebuild = true;
            pool_candidates.reserve(all_candidates_.size());
            absl::flat_hash_set<Ticker> added;
            for (const auto& t : all_candidates_) {
                if (screener_approved_.count(t) || t == ticker) {
                    pool_candidates.push_back(t);
                    added.insert(t);
                }
            }
            for (const auto& t : screener_approved_) {
                if (!added.contains(t)) {
                    pool_candidates.push_back(t);
                }
            }
            if (!added.contains(ticker)) {
                pool_candidates.push_back(ticker);
            }
        }
    }

    if (should_rebuild) {
        if (stats_lookup_) {
            for (const auto& t : pool_candidates) {
                if (auto s = stats_lookup_(t)) {
                    temp_stats[t] = *s;
                }
            }
            std::vector<Ticker> screener_coins;
            {
                std::shared_lock lock(rw_mtx_);
                screener_coins.assign(screener_approved_.begin(), screener_approved_.end());
            }
            for (const auto& t : screener_coins) {
                if (!temp_stats.contains(t)) {
                    if (auto s = stats_lookup_(t)) {
                        temp_stats[t] = *s;
                    }
                }
            }
        }

        std::unique_lock lock(rw_mtx_);
        if (screener_approved_.insert(ticker).second) {
            LOG_INFO("[Universe] Screener: {} approved, rebuilding pool ({} screener coins)",
                     ticker, screener_approved_.size());
            refresh_pool_locked(pool_candidates, now, temp_stats);
            changes = collect_affinity_changes_();
        }
    }
    fire_affinity_changes_(changes);
}

void TickerUniverse::on_big_event(const Ticker& ticker,
                                  std::chrono::system_clock::time_point now) {
    PendingChanges changes;
    {
        std::unique_lock lock(rw_mtx_);
        on_big_event_locked(ticker, now);
        changes = collect_affinity_changes_();
    }
    fire_affinity_changes_(changes);
}

void TickerUniverse::on_big_event_locked(const Ticker& ticker,
                                   std::chrono::system_clock::time_point now) {
    // AG3: also clean expired boosts before adding a new one
    for (auto it = boosts_.begin(); it != boosts_.end(); ) {
        if ((now - it->second) > cfg_.boost_ttl) it = boosts_.erase(it);
        else ++it;
    }
    boosts_[ticker] = now;
}

void TickerUniverse::update_orderbook_settings(const OrderbookSettings& settings) {
    std::unique_lock lock(rw_mtx_);
    large_amounts_[settings.ticker] = settings.large_amount_usd;
}

double TickerUniverse::density_min_size_usd(const Ticker& ticker,
                                            double config_default) const {
    std::shared_lock lock(rw_mtx_);
    // Base calibrated value (volume scale factor + exchange multiplier + floor).
    double base = calibrated_min_size_usd_locked(ticker, config_default);
    auto it = large_amounts_.find(ticker);
    if (it != large_amounts_.end()) {
        // MetaScalp per-ticker large_amount_usd also gets exchange scaling.
        double calibrated_large = it->second * cfg_.exchange_volume_multiplier;
        return std::max(base, calibrated_large);
    }
    return base;
}

bool TickerUniverse::is_boosted(const Ticker& ticker,
                                std::chrono::system_clock::time_point now) const {
    std::shared_lock lock(rw_mtx_);
    auto it = boosts_.find(ticker);
    if (it == boosts_.end()) return false;
    return (now - it->second) <= cfg_.boost_ttl;
}

void TickerUniverse::cache_meta(const Ticker& ticker, const TickerMeta& meta) {
    std::unique_lock lock(rw_mtx_);
    meta_cache_[ticker] = meta;
}

std::optional<TickerMeta> TickerUniverse::meta(const Ticker& ticker) const {
    std::shared_lock lock(rw_mtx_);
    auto it = meta_cache_.find(ticker);
    if (it == meta_cache_.end()) return std::nullopt;
    return it->second;
}

double TickerUniverse::volume_scale_factor(const Ticker& ticker) const {
    std::shared_lock lock(rw_mtx_);
    return volume_scale_factor_locked(ticker);
}

double TickerUniverse::volume_scale_factor_locked(const Ticker& ticker,
                                                 const absl::flat_hash_map<Ticker, TickerStats>& temp_stats) const {
    if (!cfg_.dynamic_thresholds_enabled) return 1.0;
    
    // B13-FIX: Validate reference_volume BEFORE any computation
    const double ref = cfg_.dynamic_thresholds_reference_volume;
    if (ref <= 0.0) {
        LOG_ERROR("TickerUniverse: dynamic_thresholds_reference_volume={} is invalid", ref);
        return 1.0;
    }

    // Check cache first.
    auto it = scale_factors_.find(ticker);
    if (it != scale_factors_.end()) return it->second;

    // Compute on-the-fly from stats.
    std::optional<TickerStats> s;
    auto stats_it = temp_stats.find(ticker);
    if (stats_it != temp_stats.end()) {
        s = stats_it->second;
    } else if (stats_lookup_) {
        s = stats_lookup_(ticker);
    }
    
    if (!s || s->volume_24h_usd <= 0.0) return 1.0;

    double raw = s->volume_24h_usd / ref;
    // Compress with sqrt so extremes don't dominate, then clamp.
    double compressed = std::sqrt(raw);
    return std::clamp(compressed,
                      cfg_.dynamic_thresholds_min_scale,
                      cfg_.dynamic_thresholds_max_scale);
}

double TickerUniverse::calibrated_min_size_usd(const Ticker& ticker,
                                                double config_default) const {
    std::shared_lock lock(rw_mtx_);
    return calibrated_min_size_usd_locked(ticker, config_default);
}

double TickerUniverse::calibrated_min_size_usd_locked(const Ticker& ticker,
                                                      double config_default) const {
    // Per-coin dynamic scale (volume-based).
    double scale = volume_scale_factor_locked(ticker);
    // Exchange-wide multiplier (e.g. MEXC 0.1x).
    double calibrated = config_default * scale * cfg_.exchange_volume_multiplier;
    // Never go below configurable floor ratio of config default.
    return std::max(calibrated, config_default * cfg_.dynamic_thresholds_floor_ratio);
}

void TickerUniverse::refresh_scale_factors() {
    std::unique_lock lock(rw_mtx_);
    refresh_scale_factors_locked();
}

void TickerUniverse::refresh_scale_factors_locked(const absl::flat_hash_map<Ticker, TickerStats>& temp_stats) {
    scale_factors_.clear();
    if (!cfg_.dynamic_thresholds_enabled) return;

    for (const auto& ticker : active_) {
        double sf = volume_scale_factor_locked(ticker, temp_stats);
        scale_factors_[ticker] = sf;
    }

    // Also cache for screener-approved coins not yet in pool.
    for (const auto& ticker : screener_approved_) {
        if (!scale_factors_.contains(ticker)) {
            scale_factors_[ticker] = volume_scale_factor_locked(ticker, temp_stats);
        }
    }
}

int TickerUniverse::get_tier(const Ticker& ticker) const {
    std::shared_lock lock(rw_mtx_);
    return get_tier_locked(ticker);
}

int TickerUniverse::get_tier_locked(const Ticker& ticker) const {
    if (!cfg_.tier_thresholds_enabled) return 0;
    if (cfg_.tier_volume_boundaries.empty()) return 0;

    std::optional<TickerStats> s;
    if (stats_lookup_) s = stats_lookup_(ticker);
    if (!s || s->volume_24h_usd <= 0.0) return 0;

    const double vol = s->volume_24h_usd;
    
    // Tier 0: volume >= boundaries[0] (highest volume, e.g. >$1B)
    // Tier 1: boundaries[1] <= volume < boundaries[0] (mid volume, e.g. $100M-$1B)
    // Tier 2: volume < boundaries[1] (lowest volume, e.g. <$100M)
    for (std::size_t i = 0; i < cfg_.tier_volume_boundaries.size(); ++i) {
        if (vol >= cfg_.tier_volume_boundaries[i]) {
            return static_cast<int>(i);
        }
    }
    
    // If volume is below all boundaries, return last tier
    return static_cast<int>(cfg_.tier_volume_boundaries.size());
}

double TickerUniverse::get_tiered_threshold(const Ticker& ticker,
                                             const std::vector<double>& tier_values) const {
    std::shared_lock lock(rw_mtx_);
    if (!cfg_.tier_thresholds_enabled || tier_values.empty()) {
        return tier_values.empty() ? 0.0 : tier_values[0];
    }

    const int tier = get_tier_locked(ticker);
    const std::size_t idx = std::min(static_cast<std::size_t>(tier), tier_values.size() - 1);
    return tier_values[idx];
}

}  // namespace trade_bot

