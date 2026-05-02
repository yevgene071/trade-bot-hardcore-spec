#include "strategy/StrategyEngine.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;

class MockStrategy : public IStrategy {
public:
    MOCK_METHOD(const std::string&, name, (), (const, override));
    MOCK_METHOD(void, on_frame, (const FeatureFrame&), (override));
    MOCK_METHOD(void, on_signal, (const Signal&), (override));
    MOCK_METHOD(std::optional<TradePlan>, tick, (std::chrono::system_clock::time_point), (override));
};

TEST(StrategyEngineTest, RoutesEventsAndCollectsPlans) {
    SignalBus bus;
    StrategyEngine engine(bus);
    
    auto strat = std::make_unique<MockStrategy>();
    auto* strat_ptr = strat.get();
    engine.add_strategy(std::move(strat));
    
    FeatureFrame frame{};
    frame.ticker = "BTCUSDT";
    
    Signal signal{};
    signal.kind = SignalKind::TapeBurst;
    signal.ticker = "BTCUSDT";
    
    EXPECT_CALL(*strat_ptr, on_frame(testing::_)).Times(1);
    EXPECT_CALL(*strat_ptr, on_signal(testing::_)).Times(1);
    
    TradePlan mock_plan{};
    mock_plan.ticker = "BTCUSDT";
    mock_plan.strategy_name = "Mock";
    
    EXPECT_CALL(*strat_ptr, tick(testing::_)).WillOnce(testing::Return(mock_plan));
    
    int plans_received = 0;
    engine.set_on_plan([&plans_received](const TradePlan&){ plans_received++; });
    
    engine.on_frame(frame);
    bus.publish(signal);
    engine.tick(std::chrono::system_clock::now());
    
    EXPECT_EQ(plans_received, 1);
}
