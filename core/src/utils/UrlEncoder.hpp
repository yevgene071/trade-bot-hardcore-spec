#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace trade_bot {

/**
 * Simple URL encoder for query parameters.
 */
inline std::string url_encode(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() * 3);
    static const char hex_chars[] = "0123456789ABCDEF";

    for (auto c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped.push_back(c);
        } else {
            escaped.push_back('%');
            escaped.push_back(hex_chars[uc >> 4]);
            escaped.push_back(hex_chars[uc & 0x0f]);
        }
    }
    return escaped;
}

} // namespace trade_bot
