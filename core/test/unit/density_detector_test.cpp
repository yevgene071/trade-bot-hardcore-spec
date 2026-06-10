#include "signals/DensityDetector.hpp"
#include "signals/SignalBus.hpp"
#include "marketdata/OrderBook.hpp"
#include "universe/TickerUniverse.hpp"
#include "logger/Logger.hpp"
#include <gtest/gtest.h>

using namespace trade_bot;

class DensityDetectorTest : public ::testing::Test {
protected:
    void SetUp() override { Logger::init(); }
    
    FeatureFrame make_frame(Ticker t, std::chrono::system_clock::time_point ts) {
        FeatureFrame f;
        f.ticker = std::move(t);
        f.timestamp = ts;
        f.valid = true;
        return f;
    }
    
    DensityDetector::Config make_cfg() {
        DensityDetector::Config cfg;
        cfg.min_size_vs_avg = 1.0;
        cfg.min_size_usd = 100.0; // low for tests
        cfg.min_distance_bps = 1.0;
        return cfg;
    }
};

TEST_F(DensityDetectorTest, DetectsStickyDensity) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    auto cfg = make_cfg();
    cfg.sticky_duration = std::chrono::milliseconds(2000);
    
    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int signals = 0;
    bus.subscribe([&signals](const Signal& s){
        if (s.kind == SignalKind::DensityDetected) signals++;
    });
    
    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT", {{100.10, 1.0, Side::Sell}}, {{100.00, 1.0, Side::Buy}}, now});
    
    detector.on_book_update({"BTCUSDT", {{100.50, 100.0, Side::Sell}}, now});
    
    detector.on_frame(make_frame("BTCUSDT", now + std::chrono::seconds(1)));
    EXPECT_EQ(signals, 0);
    
    detector.on_frame(make_frame("BTCUSDT", now + std::chrono::seconds(3)));
    EXPECT_EQ(signals, 1);
}

TEST_F(DensityDetectorTest, DetectsFakeDensityRemoval) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    auto cfg = make_cfg();
    cfg.fake_threshold = std::chrono::milliseconds(300);
    
    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int fake_signals = 0;
    bus.subscribe([&fake_signals](const Signal& s){
        if (s.kind == SignalKind::DensityRemoved) {
            if (s.payload.fake) fake_signals++;
        }
    });
    
    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT", {{100.10, 1.0, Side::Sell}}, {{100.00, 1.0, Side::Buy}}, now});
    
    detector.on_book_update({"BTCUSDT", {{100.50, 100.0, Side::Sell}}, now});
    detector.on_book_update({"BTCUSDT", {{100.50, 0.0, Side::Sell}}, now + std::chrono::milliseconds(100)});
    
    EXPECT_EQ(fake_signals, 1);
}

TEST_F(DensityDetectorTest, DetectsDensityEating) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;
    
    auto cfg = make_cfg();
    cfg.eating_ratio_threshold = 0.5;
    cfg.eating_min_prints = 2;
    
    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    
    int eating_signals = 0;
    double last_eaten_ratio = 0.0;
    double last_remaining_ratio = 0.0;
    bus.subscribe([&](const Signal& s){
        if (s.kind == SignalKind::DensityEating) {
            eating_signals++;
            last_eaten_ratio = s.payload.eaten_ratio;
            last_remaining_ratio = s.payload.remaining_ratio;
        }
    });
    
    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT", {{100.10, 1.0, Side::Sell}}, {{100.00, 1.0, Side::Buy}}, now});
    
    // Track Ask density of 10.0 at 100.50
    detector.on_book_update({"BTCUSDT", {{100.50, 10.0, Side::Sell}}, now});
    
    // Eat 3.0 + 3.0 = 6.0 → eaten_ratio = 0.6, remaining_ratio = 0.4
    detector.on_trade({100.50, 3.0, Side::Buy, now + std::chrono::milliseconds(100)});
    EXPECT_EQ(eating_signals, 0);
    
    detector.on_trade({100.50, 3.0, Side::Buy, now + std::chrono::milliseconds(200)});
    EXPECT_EQ(eating_signals, 1);
    // eaten_ratio should be 0.6 (6/10)
    EXPECT_DOUBLE_EQ(last_eaten_ratio, 0.6);
    // remaining_ratio should be 0.4 (4/10)
    EXPECT_DOUBLE_EQ(last_remaining_ratio, 0.4);
}

