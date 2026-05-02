#include <gtest/gtest.h>
#include "control/ClockDriftMonitor.hpp"
#include "control/KillSwitch.hpp"
#include "logger/Logger.hpp"
#include <boost/asio.hpp>
#include <thread>
#include <chrono>

using namespace trade_bot;
using boost::asio::ip::udp;

class MockNtpServer {
public:
    explicit MockNtpServer(uint16_t port, int64_t offset_ms = 0)
        : socket_(io_context_, udp::endpoint(udp::v4(), port)), offset_ms_(offset_ms) {}

    void start() {
        running_ = true;
        thread_ = std::thread([this]() {
            while (running_) {
                try {
                    uint8_t buffer[48];
                    udp::endpoint remote_endpoint;
                    boost::system::error_code ec;
                    size_t len = socket_.receive_from(boost::asio::buffer(buffer), remote_endpoint, 0, ec);
                    if (ec) continue;

                    if (len >= 48) {
                        uint8_t response[48] = {0};
                        response[0] = 0x24; // LI=0, VN=4, Mode=4 (server)
                        
                        auto now = std::chrono::system_clock::now().time_since_epoch();
                        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
                        auto ntp_now_ms = now_ms + offset_ms_;
                        
                        // Convert ntp_now_ms to NTP format
                        const uint32_t NTP_UNIX_DIFF = 2208988800U;
                        uint32_t seconds = static_cast<uint32_t>(ntp_now_ms / 1000) + NTP_UNIX_DIFF;
                        uint32_t fraction = static_cast<uint32_t>((ntp_now_ms % 1000) * 0x100000000ULL / 1000);
                        
                        uint32_t n_seconds = htonl(seconds);
                        uint32_t n_fraction = htonl(fraction);
                        
                        std::memcpy(&response[32], &n_seconds, 4);
                        std::memcpy(&response[36], &n_fraction, 4);
                        std::memcpy(&response[40], &n_seconds, 4);
                        std::memcpy(&response[44], &n_fraction, 4);
                        
                        socket_.send_to(boost::asio::buffer(response), remote_endpoint);
                    }
                } catch (...) {}
            }
        });
    }

    void stop() {
        running_ = false;
        socket_.close();
        if (thread_.joinable()) thread_.join();
    }

    void set_offset(int64_t offset_ms) { offset_ms_ = offset_ms; }

private:
    boost::asio::io_context io_context_;
    udp::socket socket_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int64_t offset_ms_;
};

class ClockDriftMonitorTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init();
        if (std::filesystem::exists("./killswitch")) {
            std::filesystem::remove("./killswitch");
        }
        KillSwitch::instance().start();
    }

    void TearDown() override {
        KillSwitch::instance().stop();
        if (std::filesystem::exists("./killswitch")) {
            std::filesystem::remove("./killswitch");
        }
    }
};

TEST_F(ClockDriftMonitorTest, DetectsDrift) {
    uint16_t port = 12345;
    MockNtpServer server(port, 700); // 700ms drift
    server.start();

    ClockDriftMonitor::Config cfg;
    cfg.sources = {"127.0.0.1:12345"};
    cfg.check_interval_sec = 1;
    cfg.warn_drift_ms = 200;
    cfg.max_clock_drift_ms = 500;

    ClockDriftMonitor monitor(cfg);
    monitor.start();

    // Wait for drift detection and killswitch trigger
    bool triggered = false;
    for (int i = 0; i < 50; ++i) {
        if (KillSwitch::instance().is_triggered()) {
            triggered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(triggered);
    
    monitor.stop();
    server.stop();
}

TEST_F(ClockDriftMonitorTest, Failover) {
    ClockDriftMonitor::Config cfg;
    cfg.sources = {"127.0.0.1:12346", "127.0.0.1:12345"}; // First one fails
    cfg.check_interval_sec = 1;
    
    uint16_t port = 12345;
    MockNtpServer server(port, 100);
    server.start();

    ClockDriftMonitor monitor(cfg);
    monitor.start();

    // Should not trigger killswitch (100ms < 500ms)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_FALSE(KillSwitch::instance().is_triggered());
    
    // Check if we got some drift value
    EXPECT_NEAR(monitor.drift_ms(), 100, 50);

    monitor.stop();
    server.stop();
}
