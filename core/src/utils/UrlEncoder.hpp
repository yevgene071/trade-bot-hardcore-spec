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
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (auto c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(static_cast<unsigned char>(c)) << std::nouppercase;
        }
    }

    return escaped.str();
}

} // namespace trade_bot
