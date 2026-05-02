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

/// Per-ticker dynamic stats fed from TradeStream / OrderBook into the
/// dynamic filter (volume gate + spread gate). Tests inject a pure function.
struct TickerStats {
    double volume_24h_usd{0.0};
    double avg_spread_bps{0.0};
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
 *               filter chain (manual deny → manual allow → static glob
 *               patterns → dynamic stats gate) selects up to `max_pool_size`,
 *               ordered by 24h volume.
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
    };

    TickerUniverse();
    explicit TickerUniverse(Config cfg);

    void set_stats_lookup(StatsLookup fn);
    void set_affinity_change_handler(AffinityChange fn);
    void register_strategy(const std::string& name, AffinityScore fn);

    /// Rebuild the pool from `available` tickers. After this call `active()`
    /// reflects the new pool (manual rules + globs + stats gate, top-N).
    void refresh_pool(const std::vector<Ticker>& available);

    /// Recompute strategy enablement per (ticker, strategy). Fires
    /// AffinityChange callbacks for transitions.
    void refresh_affinity();

    /// Currently active pool, ordered by descending 24h volume.
    std::vector<Ticker>      active() const;
    std::size_t              pool_size() const noexcept;

    bool is_strategy_enabled(const Ticker& ticker, const std::string& strategy) const;
    std::vector<std::string> enabled_strategies(const Ticker& ticker) const;

    void on_big_tick(const Ticker& ticker, double size_usd, std::chrono::system_clock::time_point now);
    void on_big_amount(const Ticker& ticker, double size_usd, std::chrono::system_clock::time_point now);
    void on_screener_new_coin(const Ticker& ticker);

    void on_big_event(const Ticker& ticker, std::chrono::system_clock::time_point now);
    bool is_boosted(const Ticker& ticker,
                    std::chrono::system_clock::time_point now) const;

    void cache_meta(const Ticker& ticker, const TickerMeta& meta);
    std::optional<TickerMeta> meta(const Ticker& ticker) const;

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
    std::unordered_map<Ticker, TickerMeta>         meta_cache_;
};

}  // namespace trade_bot
