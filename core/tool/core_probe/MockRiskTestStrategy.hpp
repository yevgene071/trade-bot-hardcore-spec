#pragma once

#include "strategy/IStrategy.hpp"
#include "strategy/TradePlan.hpp"

#include <string>
#include <optional>
#include <utility>

namespace trade_bot::probe {

class MockRiskTestStrategy final : public IStrategy {
public:
    explicit MockRiskTestStrategy(Ticker ticker)
        : ticker_(std::move(ticker))
        , name_("MockRiskTestStrategy") {}

    const std::string& name() const override { return name_; }
    const Ticker& ticker() const override { return ticker_; }

    void on_frame(const FeatureFrame&) override {}

    std::optional<TradePlan> on_signal(const Signal&, std::chrono::system_clock::time_point) override {
        return next_plan_;
    }

    std::optional<TradePlan> tick(std::chrono::system_clock::time_point) override {
        return next_plan_;
    }

    bool has_active_plan() const override { return next_plan_.has_value(); }

    void reset_active_plan() override { next_plan_.reset(); }

    void on_plan_accepted(const TradePlan&) override {}

    StrategyState get_state() const override {
        StrategyState state;
        state.ticker = ticker_;
        state.strategy_name = name_;
        state.ready_state = StrategyReadyState::Ready;
        state.readiness_pct = 100.0;
        return state;
    }

    void set_next_plan(const std::optional<TradePlan>& plan) {
        next_plan_ = plan;
    }

private:
    Ticker ticker_;
    std::string name_;
    std::optional<TradePlan> next_plan_;
};

} // namespace trade_bot::probe
