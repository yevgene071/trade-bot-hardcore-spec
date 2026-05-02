#include "SntpClient.hpp"

#include "logger/Logger.hpp"

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>

namespace trade_bot {

namespace {

// SNTP packet is 48 bytes. We use only the transmit timestamp (offset 40,
// 8 bytes: 4 byte seconds since 1900-01-01 + 4 byte fractional seconds).
constexpr size_t kSntpPacketSize = 48;
constexpr int    kNtpPort        = 123;
constexpr int64_t kSecondsBetween1900And1970 = 2'208'988'800LL;

}  // namespace

std::optional<std::chrono::system_clock::time_point>
SntpClient::query(const std::string& host, int timeout_ms) {
    namespace asio = boost::asio;
    using udp = asio::ip::udp;

    try {
        asio::io_context ioc;
        udp::resolver resolver(ioc);
        const auto endpoints = resolver.resolve(udp::v4(), host, std::to_string(kNtpPort));
        if (endpoints.empty()) {
            return std::nullopt;
        }
        udp::socket socket(ioc);
        socket.open(udp::v4());

        // Build minimal SNTPv4 request: LI=0, VN=4, Mode=3 (client) → 0x23.
        std::array<std::uint8_t, kSntpPacketSize> request{};
        request[0] = 0x23;

        socket.send_to(asio::buffer(request), *endpoints.begin());

        // Receive with timeout.
        std::array<std::uint8_t, kSntpPacketSize> response{};
        udp::endpoint remote;

        std::optional<std::error_code> ec_opt;
        std::size_t bytes = 0;
        socket.async_receive_from(asio::buffer(response), remote,
                                  [&](const boost::system::error_code& ec, std::size_t n) {
                                      ec_opt = ec;
                                      bytes  = n;
                                  });

        ioc.run_for(std::chrono::milliseconds(timeout_ms));
        if (!ec_opt.has_value()) {
            socket.cancel();
            return std::nullopt;  // timeout
        }
        if (*ec_opt || bytes < kSntpPacketSize) {
            return std::nullopt;
        }

        // Transmit timestamp at offset 40, big-endian.
        std::uint32_t seconds_be = 0;
        std::uint32_t fraction_be = 0;
        std::memcpy(&seconds_be, response.data() + 40, 4);
        std::memcpy(&fraction_be, response.data() + 44, 4);

        const auto seconds_1900 = static_cast<std::uint32_t>(
            (seconds_be << 24) | ((seconds_be << 8) & 0x00FF0000U) |
            ((seconds_be >> 8) & 0x0000FF00U) | (seconds_be >> 24));
        const auto fraction = static_cast<std::uint32_t>(
            (fraction_be << 24) | ((fraction_be << 8) & 0x00FF0000U) |
            ((fraction_be >> 8) & 0x0000FF00U) | (fraction_be >> 24));

        const int64_t epoch_seconds = static_cast<int64_t>(seconds_1900) - kSecondsBetween1900And1970;
        const int64_t fraction_ns =
            static_cast<int64_t>((static_cast<double>(fraction) / 4'294'967'296.0) * 1e9);

        return std::chrono::system_clock::time_point{
            std::chrono::seconds{epoch_seconds} + std::chrono::nanoseconds{fraction_ns}};
    } catch (const std::exception& ex) {
        LOG_WARN("SntpClient::query({}) failed: {}", host, ex.what());
        return std::nullopt;
    }
}

}  // namespace trade_bot
