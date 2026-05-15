#include "strategy/StrategyEngine.hpp"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

using namespace trade_bot;

class MockStrategy : public IStrategy {
public:
    MOCK_METHOD(const std::string&, name, (), (const, override));
    MOCK_METHOD(const Ticker&, ticker, (), (const, override));
    MOCK_METHOD(void, on_frame, (const FeatureFrame&), (override));
    MOCK_METHOD(void, on_signal, (const Signal&), (override));
    MOCK_METHOD(std::optional<TradePlan>, tick, (std::chrono::system_clock::time_point), (override));
    MOCK_METHOD(void, reset_active_plan, (), (override));
    MOCK_METHOD(StrategyState, get_state, (), (const, override));
};

TEST(StrategyEngineTest, RoutesEventsAndCollectsPlans) {
    SignalBus bus;
    StrategyEngine engine(bus);
    
    auto strat = std::make_unique<MockStrategy>();
    auto* strat_ptr = strat.get();
    
    Ticker ticker = "BTCUSDT";
    std::string name = "Mock";
    EXPECT_CALL(*strat_ptr, ticker()).WillRepeatedly(testing::ReturnRef(ticker));
    EXPECT_CALL(*strat_ptr, name()).WillRepeatedly(testing::ReturnRef(name));
    
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

TEST(StrategyEngineTest, GetAllStatesCollectsFromAllStrategies) {
    SignalBus bus;
    StrategyEngine engine(bus);

    // Strategy 1: "BTCUSDT" / "MockA" → Warming
    auto stratA = std::make_unique<MockStrategy>();
    auto* ptrA = stratA.get();
    Ticker tickerA = "BTCUSDT";
    std::string nameA = "MockA";
    EXPECT_CALL(*ptrA, ticker()).WillRepeatedly(testing::ReturnRef(tickerA));
    EXPECT_CALL(*ptrA, name()).WillRepeatedly(testing::ReturnRef(nameA));

    StrategyState stateA;
    stateA.ticker = "BTCUSDT";
    stateA.strategy_name = "MockA";
    stateA.ready_state = StrategyReadyState::Warming;
    stateA.readiness_pct = 60.0;
    StrategyCondition cond;
    cond.name = "CUSUM";
    cond.current = 24.0;
    cond.target = 30.0;
    cond.met = false;
    cond.unit = "samples";
    stateA.conditions.push_back(cond);
    stateA.signals_last_60s = 5;
    EXPECT_CALL(*ptrA, get_state()).WillRepeatedly(testing::Return(stateA));

    engine.add_strategy(std::move(stratA));

    // Strategy 2: "ETHUSDT" / "MockB" → Ready
    auto stratB = std::make_unique<MockStrategy>();
    auto* ptrB = stratB.get();
    Ticker tickerB = "ETHUSDT";
    std::string nameB = "MockB";
    EXPECT_CALL(*ptrB, ticker()).WillRepeatedly(testing::ReturnRef(tickerB));
    EXPECT_CALL(*ptrB, name()).WillRepeatedly(testing::ReturnRef(nameB));

    StrategyState stateB;
    stateB.ticker = "ETHUSDT";
    stateB.strategy_name = "MockB";
    stateB.ready_state = StrategyReadyState::Ready;
    stateB.readiness_pct = 100.0;
    stateB.signals_last_60s = 12;
    EXPECT_CALL(*ptrB, get_state()).WillRepeatedly(testing::Return(stateB));

    engine.add_strategy(std::move(stratB));

    auto states = engine.get_all_states();
    EXPECT_EQ(states.size(), 2u);

    // Verify both strategies are present (order may vary by map iteration)
    bool foundA = false, foundB = false;
    for (const auto& s : states) {
        if (s.ticker == "BTCUSDT") {
            foundA = true;
            EXPECT_EQ(s.strategy_name, "MockA");
            EXPECT_EQ(s.ready_state, StrategyReadyState::Warming);
            EXPECT_DOUBLE_EQ(s.readiness_pct, 60.0);
            EXPECT_EQ(s.conditions.size(), 1u);
            EXPECT_EQ(s.conditions[0].name, "CUSUM");
            EXPECT_DOUBLE_EQ(s.conditions[0].current, 24.0);
            EXPECT_DOUBLE_EQ(s.conditions[0].target, 30.0);
            EXPECT_FALSE(s.conditions[0].met);
            EXPECT_EQ(s.conditions[0].unit, "samples");
            EXPECT_EQ(s.signals_last_60s, 5);
        }
        if (s.ticker == "ETHUSDT") {
            foundB = true;
            EXPECT_EQ(s.strategy_name, "MockB");
            EXPECT_EQ(s.ready_state, StrategyReadyState::Ready);
            EXPECT_DOUBLE_EQ(s.readiness_pct, 100.0);
            EXPECT_EQ(s.signals_last_60s, 12);
        }
    }
    EXPECT_TRUE(foundA && foundB) << "Both strategies should be in result";
}
