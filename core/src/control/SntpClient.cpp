#include "SntpClient.hpp"

#include "logger/Logger.hpp"

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

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

        const auto t1 = std::chrono::system_clock::now();
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
        const auto t4 = std::chrono::system_clock::now();
        if (!ec_opt.has_value()) {
            socket.cancel();
            return std::nullopt;  // timeout
        }
        if (*ec_opt || bytes < kSntpPacketSize) {
            return std::nullopt;
        }

        // Transmit timestamp at offset 40, big-endian. ntohl() correctly
        // handles both little- and big-endian hosts (manual shifts were wrong
        // on big-endian machines).
        std::uint32_t seconds_raw = 0;
        std::uint32_t fraction_raw = 0;
        std::memcpy(&seconds_raw, response.data() + 40, 4);
        std::memcpy(&fraction_raw, response.data() + 44, 4);

        const std::uint32_t seconds_host = ntohl(seconds_raw);
        const std::uint32_t fraction     = ntohl(fraction_raw);

        // Y2036 fix: NTP era 0 (bit 31 set) covers 1900–2036;
        // era 1 (bit 31 clear) covers 2036–2104. Add 2^32 for era 1.
        std::uint64_t seconds_1900;
        if (seconds_host & 0x80000000U) {
            seconds_1900 = seconds_host;  // era 0
        } else {
            seconds_1900 = static_cast<std::uint64_t>(seconds_host) + 0x100000000ULL;  // era 1
        }

        const int64_t epoch_seconds = static_cast<int64_t>(seconds_1900) - kSecondsBetween1900And1970;
        const int64_t fraction_ns =
            static_cast<int64_t>((static_cast<double>(fraction) / 4'294'967'296.0) * 1e9);

        const auto server_time = std::chrono::system_clock::time_point{
            std::chrono::seconds{epoch_seconds} + std::chrono::nanoseconds{fraction_ns}};

        // RTT correction: server transmit timestamp is ~RTT/2 old by the time
        // we receive it. Add RTT/2 so the caller's (local − server) difference
        // is the true clock offset rather than offset + RTT/2.
        return server_time + (t4 - t1) / 2;
    } catch (const std::exception& ex) {
        LOG_WARN("SntpClient::query({}) failed: {}", host, ex.what());
        return std::nullopt;
    }
}

}  // namespace trade_bot
