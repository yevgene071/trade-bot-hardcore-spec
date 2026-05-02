#include "risk/TradingDay.hpp"
#include "risk/AccountStatePersister.hpp"
#include <gtest/gtest.h>
#include <filesystem>

using namespace trade_bot;

TEST(TradingDayResetTest, IdempotentReset) {
    const std::string path = "test_account_state.json";
    if (std::filesystem::exists(path)) std::filesystem::remove(path);
    
    AccountStatePersister persister(path);
    std::string today = TradingDay::current_date_utc();
    
    // 1. Initial state (saved yesterday)
    AccountStatePersister::PersistedData data;
    data.last_reset_day_utc = "2020-01-01";
    data.account_state.starting_equity_usd = 5000.0;
    persister.save(data);
    
    // 2. Load and check if reset needed
    auto loaded = persister.load();
    ASSERT_TRUE(loaded.has_value());
    
    if (TradingDay::is_new_day(loaded->last_reset_day_utc)) {
        loaded->account_state.starting_equity_usd = 10000.0; // simulated reset
        loaded->last_reset_day_utc = today;
        persister.save(*loaded);
    }
    
    // 3. Second run (simulate restart today)
    auto loaded2 = persister.load();
    ASSERT_TRUE(loaded2.has_value());
    EXPECT_EQ(loaded2->last_reset_day_utc, today);
    EXPECT_DOUBLE_EQ(loaded2->account_state.starting_equity_usd, 10000.0);
    
    if (TradingDay::is_new_day(loaded2->last_reset_day_utc)) {
        // Should not enter here
        loaded2->account_state.starting_equity_usd = 99999.0;
        persister.save(*loaded2);
    }
    
    auto loaded3 = persister.load();
    EXPECT_DOUBLE_EQ(loaded3->account_state.starting_equity_usd, 10000.0);
    
    std::filesystem::remove(path);
}
