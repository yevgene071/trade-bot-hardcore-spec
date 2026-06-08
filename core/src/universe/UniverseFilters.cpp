#include "UniverseFilters.hpp"
#include "utils/TickerSymbol.hpp"

#include <algorithm>

namespace trade_bot {

UniverseFilters::UniverseFilters() : UniverseFilters(Config{}) {}

UniverseFilters::UniverseFilters(Config cfg) : cfg_(std::move(cfg)) {
    std::transform(cfg_.manual_allow.begin(), cfg_.manual_allow.end(),
                   std::inserter(manual_allow_, manual_allow_.end()),
                   [](const auto& t) { return to_internal_ticker(t); });
    std::transform(cfg_.manual_deny.begin(), cfg_.manual_deny.end(),
                   std::inserter(manual_deny_, manual_deny_.end()),
                   [](const auto& t) { return to_internal_ticker(t); });
}

bool UniverseFilters::accepts(const Ticker& ticker) const {
    const Ticker key = to_internal_ticker(ticker);
    const std::string wire = to_metascalp_symbol(ticker);

    if (manual_deny_.count(key))  return false;
    if (manual_allow_.count(key)) return true;

    auto matches = [&](const auto& pattern) {
        return glob_match(pattern, key) || glob_match(pattern, wire);
    };

    auto deny_it = std::any_of(cfg_.deny_patterns.begin(), cfg_.deny_patterns.end(), matches);
    if (deny_it) return false;

    if (cfg_.allow_patterns.empty()) return true;

    return std::any_of(cfg_.allow_patterns.begin(), cfg_.allow_patterns.end(), matches);
}

// Iterative segment-by-segment glob matcher. O(n*m) worst case, plenty fast
// for the small ticker counts involved here.
bool UniverseFilters::glob_match(std::string_view pattern, std::string_view str) {
    std::size_t pi = 0, si = 0;
    std::size_t star_p = std::string_view::npos;
    std::size_t star_s = 0;

    while (si < str.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            star_p = pi++;
            star_s = si;
        } else if (pi < pattern.size() && pattern[pi] == str[si]) {
            ++pi;
            ++si;
        } else if (star_p != std::string_view::npos) {
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

}  // namespace trade_bot
