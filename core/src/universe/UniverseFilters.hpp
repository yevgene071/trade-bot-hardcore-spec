#pragma once

#include "domain/Types.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace trade_bot {

/**
 * Static (config-driven) ticker filter. Applies, in order:
 *   1. manual_deny → reject (highest priority)
 *   2. manual_allow → accept
 *   3. any deny_pattern matches → reject
 *   4. any allow_pattern matches → accept
 *   5. (no allow_pattern given) → accept
 *
 * Patterns support a single wildcard form: `*` matches any substring.
 * Examples:  `*USDT`   matches `BTCUSDT`
 *            `*UP*`    matches `BTCUPUSDT`
 *            `1000*`   matches `1000FLOKIUSDT`
 */
class UniverseFilters {
public:
    struct Config {
        std::vector<std::string> allow_patterns;
        std::vector<std::string> deny_patterns;
        std::vector<Ticker>      manual_allow;
        std::vector<Ticker>      manual_deny;
    };

    UniverseFilters();
    explicit UniverseFilters(Config cfg);

    /// True if `ticker` passes the static filter.
    bool accepts(const Ticker& ticker) const;

    /// Glob match supporting `*` (any-substring). Case-sensitive.
    static bool glob_match(std::string_view pattern, std::string_view str);

private:
    Config           cfg_;
    std::set<Ticker> manual_allow_;
    std::set<Ticker> manual_deny_;
};

}  // namespace trade_bot
