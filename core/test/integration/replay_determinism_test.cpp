#include <gtest/gtest.h>
#include "transport/ReplayFeed.hpp"
#include "transport/Clocks.hpp"
#include "transport/MarketDataFeed.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "marketdata/LeaderTracker.hpp"
#include "features/FeatureExtractor.hpp"
#include "signals/SignalBus.hpp"
#include "signals/DensityDetector.hpp"
#include "signals/IcebergDetector.hpp"
#include "signals/TapeAnalyzer.hpp"
#include "signals/LevelDetector.hpp"
#include "signals/ApproachAnalyzer.hpp"
#include "strategy/StrategyEngine.hpp"
#include "strategy/BounceFromDensity.hpp"
#include "strategy/BreakoutEatThrough.hpp"
#include "strategy/LeaderLag.hpp"
#include "risk/RiskManager.hpp"
#include "executor/PaperExecutor.hpp"
#include "logger/TradeJournal.hpp"
#include "logger/Logger.hpp"
#include "config/Config.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <iomanip>

namespace trade_bot {
namespace test {

/**
 * PHASE3_DETERMINISM_TEST: Full pipeline replay determinism verification.
 * 
 * This test runs the complete trading pipeline (OrderBook → FeatureExtractor →
 * SignalBus → StrategyEngine → RiskManager → PaperExecutor → TradeJournal)
 * through the same NDJSON dump N=10 times and compares:
 * - Signal sequences (kind, ticker, price, payload hash)
 * - TradePlan sequences (entry/stop/tp, strategy, evidence)
 * - TradeJournal entries (PnL, exit price, cause)
 * 
 * EXPECTED RESULT: This test will FAIL initially, demonstrating non-determinism
 * sources documented in PHASE3_DETERMINISM_AUDIT.md. After Phase 4 fixes are
 * complete (IClock injection, etc.), the test should PASS.
 */

// ═══════════════════════════════════════════════════════════════════════════
// Fingerprint Structures for Comparison
// ═══════════════════════════════════════════════════════════════════════════

struct SignalFingerprint {
    SignalKind kind;
    Ticker ticker;
    int64_t price_tick;  // rounded to tick_size
    uint64_t payload_hash;
    TraceId trigger_trace_id;
    
    bool operator==(const SignalFingerprint& other) const {
        return kind == other.kind &&
               ticker == other.ticker &&
               price_tick == other.price_tick &&
               payload_hash == other.payload_hash &&
               trigger_trace_id == other.trigger_trace_id;
    }
    
    bool operator!=(const SignalFingerprint& other) const {
        return !(*this == other);
    }
};

struct TradePlanFingerprint {
    Ticker ticker;
    Side side;
    int64_t entry_tick;
    int64_t stop_tick;
    int64_t tp1_tick;
    std::string strategy_name;
    std::vector<TraceId> evidence_trace_ids;
    
    bool operator==(const TradePlanFingerprint& other) const {
        return ticker == other.ticker &&
               side == other.side &&
               entry_tick == other.entry_tick &&
               stop_tick == other.stop_tick &&
               tp1_tick == other.tp1_tick &&
               strategy_name == other.strategy_name &&
               evidence_trace_ids == other.evidence_trace_ids;
    }
    
    bool operator!=(const TradePlanFingerprint& other) const {
        return !(*this == other);
    }
};

struct JournalFingerprint {
    Ticker ticker;
    std::string strategy_name;
    Side side;
    int64_t entry_tick;
    int64_t exit_tick;
    int64_t pnl_usd_cents;
    std::string cause_of_exit;
    
    bool operator==(const JournalFingerprint& other) const {
        return ticker == other.ticker &&
               strategy_name == other.strategy_name &&
               side == other.side &&
               entry_tick == other.entry_tick &&
               exit_tick == other.exit_tick &&
               pnl_usd_cents == other.pnl_usd_cents &&
               cause_of_exit == other.cause_of_exit;
    }
    
