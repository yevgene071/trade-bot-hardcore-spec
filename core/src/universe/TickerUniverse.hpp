#pragma once

#include "UniverseFilters.hpp"
#include "domain/Types.hpp"
#include "utils/TickerSymbol.hpp"

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

namespace trade_bot {

/// Per-ticker dynamic stats fed from live market data into the affinity filter.
/// Tests inject a pure function via set_stats_lookup().
struct TickerStats {
    double volume_24h_usd{0.0};
    double avg_spread_bps{0.0};
    double funding_rate_bps{0.0};    // perpetual funding rate in bps/8h; gating avoids crowded trades
    double volatility_1min_bps{0.0}; // short-term price vol from FeatureFrame; strategy-specific bounds
};

/// Cached precision info from `GET /api/connections/{id}/tickers`. Used by
/// downstream modules (Executor, R8) but cached here at pool-build time.
struct TickerMeta {
    double price_increment{0.0};
    double size_increment{0.0};
    double min_size{0.0};
    double max_size{0.0};
};

/**
 * T1-UNIVERSE: pool of tickers + per-strategy affinity layer.
 *
 * Pool layer  : caller feeds available tickers (e.g. via OrderGateway), the
 *               filter chain (manual deny → static glob patterns →
 *               dynamic stats gate, with screener-approved bypass)
 *               selects up to `max_pool_size`, ordered by 24h volume.
 *
 * Affinity layer: for each ticker in the pool and each registered strategy,
 *               an affinity score function decides whether the strategy is
 *               "enabled" for that ticker. Affinity changes are surfaced via
 *               an optional callback so downstream components can respond
 *               (subscribe / unsubscribe market data, etc.).
 *
 * Boost events  : `on_big_event(ticker)` marks the ticker as boosted for
 *               `boost_ttl` (e.g. for BigTick / BigOrderBookAmount). Per
 *               spec the boost itself does NOT immediately change the
 *               affinity state — it only takes effect on the next
 *               refresh_affinity() call.
 */
class TickerUniverse {
public:
    using StatsLookup    = std::function<std::optional<TickerStats>(const Ticker&)>;
    using AffinityScore  = std::function<bool(const Ticker&)>;
    using AffinityChange = std::function<void(const Ticker&,
                                              const std::string& strategy,
                                              bool enabled)>;

    struct Config {
        UniverseFilters::Config  filters;
        std::size_t              max_pool_size{30};
        double                   min_volume_24h_usd{1'000'000.0};
        double                   max_avg_spread_bps{20.0};
        std::chrono::seconds     boost_ttl{300};
        /// Multiplier applied to min_volume_24h_usd based on exchange.
        /// MEXC (~0.1x Binance volume) → 0.1; Binance → 1.0.
        double                   exchange_volume_multiplier{1.0};

        // ── Per-coin dynamic threshold scaling ────────────────
        /// When true, signal-detector thresholds are scaled per-coin
        /// based on 24h volume relative to the reference.
        bool   dynamic_thresholds_enabled{true};
        /// Reference 24h volume (USD) — the "standard" coin volume.
        /// Coins with higher volume get scaled up; lower → scaled down.
        double dynamic_thresholds_reference_volume{500'000'000.0};  // 500M
        /// Floor / ceiling for the per-coin scale factor.
        double dynamic_thresholds_min_scale{0.15};
        double dynamic_thresholds_max_scale{5.0};
        /// Absolute floor for calibrated_min_size_usd as a ratio of config_default.
        /// Prevents thresholds from collapsing below 10% of their original value
        /// even for the smallest coins on the lowest-volume exchanges.
        double dynamic_thresholds_floor_ratio{0.10};

