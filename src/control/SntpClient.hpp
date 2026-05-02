#pragma once

#include "INtpClient.hpp"

namespace trade_bot {

/// Minimal SNTPv4 client over UDP/123 using boost::asio.
/// Sends a single client-mode request and returns the server-transmitted
/// timestamp converted to system_clock::time_point.
class SntpClient : public INtpClient {
public:
    SntpClient() = default;

    std::optional<std::chrono::system_clock::time_point>
    query(const std::string& host, int timeout_ms) override;
};

}  // namespace trade_bot
