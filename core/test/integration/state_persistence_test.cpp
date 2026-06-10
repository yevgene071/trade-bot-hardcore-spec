#include "risk/AccountStatePersister.hpp"

#include <filesystem>
#include <gtest/gtest.h>

using namespace trade_bot;

// FN-008: Verify that SignalKind ordinals remain stable. If this fails,
// persisted integer values in journal/account_state.json would silently
// map to the wrong signal kind.
static_assert(static_cast<int>(SignalKind::DensityDetected) == 0,  "ordinal drift: DensityDetected");
static_assert(static_cast<int>(SignalKind::DensityRemoved) == 1,   "ordinal drift: DensityRemoved");
static_assert(static_cast<int>(SignalKind::DensityEating) == 2,    "ordinal drift: DensityEating");
static_assert(static_cast<int>(SignalKind::IcebergSuspected) == 3, "ordinal drift: IcebergSuspected");
static_assert(static_cast<int>(SignalKind::TapeBurst) == 4,        "ordinal drift: TapeBurst");
static_assert(static_cast<int>(SignalKind::TapeFade) == 5,         "ordinal drift: TapeFade");
static_assert(static_cast<int>(SignalKind::TapeFlush) == 6,        "ordinal drift: TapeFlush");
static_assert(static_cast<int>(SignalKind::TapeDistribution) == 7, "ordinal drift: TapeDistribution");
static_assert(static_cast<int>(SignalKind::LevelFormed) == 8,      "ordinal drift: LevelFormed");
static_assert(static_cast<int>(SignalKind::LevelApproach) == 9,    "ordinal drift: LevelApproach");
static_assert(static_cast<int>(SignalKind::LevelRejection) == 10,  "ordinal drift: LevelRejection");
static_assert(static_cast<int>(SignalKind::LevelBreak) == 11,      "ordinal drift: LevelBreak");
static_assert(static_cast<int>(SignalKind::LeaderMove) == 12,      "ordinal drift: LeaderMove");
static_assert(static_cast<int>(SignalKind::DensityStack) == 13,    "ordinal drift: DensityStack");

TEST(StatePersistenceTest, SaveAndLoadMatch) {
    const std::string path = "test_persist.json";
    if (std::filesystem::exists(path))
        std::filesystem::remove(path);

    AccountStatePersister persister(path);

    AccountStatePersister::PersistedData data;
    data.last_reset_day_utc = "2026-05-02";
    data.account_state.equity_usd = 12345.67;
    data.account_state.active_positions = 2;
    data.account_state.active_tickers = {"BTCUSDT", "ETHUSDT"};

    ActiveTrade t;
    t.plan.ticker = "BTCUSDT";
    t.plan.strategy_name = "Bounce";
    t.plan.tp2_price = 50500.0;
    t.plan.no_progress_timeout_sec = 42.0;
    t.plan.post_entry_grace_sec = 3.0;
    t.plan.min_follow_through_bps = 7.0;
    t.plan.entry_correlation = 0.81;
    t.plan.leader_entry_lag_pct = -0.12;
    t.plan.correlation_exit_threshold = 0.35;
    t.plan.leader_exit_reversal_bps = 4.5;
    t.plan.density_price_for_stop = 49980.0;
    t.plan.approach_count = 3;
    t.plan.trace_id = 12345;
    t.state = TradeState::Open;
    t.executed_size = 0.5;
    data.active_trades.push_back(t);

    persister.save(data);

    auto loaded = persister.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->last_reset_day_utc, "2026-05-02");
    EXPECT_DOUBLE_EQ(loaded->account_state.equity_usd, 12345.67);
    ASSERT_EQ(loaded->active_trades.size(), 1);
    EXPECT_EQ(loaded->active_trades[0].plan.ticker, "BTCUSDT");
    EXPECT_EQ(loaded->active_trades[0].state, TradeState::Open);
    const auto& plan = loaded->active_trades[0].plan;
    ASSERT_TRUE(plan.tp2_price.has_value());
    EXPECT_DOUBLE_EQ(*plan.tp2_price, 50500.0);
    EXPECT_DOUBLE_EQ(plan.no_progress_timeout_sec, 42.0);
    EXPECT_DOUBLE_EQ(plan.post_entry_grace_sec, 3.0);
    EXPECT_DOUBLE_EQ(plan.min_follow_through_bps, 7.0);
    EXPECT_DOUBLE_EQ(plan.entry_correlation, 0.81);
    EXPECT_DOUBLE_EQ(plan.leader_entry_lag_pct, -0.12);
    EXPECT_DOUBLE_EQ(plan.correlation_exit_threshold, 0.35);
    EXPECT_DOUBLE_EQ(plan.leader_exit_reversal_bps, 4.5);
    EXPECT_DOUBLE_EQ(plan.density_price_for_stop, 49980.0);
    EXPECT_EQ(plan.approach_count, 3);
    EXPECT_EQ(plan.trace_id, 12345);

    std::filesystem::remove(path);
}