        // ── Tier-based threshold scaling (Phase 0-4) ──────────
        /// When true, strategy thresholds are scaled by volume tier.
        /// Simpler alternative to sqrt-based scaling for initial phases.
        bool   tier_thresholds_enabled{false};
        /// Volume tier boundaries (USD). Example: [1B, 100M, 0]
        /// Tier 0: volume >= tiers[0] (e.g. >$1B: BTC, ETH)
        /// Tier 1: tiers[1] <= volume < tiers[0] (e.g. $100M-$1B: SOL, AVAX)
        /// Tier 2: volume < tiers[1] (e.g. <$100M: small caps)
        std::vector<double> tier_volume_boundaries{1'000'000'000.0, 100'000'000.0, 0.0};
    };

    TickerUniverse();
    explicit TickerUniverse(Config cfg);

    void update_config(Config cfg) {
        cfg_ = std::move(cfg);
        filters_ = UniverseFilters(cfg_.filters);
    }

    void set_stats_lookup(StatsLookup fn);
    /// Lookup stats for a ticker using the configured stats provider.
    std::optional<TickerStats> get_stats(const Ticker& ticker) const {
        return stats_lookup_ ? stats_lookup_(to_internal_ticker(ticker)) : std::nullopt;
    }
    void set_affinity_change_handler(AffinityChange fn);
    void register_strategy(const std::string& name, AffinityScore fn);

    /// Rebuild the pool from `available` tickers. After this call `active()`
    /// reflects the new pool (manual rules + globs + stats gate, top-N).
    void refresh_pool(const std::vector<Ticker>& available, std::chrono::system_clock::time_point now);

    /// Recompute strategy enablement per (ticker, strategy). Fires
    /// AffinityChange callbacks for transitions.
    void refresh_affinity();

    /// Currently active pool, ordered by descending 24h volume.
    const std::vector<Ticker>& active() const;
    std::size_t              pool_size() const noexcept;

    bool is_strategy_enabled(const Ticker& ticker, const std::string& strategy) const;
    bool is_in_pool(const Ticker& ticker) const;
    // GAP-10: spec alias for is_strategy_enabled
    bool is_tradable_by(const Ticker& ticker, const std::string& strategy) const {
        return is_strategy_enabled(ticker, strategy);
    }
    std::vector<std::string> enabled_strategies(const Ticker& ticker) const;

    /// Returns true if the ticker has been continuously in the active pool
    /// for at least min_stable (FEAT-06 §0.7 affinity_stable_min).
    bool is_affinity_stable(const Ticker& ticker,
                            std::chrono::seconds min_stable,
                            std::chrono::system_clock::time_point now) const {
        auto it = active_since_.find(to_internal_ticker(ticker));
        if (it == active_since_.end()) return false;
        return (now - it->second) >= min_stable;
    }

    void on_big_tick(const Ticker& ticker, double size_usd, std::chrono::system_clock::time_point now);
    void on_big_amount(const Ticker& ticker, double size_usd, std::chrono::system_clock::time_point now);
    void on_screener_new_coin(const Ticker& ticker, std::chrono::system_clock::time_point now);

    void on_big_event(const Ticker& ticker, std::chrono::system_clock::time_point now);
    bool is_boosted(const Ticker& ticker,
                    std::chrono::system_clock::time_point now) const;

    void update_orderbook_settings(const OrderbookSettings& settings);
    double density_min_size_usd(const Ticker& ticker, double config_default) const;

    void cache_meta(const Ticker& ticker, const TickerMeta& meta);
    std::optional<TickerMeta> meta(const Ticker& ticker) const;

    /// Seed the full list of static-filtered candidates (called once at startup).
    /// These are the candidates `on_screener_new_coin` will draw from.
    void seed_candidates(const std::vector<Ticker>& candidates);

    // ── Per-coin dynamic threshold helpers ─────────────────

    /// Returns a per-coin scale factor ∈ [cfg_.dynamic_thresholds_min_scale,
    /// cfg_.dynamic_thresholds_max_scale] computed from the coin's 24h volume
    /// relative to cfg_.dynamic_thresholds_reference_volume.
    /// Returns 1.0 when dynamic thresholds are disabled or stats unavailable.
    [[nodiscard]] double volume_scale_factor(const Ticker& ticker) const;

