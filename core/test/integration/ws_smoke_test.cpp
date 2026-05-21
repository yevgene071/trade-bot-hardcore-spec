#include <gtest/gtest.h>
#include "transport/BeastWsClient.hpp"
#include "logger/Logger.hpp"
#include <boost/asio.hpp>
#include <thread>
#include <atomic>

using namespace trade_bot;

TEST(WsSmokeTest, LiveConnection) {
    char* run_integration = std::getenv("WITH_METASCALP");
    if (!run_integration) {
        GTEST_SKIP() << "Skipping integration test: WITH_METASCALP env var not set";
    }

    boost::asio::io_context ioc;
    auto client = std::make_shared<BeastWsClient>(ioc);
    
    std::atomic<bool> subscribed{false};
    client->set_on_message([&](const nlohmann::json& j, uint64_t /*recv_ns*/, trade_bot::TraceId /*tid*/) {
        LOG_INFO("WS received: {}", j.dump());
        if (j.contains("Type") && j["Type"] == "subscribed") {
            subscribed = true;
        }
    });

    client->connect("ws://127.0.0.1:17845/");

    std::thread t([&]() { ioc.run(); });

    // Wait for connection
    int retries = 50;
    while (!client->is_connected() && retries-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (client->is_connected()) {
        nlohmann::json sub = {
            {"Type", "subscribe"},
            {"Data", {{"ConnectionId", 1}}}
        };
        client->send(sub.dump());

        // Wait for ack
        retries = 50;
        while (!subscribed && retries-- > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        EXPECT_TRUE(subscribed);
    } else {
        FAIL() << "Could not connect to MetaScalp";
    }

    client->disconnect();
    ioc.stop();
    if (t.joinable()) t.join();
}
