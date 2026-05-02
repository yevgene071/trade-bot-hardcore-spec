#include "UniverseFilters.hpp"

#include <algorithm>

namespace trade_bot {

UniverseFilters::UniverseFilters() : UniverseFilters(Config{}) {}

UniverseFilters::UniverseFilters(Config cfg) : cfg_(std::move(cfg)) {
    manual_allow_.insert(cfg_.manual_allow.begin(), cfg_.manual_allow.end());
    manual_deny_.insert(cfg_.manual_deny.begin(),   cfg_.manual_deny.end());
}

bool UniverseFilters::accepts(const Ticker& ticker) const {
    if (manual_deny_.count(ticker))  return false;
    if (manual_allow_.count(ticker)) return true;

    for (const auto& p : cfg_.deny_patterns) {
        if (glob_match(p, ticker)) return false;
    }
    if (cfg_.allow_patterns.empty()) return true;
    for (const auto& p : cfg_.allow_patterns) {
        if (glob_match(p, ticker)) return true;
    }
    return false;
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