    /// Convenience: calibrated min-size-usd for a detector, applying both
    /// the per-coin scale factor AND the exchange volume multiplier.
    [[nodiscard]] double calibrated_min_size_usd(const Ticker& ticker,
                                                  double config_default) const;

    /// Force-refresh scale factor cache from current stats (call after pool rebuild).
    void refresh_scale_factors();

    /// Manually override a strategy's affinity for a ticker (e.g. from dashboard commands).
    /// Fires the AffinityChange callback if the value changed.
    void override_affinity(const Ticker& ticker, const std::string& strategy, bool enabled);

    // ── Tier-based threshold helpers ───────────────────────

    /// Returns the volume tier (0, 1, 2, ...) for a ticker based on its 24h volume.
    /// Tier 0 = highest volume (>= tier_volume_boundaries[0])
    /// Tier N = lowest volume (< tier_volume_boundaries[N-1])
    /// Returns 0 if tier system is disabled or stats unavailable.
    [[nodiscard]] int get_tier(const Ticker& ticker) const;

    /// Returns the threshold value for a given parameter and ticker, scaled by tier.
    /// `param_name` must match a key in the strategy's tier config (e.g. "min_approach_speed_bps_1s").
    /// `tier_values` is the array of values per tier from config (e.g. [50.0, 15.0, 8.0]).
    /// Returns tier_values[tier] or tier_values.back() if tier >= tier_values.size().
    [[nodiscard]] double get_tiered_threshold(const Ticker& ticker,
                                               const std::vector<double>& tier_values) const;

private:
    bool passes_filter_locked(const Ticker& ticker, double& out_volume, const absl::flat_hash_map<Ticker, TickerStats>& temp_stats = {}) const;
    double volume_scale_factor_locked(const Ticker& ticker, const absl::flat_hash_map<Ticker, TickerStats>& temp_stats = {}) const;
    double calibrated_min_size_usd_locked(const Ticker& ticker, double config_default) const;
    int get_tier_locked(const Ticker& ticker) const;
    void refresh_pool_locked(const std::vector<Ticker>& available, std::chrono::system_clock::time_point now, const absl::flat_hash_map<Ticker, TickerStats>& temp_stats = {});
    // Collect affinity transitions while write lock is held (does NOT call on_change_).
    // Callers must fire the returned changes AFTER releasing rw_mtx_ to avoid re-entrant
    // locking (on_change_ callbacks call back into TickerUniverse → shared_lock → EDEADLK).
    using PendingChanges = std::vector<std::tuple<Ticker, std::string, bool>>;
    PendingChanges collect_affinity_changes_();
    void fire_affinity_changes_(PendingChanges& changes);
    void refresh_scale_factors_locked(const absl::flat_hash_map<Ticker, TickerStats>& temp_stats = {});
    void on_big_event_locked(const Ticker& ticker, std::chrono::system_clock::time_point now);

    Config                                         cfg_;
    UniverseFilters                                filters_;
    StatsLookup                                    stats_lookup_;
    AffinityChange                                 on_change_;
    absl::btree_map<std::string, AffinityScore>    strategies_;

    std::vector<Ticker>                            active_;          // ordered by volume desc
    std::map<Ticker, std::map<std::string, bool>>  affinity_;        // ticker → {strategy → enabled}
    absl::btree_map<Ticker, std::chrono::system_clock::time_point> boosts_;
    absl::btree_map<Ticker, std::chrono::system_clock::time_point> active_since_; // FEAT-06
    absl::btree_map<Ticker, double>                large_amounts_;
    absl::btree_map<Ticker, TickerMeta>            meta_cache_;

    std::set<Ticker>                               screener_approved_;  // tickers from MetaScalp screener
    std::vector<Ticker>                            all_candidates_;     // full static-filtered list

    mutable std::shared_mutex rw_mtx_;

    // Per-coin cached scale factors (computed from 24h volume stats).
    absl::btree_map<Ticker, double>                scale_factors_;
};

}  // namespace trade_bot