// Issue #116: trades arriving one tick off the tracked level should still
// register as eat-through, accommodating exchange rounding / micro-slippage.
TEST_F(DensityDetectorTest, EatingMatchesOnNeighbourTick) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;

    auto cfg = make_cfg();
    cfg.eating_ratio_threshold = 0.5;
    cfg.eating_min_prints = 2;

    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    int eating_signals = 0;
    bus.subscribe([&](const Signal& s) {
        if (s.kind == SignalKind::DensityEating) eating_signals++;
    });

    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT",
                         {{100.10, 1.0, Side::Sell}},
                         {{100.00, 1.0, Side::Buy}},
                         now});

    // Track an Ask density at 100.50.
    detector.on_book_update({"BTCUSDT", {{100.50, 10.0, Side::Sell}}, now});

    // Trades land at 100.49 (one tick BELOW) — must still count as eating.
    detector.on_trade({100.49, 3.0, Side::Buy, now + std::chrono::milliseconds(100)});
    EXPECT_EQ(eating_signals, 0);
    detector.on_trade({100.49, 3.0, Side::Buy, now + std::chrono::milliseconds(200)});
    EXPECT_EQ(eating_signals, 1);
}

TEST_F(DensityDetectorTest, EatingMatchesOnTickPlusOne) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;

    auto cfg = make_cfg();
    cfg.eating_ratio_threshold = 0.5;
    cfg.eating_min_prints = 2;

    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);
    int eating_signals = 0;
    bus.subscribe([&](const Signal& s) {
        if (s.kind == SignalKind::DensityEating) eating_signals++;
    });

    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT",
                         {{100.10, 1.0, Side::Sell}},
                         {{100.00, 1.0, Side::Buy}},
                         now});

    // Track a Bid density at 100.00.
    detector.on_book_update({"BTCUSDT", {{99.50, 10.0, Side::Buy}}, now});

    // Sell-aggressor trades land at 99.51 (one tick ABOVE the bid level).
    detector.on_trade({99.51, 3.0, Side::Sell, now + std::chrono::milliseconds(100)});
    detector.on_trade({99.51, 3.0, Side::Sell, now + std::chrono::milliseconds(200)});
    EXPECT_EQ(eating_signals, 1);
}

// ═══════════════════════════════════════════════════════════════
// FN-008: DensityStack tests
// ═══════════════════════════════════════════════════════════════

// Ask stack: two Ask density levels within 20 bps, total USD ≥ threshold.
TEST_F(DensityDetectorTest, DensityStackAskSide) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;

    auto cfg = make_cfg();
    cfg.sticky_duration = std::chrono::milliseconds(0); // emit immediately
    cfg.stack_min_levels = 2;
    cfg.stack_max_width_bps = 20.0;
    cfg.stack_min_size_usd = 100.0; // low for tests

    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);

    int stack_signals = 0;
    double captured_first = 0.0;
    double captured_last = 0.0;
    double captured_width = 0.0;
    double captured_total_usd = 0.0;
    double captured_stop_anchor = 0.0;
    FixedString<8> captured_side;

    bus.subscribe([&](const Signal& s) {
        if (s.kind == SignalKind::DensityStack) {
            stack_signals++;
            captured_first = s.payload.first_price;
            captured_last = s.payload.last_price;
            captured_width = s.payload.width_bps;
            captured_total_usd = s.payload.total_size_usd;
            captured_stop_anchor = s.payload.stop_anchor_price;
            captured_side = s.payload.side;
        }
    });

    auto now = std::chrono::system_clock::now();
    // Snapshot with mid at 100.00
    book.apply_snapshot({"BTCUSDT",
                         {{100.10, 1.0, Side::Sell}},
                         {{100.00, 1.0, Side::Buy}},
                         now});

    // Two Ask densities at 100.50 and 100.60 (within 20 bps of mid).
    detector.on_book_update({"BTCUSDT", {{100.50, 10.0, Side::Sell}}, now});
    detector.on_book_update({"BTCUSDT", {{100.60, 12.0, Side::Sell}}, now});

    // Trigger sticky check (frame advances past sticky_duration=0).
    auto frame = make_frame("BTCUSDT", now + std::chrono::milliseconds(50));
    detector.on_frame(frame);

    EXPECT_EQ(stack_signals, 1);
    EXPECT_EQ(std::string(captured_side), "Ask");
    // Ask: first_price = nearest (lower) = 100.50, last/stop = farthest (higher) = 100.60
    EXPECT_DOUBLE_EQ(captured_first, 100.50);
    EXPECT_DOUBLE_EQ(captured_last, 100.60);
    EXPECT_DOUBLE_EQ(captured_stop_anchor, 100.60);
    // width_bps ≈ |100.60 - 100.50| / 100.00 * 10000 = 10.0
    EXPECT_NEAR(captured_width, 10.0, 0.1);
    // total_usd = 10*100.50 + 12*100.60 = 1005.0 + 1207.2 = 2212.2
    EXPECT_GT(captured_total_usd, 2000.0);
}