// FN-008: Verify that new DensityStack / DensityEating payload fields
// round-trip through JSON persistence and that old JSON missing the new
// fields still loads with zero defaults.
TEST(StatePersistenceTest, NewPayloadFieldsRoundTrip) {
    const std::string path = "test_persist_new_fields.json";
    if (std::filesystem::exists(path))
        std::filesystem::remove(path);

    AccountStatePersister persister(path);

    // Build a Signal with all the new payload fields populated.
    Signal sig;
    sig.kind = SignalKind::DensityStack;
    sig.ticker = "BTCUSDT";
    sig.price = 65432.0;
    sig.confidence = 0.95;
    sig.payload.side = "Ask";
    sig.payload.size = 12.5;
    sig.payload.remaining_ratio = 0.4;
    sig.payload.first_price = 65400.0;
    sig.payload.last_price = 65500.0;
    sig.payload.width_bps = 15.3;
    sig.payload.total_size_usd = 818000.0;
    sig.payload.stop_anchor_price = 65500.0;
    sig.payload.eaten_ratio = 0.6;
    sig.payload.original_size = 20.0;
    sig.payload.remaining_size = 8.0;

    // Serialize → deserialize and verify round-trip.
    nlohmann::json j = sig;
    Signal loaded = j.get<Signal>();

    EXPECT_EQ(loaded.kind, SignalKind::DensityStack);
    EXPECT_EQ(static_cast<int>(loaded.kind), 13);
    EXPECT_DOUBLE_EQ(loaded.payload.remaining_ratio, 0.4);
    EXPECT_DOUBLE_EQ(loaded.payload.first_price, 65400.0);
    EXPECT_DOUBLE_EQ(loaded.payload.last_price, 65500.0);
    EXPECT_DOUBLE_EQ(loaded.payload.width_bps, 15.3);
    EXPECT_DOUBLE_EQ(loaded.payload.total_size_usd, 818000.0);
    EXPECT_DOUBLE_EQ(loaded.payload.stop_anchor_price, 65500.0);
    EXPECT_DOUBLE_EQ(loaded.payload.eaten_ratio, 0.6);
    EXPECT_DOUBLE_EQ(loaded.payload.original_size, 20.0);
    EXPECT_DOUBLE_EQ(loaded.payload.remaining_size, 8.0);

    std::filesystem::remove(path);
}

TEST(StatePersistenceTest, OldJsonMissingNewFieldsLoadsZeroDefaults) {
    // Simulate old persisted JSON that lacks the new fields.
    // from_json must default missing fields to 0.0.
    nlohmann::json old = {
        {"side", "Bid"},
        {"size", 5.0},
        {"size_usd", 250000.0},
        {"total_eaten_usd", 0.0},
        {"original_size", 5.0},
        {"remaining_size", 2.0},
        {"eaten_ratio", 0.6},
        // remaining_ratio, first_price, last_price, width_bps,
        // total_size_usd, stop_anchor_price intentionally omitted.
        {"lag_pct", 0.0},
        {"correlation", 0.0},
        {"dist_bps", 10.0},
        {"delta_bps", 0.0},
        {"leader_move_pct", 0.0},
        {"our_move_pct", 0.0},
        {"expected_move_pct", 0.0},
        {"lag_ms", 0.0},
        {"ratio", 0.0},
        {"intensity", 0.0},
        {"peak_rate", 0.0},
        {"current_rate", 0.0},
        {"cusum", 0.0},
        {"volatility_bps", 0.0},
        {"volume_usd_30s", 0.0},
        {"max_range_bps", 0.0},
        {"touches", 0},
        {"prints", 3},
        {"age_ms", 500},
        {"refill_events", 0},
        {"fake", false},
        {"id", ""},
        {"source", ""}
    };

    SignalPayload p = old.get<SignalPayload>();
    EXPECT_DOUBLE_EQ(p.size, 5.0);
    EXPECT_DOUBLE_EQ(p.remaining_size, 2.0);
    EXPECT_DOUBLE_EQ(p.eaten_ratio, 0.6);
    // New fields must default to zero.
    EXPECT_DOUBLE_EQ(p.remaining_ratio, 0.0);
    EXPECT_DOUBLE_EQ(p.first_price, 0.0);
    EXPECT_DOUBLE_EQ(p.last_price, 0.0);
    EXPECT_DOUBLE_EQ(p.width_bps, 0.0);
    EXPECT_DOUBLE_EQ(p.total_size_usd, 0.0);
    EXPECT_DOUBLE_EQ(p.stop_anchor_price, 0.0);
}
