#include "signals/SignalBus.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

TEST(SignalBusTest, PublishesToSubscribers) {
    SignalBus bus;
    int count = 0;
    bus.subscribe([&count](const Signal&){ count++; });
    bus.subscribe([&count](const Signal&){ count++; });
    
    Signal s {
        .kind = SignalKind::TapeBurst,
        .timestamp = std::chrono::system_clock::now(),
        .ticker = "BTCUSDT",
        .price = 50000.0,
        .confidence = 0.9,
        .payload = {}
    };
    
    bus.publish(s);
    EXPECT_EQ(count, 2);
}