// Bid stack: two Bid density levels within 20 bps, total USD ≥ threshold.
TEST_F(DensityDetectorTest, DensityStackBidSide) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;

    auto cfg = make_cfg();
    cfg.sticky_duration = std::chrono::milliseconds(0);
    cfg.stack_min_levels = 2;
    cfg.stack_max_width_bps = 20.0;
    cfg.stack_min_size_usd = 100.0;

    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);

    int stack_signals = 0;
    double captured_first = 0.0;
    double captured_last = 0.0;
    double captured_stop_anchor = 0.0;
    FixedString<8> captured_side;

    bus.subscribe([&](const Signal& s) {
        if (s.kind == SignalKind::DensityStack) {
            stack_signals++;
            captured_first = s.payload.first_price;
            captured_last = s.payload.last_price;
            captured_stop_anchor = s.payload.stop_anchor_price;
            captured_side = s.payload.side;
        }
    });

    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT",
                         {{100.10, 1.0, Side::Sell}},
                         {{100.00, 1.0, Side::Buy}},
                         now});

    // Two Bid densities at 99.50 and 99.40.
    detector.on_book_update({"BTCUSDT", {{99.50, 10.0, Side::Buy}}, now});
    detector.on_book_update({"BTCUSDT", {{99.40, 12.0, Side::Buy}}, now});

    auto frame = make_frame("BTCUSDT", now + std::chrono::milliseconds(50));
    detector.on_frame(frame);

    EXPECT_EQ(stack_signals, 1);
    EXPECT_EQ(std::string(captured_side), "Bid");
    // Bid: first_price = nearest (higher) = 99.50, stop_anchor = farthest (lower) = 99.40
    EXPECT_DOUBLE_EQ(captured_first, 99.50);
    EXPECT_DOUBLE_EQ(captured_last, 99.40);
    EXPECT_DOUBLE_EQ(captured_stop_anchor, 99.40);
}

// Duplicate suppression: same cluster should not emit DensityStack twice.
TEST_F(DensityDetectorTest, DensityStackDuplicateSuppression) {
    SignalBus bus;
    OrderBook book{"BTCUSDT", 0.01, 1e-6};
    TickerUniverse universe;

    auto cfg = make_cfg();
    cfg.sticky_duration = std::chrono::milliseconds(0);
    cfg.stack_min_levels = 2;
    cfg.stack_max_width_bps = 20.0;
    cfg.stack_min_size_usd = 100.0;

    DensityDetector detector("BTCUSDT", bus, book, universe, cfg);

    int stack_signals = 0;
    bus.subscribe([&](const Signal& s) {
        if (s.kind == SignalKind::DensityStack) stack_signals++;
    });

    auto now = std::chrono::system_clock::now();
    book.apply_snapshot({"BTCUSDT",
                         {{100.10, 1.0, Side::Sell}},
                         {{100.00, 1.0, Side::Buy}},
                         now});

    detector.on_book_update({"BTCUSDT", {{100.50, 10.0, Side::Sell}}, now});
    detector.on_book_update({"BTCUSDT", {{100.60, 12.0, Side::Sell}}, now});

    // First frame → should emit one stack.
    detector.on_frame(make_frame("BTCUSDT", now + std::chrono::milliseconds(50)));
    EXPECT_EQ(stack_signals, 1);

    // Second frame with same levels → should NOT emit again.
    detector.on_frame(make_frame("BTCUSDT", now + std::chrono::milliseconds(150)));
    EXPECT_EQ(stack_signals, 1);

    // Third frame → still one.
    detector.on_frame(make_frame("BTCUSDT", now + std::chrono::milliseconds(250)));
    EXPECT_EQ(stack_signals, 1);
}
