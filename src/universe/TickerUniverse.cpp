#include "TickerUniverse.hpp"

#include "logger/Logger.hpp"

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

bool TickerUniverse::passes_filter_(const Ticker& ticker, double& out_volume) const {
    out_volume = 0.0;
    if (!filters_.accepts(ticker)) return false;

    std::optional<TickerStats> s;
    if (stats_lookup_) s = stats_lookup_(ticker);

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

void TickerUniverse::refresh_pool(const std::vector<Ticker>& available) {
    std::vector<std::pair<Ticker, double>> survivors;
    survivors.reserve(available.size());

    for (const auto& t : available) {
        double vol = 0.0;
        if (passes_filter_(t, vol)) {
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

    // Recompute per-coin scale factors for dynamic thresholds.
    refresh_scale_factors();
}

void TickerUniverse::refresh_affinity() {
    for (const auto& t : active_) {
        auto& tickerMap = affinity_[t];
        for (const auto& [name, fn] : strategies_) {
            const bool enabled = fn ? fn(t) : false;
            const auto it = tickerMap.find(name);
            const bool prev = (it != tickerMap.end()) ? it->second : false;
            if (it == tickerMap.end() || prev != enabled) {
                tickerMap[name] = enabled;
                if (on_change_ && (it == tickerMap.end() ? enabled : true)) {
                    on_change_(t, name, enabled);
                }
            }
        }
    }
}

const std::vector<Ticker>& TickerUniverse::active() const { return active_; }
std::size_t TickerUniverse::pool_size() const noexcept { return active_.size(); }

bool TickerUniverse::is_strategy_enabled(const Ticker& ticker,
                                         const std::string& strategy) const {
    auto it = affinity_.find(ticker);
    if (it == affinity_.end()) return false;
    auto sit = it->second.find(strategy);
    return sit != it->second.end() && sit->second;
}

std::vector<std::string> TickerUniverse::enabled_strategies(const Ticker& ticker) const {
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
    on_big_event(ticker, now);
    refresh_affinity();
}

void TickerUniverse::on_big_amount(const Ticker& ticker, double /*size_usd*/,
                                   std::chrono::system_clock::time_point now) {
    on_big_event(ticker, now);
    refresh_affinity();
}

void TickerUniverse::seed_candidates(const std::vector<Ticker>& candidates) {
    all_candidates_ = candidates;
}

void TickerUniverse::on_screener_new_coin(const Ticker& ticker) {
    if (!filters_.accepts(ticker)) return;
    if (screener_approved_.insert(ticker).second) {
        LOG_INFO("[Universe] Screener: {} approved, rebuilding pool ({} screener coins)",
                 ticker, screener_approved_.size());
        // Rebuild pool from all static-filtered candidates, gated by
        // screener_approved_ only (no manual_allow — the screener IS the
        // sole authority on which coins to trade).
        std::vector<Ticker> pool_candidates;
        pool_candidates.reserve(all_candidates_.size());
        for (const auto& t : all_candidates_) {
            if (screener_approved_.count(t)) {
                pool_candidates.push_back(t);
            }
        }
        // Also include any screener coins that weren't in the initial
        // all_candidates_ fetch (e.g. just listed on exchange).
        for (const auto& t : screener_approved_) {
            if (std::find(pool_candidates.begin(), pool_candidates.end(), t) == pool_candidates.end()) {
                pool_candidates.push_back(t);
            }
        }
        refresh_pool(pool_candidates);
        refresh_affinity();
        // refresh_scale_factors() already called inside refresh_pool().
    }
}

void TickerUniverse::on_big_event(const Ticker& ticker,
                                  std::chrono::system_clock::time_point now) {
    boosts_[ticker] = now;
}

void TickerUniverse::update_orderbook_settings(const OrderbookSettings& settings) {
    large_amounts_[settings.ticker] = settings.large_amount_usd;
}

double TickerUniverse::density_min_size_usd(const Ticker& ticker,
                                            double config_default) const {
    // Base calibrated value (volume scale factor + exchange multiplier + floor).
    double base = calibrated_min_size_usd(ticker, config_default);
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
    auto it = boosts_.find(ticker);
    if (it == boosts_.end()) return false;
    return (now - it->second) <= cfg_.boost_ttl;
}

void TickerUniverse::cache_meta(const Ticker& ticker, const TickerMeta& meta) {
    meta_cache_[ticker] = meta;
}

std::optional<TickerMeta> TickerUniverse::meta(const Ticker& ticker) const {
    auto it = meta_cache_.find(ticker);
    if (it == meta_cache_.end()) return std::nullopt;
    return it->second;
}

double TickerUniverse::volume_scale_factor(const Ticker& ticker) const {
    if (!cfg_.dynamic_thresholds_enabled) return 1.0;

    // Check cache first.
    auto it = scale_factors_.find(ticker);
    if (it != scale_factors_.end()) return it->second;

    // Compute on-the-fly from stats.
    std::optional<TickerStats> s;
    if (stats_lookup_) s = stats_lookup_(ticker);
    if (!s || s->volume_24h_usd <= 0.0) return 1.0;

    const double ref = cfg_.dynamic_thresholds_reference_volume;
    if (ref <= 0.0) return 1.0;

    double raw = s->volume_24h_usd / ref;
    // Compress with sqrt so extremes don't dominate, then clamp.
    double compressed = std::sqrt(raw);
    return std::clamp(compressed,
                      cfg_.dynamic_thresholds_min_scale,
                      cfg_.dynamic_thresholds_max_scale);
}

double TickerUniverse::calibrated_min_size_usd(const Ticker& ticker,
                                                double config_default) const {
    // Per-coin dynamic scale (volume-based).
    double scale = volume_scale_factor(ticker);
    // Exchange-wide multiplier (e.g. MEXC 0.1x).
    double calibrated = config_default * scale * cfg_.exchange_volume_multiplier;
    // Never go below configurable floor ratio of config default.
    return std::max(calibrated, config_default * cfg_.dynamic_thresholds_floor_ratio);
}

void TickerUniverse::refresh_scale_factors() {
    scale_factors_.clear();
    if (!cfg_.dynamic_thresholds_enabled) return;

    for (const auto& ticker : active_) {
        double sf = volume_scale_factor(ticker);
        scale_factors_[ticker] = sf;
    }

    // Also cache for screener-approved coins not yet in pool.
    for (const auto& ticker : screener_approved_) {
        if (!scale_factors_.contains(ticker)) {
            scale_factors_[ticker] = volume_scale_factor(ticker);
        }
    }
}

}  // namespace trade_bot
