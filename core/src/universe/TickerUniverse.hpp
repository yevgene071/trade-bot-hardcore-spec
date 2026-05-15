#pragma once

#include "UniverseFilters.hpp"
#include "domain/Types.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
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
        return stats_lookup_ ? stats_lookup_(ticker) : std::nullopt;
    }
    void set_affinity_change_handler(AffinityChange fn);
    void register_strategy(const std::string& name, AffinityScore fn);

    /// Rebuild the pool from `available` tickers. After this call `active()`
    /// reflects the new pool (manual rules + globs + stats gate, top-N).
    void refresh_pool(const std::vector<Ticker>& available);

    /// Recompute strategy enablement per (ticker, strategy). Fires
    /// AffinityChange callbacks for transitions.
    void refresh_affinity();

    /// Currently active pool, ordered by descending 24h volume.
    const std::vector<Ticker>& active() const;
    std::size_t              pool_size() const noexcept;

    bool is_strategy_enabled(const Ticker& ticker, const std::string& strategy) const;
    std::vector<std::string> enabled_strategies(const Ticker& ticker) const;

    void on_big_tick(const Ticker& ticker, double size_usd, std::chrono::system_clock::time_point now);
    void on_big_amount(const Ticker& ticker, double size_usd, std::chrono::system_clock::time_point now);
    void on_screener_new_coin(const Ticker& ticker);

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

private:
    bool passes_filter_(const Ticker& ticker, double& out_volume) const;

    Config                                         cfg_;
    UniverseFilters                                filters_;
    StatsLookup                                    stats_lookup_;
    AffinityChange                                 on_change_;
    std::unordered_map<std::string, AffinityScore> strategies_;

    std::vector<Ticker>                            active_;          // ordered by volume desc
    std::map<Ticker, std::map<std::string, bool>>  affinity_;        // ticker → {strategy → enabled}
    std::unordered_map<Ticker, std::chrono::system_clock::time_point> boosts_;
    std::unordered_map<Ticker, double>             large_amounts_;
    std::unordered_map<Ticker, TickerMeta>         meta_cache_;

    std::set<Ticker>                               screener_approved_;  // tickers from MetaScalp screener
    std::vector<Ticker>                            all_candidates_;     // full static-filtered list

    // Per-coin cached scale factors (computed from 24h volume stats).
    std::unordered_map<Ticker, double>             scale_factors_;
};

}  // namespace trade_bot
