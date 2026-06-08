#pragma once

#include "domain/Types.hpp"

#include <cctype>
#include <string>
#include <string_view>

namespace trade_bot {

inline std::string to_metascalp_symbol(std::string_view ticker) {
    std::string out;
    out.reserve(ticker.size());
    for (const char c : ticker) {
        if (c == '_' || c == '-' || c == '/') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

inline Ticker to_internal_ticker(std::string_view raw) {
    std::string normalized = to_metascalp_symbol(raw);
    for (const char* quote : {"USDT", "USDC", "BTC", "ETH", "BNB", "BUSD"}) {
        const std::string_view q{quote};
        if (normalized.size() > q.size() &&
            normalized.compare(normalized.size() - q.size(), q.size(), q) == 0) {
            return normalized.substr(0, normalized.size() - q.size()) + "_" + std::string(q);
        }
    }
    return normalized;
}

} // namespace trade_bot