    bool operator!=(const JournalFingerprint& other) const {
        return !(*this == other);
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Helper Functions
// ═══════════════════════════════════════════════════════════════════════════

static uint64_t hash_signal_payload(const Signal& sig) {
    // Simple hash of payload fields (not cryptographic, just for comparison)
    uint64_t h = 0;
    h ^= std::hash<double>{}(sig.payload.size);
    h ^= std::hash<double>{}(sig.payload.speed_bps);
    h ^= std::hash<int>{}(sig.payload.age_ms);
    h ^= std::hash<int>{}(sig.payload.touches);
    h ^= std::hash<double>{}(sig.payload.lag_pct);
    h ^= std::hash<std::string>{}(std::string(sig.payload.side));
    h ^= std::hash<std::string>{}(std::string(sig.payload.approach_type));
    return h;
}

[[maybe_unused]] static SignalFingerprint make_fingerprint(const Signal& sig, double tick_size) {
    SignalFingerprint fp;
    fp.kind = sig.kind;
    fp.ticker = sig.ticker;
    fp.price_tick = static_cast<int64_t>(std::round(sig.price / tick_size));
    fp.payload_hash = hash_signal_payload(sig);
    fp.trigger_trace_id = sig.trigger_trace_id;
    return fp;
}

[[maybe_unused]] static TradePlanFingerprint make_fingerprint(const TradePlan& plan, double tick_size) {
    TradePlanFingerprint fp;
    fp.ticker = plan.ticker;
    fp.side = plan.side;
    fp.entry_tick = static_cast<int64_t>(std::round(plan.entry_price / tick_size));
    fp.stop_tick = static_cast<int64_t>(std::round(plan.stop_price / tick_size));
    fp.tp1_tick = static_cast<int64_t>(std::round(plan.tp1_price / tick_size));
    fp.strategy_name = std::string(plan.strategy_name);
    
    // Extract and sort evidence trace IDs for deterministic comparison
    for (const auto& sig : plan.evidence) {
        fp.evidence_trace_ids.push_back(sig.trigger_trace_id);
    }
    std::sort(fp.evidence_trace_ids.begin(), fp.evidence_trace_ids.end());
    
    return fp;
}

[[maybe_unused]] static JournalFingerprint make_fingerprint(const TradeJournal::Entry& entry, double tick_size) {
    JournalFingerprint fp;
    fp.ticker = entry.plan.ticker;
    fp.strategy_name = std::string(entry.plan.strategy_name);
    fp.side = entry.plan.side;
    fp.entry_tick = static_cast<int64_t>(std::round(entry.plan.entry_price / tick_size));
    fp.exit_tick = static_cast<int64_t>(std::round(entry.exit_price / tick_size));
    fp.pnl_usd_cents = static_cast<int64_t>(std::round(entry.pnl_usd * 100.0));
    fp.cause_of_exit = std::string(entry.cause_of_exit);
    return fp;
}

// ═══════════════════════════════════════════════════════════════════════════
// Replay Run State
// ═══════════════════════════════════════════════════════════════════════════

struct ReplayRunState {
    // Captured sequences
    std::vector<Signal> signals;
    std::vector<TradePlan> plans;
    std::vector<TradeJournal::Entry> journal_entries;
    
    // Fingerprints for comparison
    std::vector<SignalFingerprint> signal_fps;
    std::vector<TradePlanFingerprint> plan_fps;
    std::vector<JournalFingerprint> journal_fps;
};

// ═══════════════════════════════════════════════════════════════════════════
// Test Fixture
// ═══════════════════════════════════════════════════════════════════════════

class ReplayDeterminismTest : public ::testing::Test {
protected:
    void SetUp() override {
        Logger::init("test_logs/replay_determinism_test.log");
        temp_dir_ = std::filesystem::temp_directory_path() / "replay_determinism_test";
        std::filesystem::create_directories(temp_dir_);
        
        // Create fixtures directory
        fixtures_dir_ = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "replay";
        std::filesystem::create_directories(fixtures_dir_);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(temp_dir_);
    }
    
    std::filesystem::path temp_dir_;
    std::filesystem::path fixtures_dir_;
    
    // Generate a synthetic deterministic dump for testing
    std::string generate_synthetic_dump() {
        auto dump_path = temp_dir_ / "synthetic.ndjson";
        std::ofstream out(dump_path);
        
        uint64_t base_ts = 1700000000000000000ULL;  // Nov 2023
        const double base_price = 50000.0;
        
        // Orderbook snapshot
        out << R"({"recv_ts_ns":)" << base_ts << R"(,"message":{"Type":"orderbook_snapshot","Data":{"Ticker":"BTC_USDT","Bids":[[)" 
            << base_price << R"(,10.0],[)" << (base_price - 1.0) << R"(,20.0]],"Asks":[[)" 
            << (base_price + 1.0) << R"(,10.0],[)" << (base_price + 2.0) << R"(,20.0]],"Timestamp":"2023-11-14T12:00:00Z"}}})" << "\n";
        
        // Generate trades and orderbook updates to trigger signals
        for (int i = 0; i < 100; ++i) {
            uint64_t ts = base_ts + i * 100000000ULL;  // 100ms apart
            double price = base_price + std::sin(i * 0.1) * 10.0;  // Sine wave
            
            // Trade
            out << R"({"recv_ts_ns":)" << ts << R"(,"message":{"Type":"trade_update","Data":{"Ticker":"BTC_USDT","Trades":[{"Price":)" 
                << price << R"(,"Size":0.5,"Side":"Buy","Timestamp":"2023-11-14T12:00:00Z"}]}}})" << "\n";
            
            // Orderbook update (create density)
            if (i % 20 == 0) {
                double density_price = price + 5.0;
                out << R"({"recv_ts_ns":)" << ts << R"(,"message":{"Type":"orderbook_update","Data":{"Ticker":"BTC_USDT","Bids":[],"Asks":[[)" 
                    << density_price << R"(,50.0]],"Timestamp":"2023-11-14T12:00:00Z"}}})" << "\n";
            }
        }
        
        // Funding update
        out << R"({"recv_ts_ns":)" << (base_ts + 5000000000ULL) << R"(,"message":{"Type":"funding_update","Data":{"Ticker":"BTC_USDT","FundingRate":0.0001,"FundingTime":"2023-11-14T16:00:00Z"}}})" << "\n";
        
        // Mark price update
        out << R"({"recv_ts_ns":)" << (base_ts + 5100000000ULL) << R"(,"message":{"Type":"mark_price_update","Data":{"Ticker":"BTC_USDT","MarkPrice":)" << base_price << R"(}}})" << "\n";
        
        out.close();
        return dump_path.string();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Minimal Test: Message Counting (Existing)
// ═══════════════════════════════════════════════════════════════════════════

class MessageCounter : public IMarketDataListener {
public:
    void on_trade(const Ticker&, const Trade&) override { trade_count_++; }
    void on_trades(const Ticker&, const std::vector<Trade>& trades) override { 
        trade_count_ += trades.size(); 
    }
    void on_orderbook_snapshot(const OrderBookSnapshot&) override { snapshot_count_++; }
    void on_orderbook_update(const OrderBookUpdate&) override { update_count_++; }
    void on_order_update(const OrderUpdate&) override {}
    void on_position_update(const PositionUpdate&) override {}
    void on_balance_update(const BalanceUpdate&) override {}
    void on_finres_update(const FinresUpdate&) override {}
    void on_error(const std::string&) override {}
    
    size_t trade_count() const { return trade_count_; }
    size_t snapshot_count() const { return snapshot_count_; }
    size_t update_count() const { return update_count_; }
    
private:
    size_t trade_count_{0};
    size_t snapshot_count_{0};
    size_t update_count_{0};
};

TEST_F(ReplayDeterminismTest, SyntheticDumpReplayable) {
    auto dump_path = generate_synthetic_dump();
    
    // Run replay 3 times and verify identical message counts
    std::vector<MessageCounter> counters(3);
    
    for (int run = 0; run < 3; ++run) {
        auto clock = std::make_shared<VirtualClock>();
        ReplayFeed feed(dump_path, clock, 0.0);  // as-fast-as-possible
        feed.add_listener(&counters[run]);
        
        auto stats = feed.run();
        
        LOG_INFO("Run {}: messages={}, trades={}, snapshots={}, updates={}", 
                 run, stats.messages_dispatched, 
                 counters[run].trade_count(),
                 counters[run].snapshot_count(),
                 counters[run].update_count());
    }
    
    // Verify all runs produced identical counts
    for (int i = 1; i < 3; ++i) {
        EXPECT_EQ(counters[0].trade_count(), counters[i].trade_count())
            << "Trade count diverged on run " << i;
        EXPECT_EQ(counters[0].snapshot_count(), counters[i].snapshot_count())
            << "Snapshot count diverged on run " << i;
        EXPECT_EQ(counters[0].update_count(), counters[i].update_count())
            << "Update count diverged on run " << i;
    }
    
    // Basic sanity checks
    EXPECT_GT(counters[0].trade_count(), 0) << "No trades received";
    EXPECT_GT(counters[0].snapshot_count(), 0) << "No snapshots received";
}

// ═══════════════════════════════════════════════════════════════════════════
// Full Pipeline Test (DISABLED - Requires Complete Implementation)
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(ReplayDeterminismTest, FullPipelineDeterminism) {
    /**
     * IMPLEMENTATION NOTE:
     * 
     * This test is DISABLED because it requires:
     * 1. Complete pipeline wiring (OrderBook, FeatureExtractor, SignalBus, etc.)
     * 2. Signal/Plan/Journal capture infrastructure
     * 3. IClock injection into all strategies
     * 4. Config loading for detector/strategy thresholds
     * 
     * The test design is documented in PHASE3_DETERMINISM_TEST_PLAN.md.
     * The expected non-determinism sources are cataloged in PHASE3_DETERMINISM_AUDIT.md.
     * 
     * To enable this test:
     * 1. Complete Phase 4 P0 fixes (IClock .cpp updates, BotApp injection)
     * 2. Implement pipeline wiring below (see plan document for details)
     * 3. Remove DISABLED_ prefix
     * 4. Run test (expect FAILURE initially, PASS after fixes)
     * 
     * EXPECTED RESULT (before fixes):
     * - Test runs 10 times
     * - Divergences detected in signals/plans/journal
     * - Diagnostic output shows first divergence
     * - Audit report updated with actual divergences
     * 
     * EXPECTED RESULT (after fixes):
     * - All 10 runs match baseline
     * - 100% determinism achieved
     * - Test PASSES
     */
    
    GTEST_SKIP() << "Full pipeline test requires complete implementation - see PHASE3_DETERMINISM_TEST_PLAN.md";
    
    // ═══════════════════════════════════════════════════════════════════════
    // Pipeline Wiring Skeleton (for reference)
    // ═══════════════════════════════════════════════════════════════════════
    
    /*
    const int N_RUNS = 10;
    const double TICK_SIZE = 0.01;  // BTC_USDT tick size
    std::vector<ReplayRunState> runs;
    
    auto dump_path = generate_synthetic_dump();
    // Or use real dump: fixtures_dir_ / "btc_15min.ndjson"
    
    for (int run_idx = 0; run_idx < N_RUNS; ++run_idx) {
        ReplayRunState state;
        
        // Create isolated components with VirtualClock
        auto clock = std::make_shared<VirtualClock>();
        auto replay_feed = std::make_unique<ReplayFeed>(dump_path, clock, 0.0);
        
        // OrderBook + TradeStream + FeatureExtractor
        auto book = std::make_unique<OrderBook>("BTC_USDT", TickerInfo{});
        auto stream = std::make_unique<TradeStream>("BTC_USDT");
        auto extractor = std::make_unique<FeatureExtractor>("BTC_USDT");
        extractor->set_sources(book.get(), stream.get(), nullptr);
        
        // SignalBus + Detectors
        auto signal_bus = std::make_unique<SignalBus>();
        
        // Capture signals
        signal_bus->subscribe([&state, TICK_SIZE](const Signal& sig) {
            state.signals.push_back(sig);
            state.signal_fps.push_back(make_fingerprint(sig, TICK_SIZE));
        });
        
        // Create detectors (with config)
        // auto density_detector = std::make_unique<DensityDetector>(...);
        // auto iceberg_detector = std::make_unique<IcebergDetector>(...);
        // ... etc
        
        // StrategyEngine + Strategies
        auto strategy_engine = std::make_unique<StrategyEngine>(*signal_bus);
        
        // Capture plans
        strategy_engine->set_on_plan([&state, TICK_SIZE](const TradePlan& plan) {
            state.plans.push_back(plan);
            state.plan_fps.push_back(make_fingerprint(plan, TICK_SIZE));
        });
        
        // Create strategies with VirtualClock injection
        auto bounce = std::make_unique<BounceFromDensity>("BTC_USDT", TickerInfo{});
        bounce->set_clock(clock.get());
        strategy_engine->add_strategy(std::move(bounce));
        
        // RiskManager + PaperExecutor + TradeJournal
        auto risk_manager = std::make_unique<RiskManager>(...);
        auto executor = std::make_unique<PaperExecutor>(...);
        auto journal = std::make_unique<TradeJournal>(temp_dir_.string());
        
        // Wire ReplayFeed → OrderBook
        replay_feed->add_listener(book.get());
        replay_feed->add_listener(stream.get());
        
        // Run replay
        auto stats = replay_feed->run();
        
        // Extract journal entries
        state.journal_entries = journal->get_all_entries();
        for (const auto& entry : state.journal_entries) {
            state.journal_fps.push_back(make_fingerprint(entry, TICK_SIZE));
        }
        
        runs.push_back(std::move(state));
        
        LOG_INFO("Run {}: {} signals, {} plans, {} journal entries",
                 run_idx, state.signals.size(), state.plans.size(), state.journal_entries.size());
    }
    
    // Compare all runs to baseline (run 0)
    int diverged_runs = 0;
    for (int i = 1; i < N_RUNS; ++i) {
        bool diverged = false;
        
        // Compare signals
        if (runs[0].signal_fps != runs[i].signal_fps) {
            diverged = true;
            for (size_t j = 0; j < std::min(runs[0].signal_fps.size(), runs[i].signal_fps.size()); ++j) {
                if (runs[0].signal_fps[j] != runs[i].signal_fps[j]) {
                    LOG_ERROR("Run {} DIVERGED at signal #{}", i, j);
                    LOG_ERROR("  Baseline: kind={}, ticker={}, price_tick={}, hash={:#x}",
                             static_cast<int>(runs[0].signal_fps[j].kind),
                             runs[0].signal_fps[j].ticker,
                             runs[0].signal_fps[j].price_tick,
                             runs[0].signal_fps[j].payload_hash);
                    LOG_ERROR("  Run {}:    kind={}, ticker={}, price_tick={}, hash={:#x}",
                             i,
                             static_cast<int>(runs[i].signal_fps[j].kind),
                             runs[i].signal_fps[j].ticker,
                             runs[i].signal_fps[j].price_tick,
                             runs[i].signal_fps[j].payload_hash);
                    break;
                }
            }
        }
        
        // Compare plans
        if (runs[0].plan_fps != runs[i].plan_fps) {
            diverged = true;
            for (size_t j = 0; j < std::min(runs[0].plan_fps.size(), runs[i].plan_fps.size()); ++j) {
                if (runs[0].plan_fps[j] != runs[i].plan_fps[j]) {
                    LOG_ERROR("Run {} DIVERGED at plan #{}", i, j);
                    LOG_ERROR("  Baseline: strategy={}, ticker={}, side={}, entry={}, stop={}",
                             runs[0].plan_fps[j].strategy_name,
                             runs[0].plan_fps[j].ticker,
                             static_cast<int>(runs[0].plan_fps[j].side),
                             runs[0].plan_fps[j].entry_tick,
                             runs[0].plan_fps[j].stop_tick);
                    LOG_ERROR("  Run {}:    strategy={}, ticker={}, side={}, entry={}, stop={}",
                             i,
                             runs[i].plan_fps[j].strategy_name,
                             runs[i].plan_fps[j].ticker,
                             static_cast<int>(runs[i].plan_fps[j].side),
                             runs[i].plan_fps[j].entry_tick,
                             runs[i].plan_fps[j].stop_tick);
                    break;
                }
            }
        }
        
        // Compare journal
        if (runs[0].journal_fps != runs[i].journal_fps) {
            diverged = true;
            for (size_t j = 0; j < std::min(runs[0].journal_fps.size(), runs[i].journal_fps.size()); ++j) {
                if (runs[0].journal_fps[j] != runs[i].journal_fps[j]) {
                    LOG_ERROR("Run {} DIVERGED at journal entry #{}", i, j);
                    LOG_ERROR("  Baseline: ticker={}, pnl_cents={}, exit={}, cause={}",
                             runs[0].journal_fps[j].ticker,
                             runs[0].journal_fps[j].pnl_usd_cents,
                             runs[0].journal_fps[j].exit_tick,
                             runs[0].journal_fps[j].cause_of_exit);
                    LOG_ERROR("  Run {}:    ticker={}, pnl_cents={}, exit={}, cause={}",
                             i,
                             runs[i].journal_fps[j].ticker,
                             runs[i].journal_fps[j].pnl_usd_cents,
                             runs[i].journal_fps[j].exit_tick,
                             runs[i].journal_fps[j].cause_of_exit);
                    break;
                }
            }
        }
        
        if (diverged) {
            diverged_runs++;
            EXPECT_TRUE(false) << "Run " << i << " diverged from baseline";
        } else {
            LOG_INFO("Run {} MATCHED baseline", i);
        }
    }
    
    LOG_INFO("SUMMARY: {}/{} runs matched baseline ({:.1f}% determinism)",
             N_RUNS - diverged_runs - 1, N_RUNS - 1,
             (N_RUNS - diverged_runs - 1) * 100.0 / (N_RUNS - 1));
    
    // Test FAILS if any divergence detected (expected before Phase 4 fixes)
    EXPECT_EQ(diverged_runs, 0) << diverged_runs << " runs diverged from baseline";
    */
}

}  // namespace test
}  // namespace trade_bot
