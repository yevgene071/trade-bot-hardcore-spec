#include <gtest/gtest.h>
#include "transport/MetaScalpCodec.hpp"
#include <nlohmann/json.hpp>

using namespace trade_bot;

// T1-NOTIF: Тест парсинга уведомлений с разным регистром и нормализацией тикеров
TEST(CodecNotificationTest, CaseInsensitiveAndTickerNormalization) {
    // 1. Тест ScreenerNewCoin с CamelCase и тикером без подчеркивания (MEXC style)
    nlohmann::json j1 = {
        {"type", "ScreenerNewCoin"},
        {"ticker", "BTCUSDT"},
        {"exchangeId", 8},
        {"price", 60000.0},
        {"size", 1.0},
        {"date", "2026-05-12T20:00:00.000Z"}
    };
    
    auto n1 = MetaScalpCodec::parse_notification(j1);
    EXPECT_EQ(n1.kind, NotificationKind::ScreenerNewCoin);
    EXPECT_EQ(n1.ticker, "BTC_USDT"); // Должно быть нормализовано
    EXPECT_EQ(n1.exchange_id, 8);
}

TEST(CodecNotificationTest, LowercaseTypeParsing) {
    // 2. Тест полностью строчных значений типа уведомления
    nlohmann::json j2 = {
        {"type", "bigtick"},
        {"ticker", "ETH_USDT"},
        {"exchangeId", 8},
        {"price", 3000.0},
        {"size", 10.0}
    };
    
    auto n2 = MetaScalpCodec::parse_notification(j2);
    EXPECT_EQ(n2.kind, NotificationKind::BigTick);
    EXPECT_EQ(n2.ticker, "ETH_USDT");
}

TEST(CodecNotificationTest, ScreenerShortType) {
    // 3. Тест сокращенного типа "screener"
    nlohmann::json j3 = {
        {"type", "screener"},
        {"ticker", "SOLUSDC"},
        {"exchangeId", 8}
    };
    
    auto n3 = MetaScalpCodec::parse_notification(j3);
    EXPECT_EQ(n3.kind, NotificationKind::ScreenerNewCoin);
    EXPECT_EQ(n3.ticker, "SOL_USDC"); // Нормализация для USDC
}

TEST(CodecNotificationTest, NotificationSnapshotCaseMapping) {
    // 4. Тест парсинга обертки в NotificationFeed (имитация через JSON)
    // В самом коде NotificationFeed::handle_message_ используется поиск ключа.
    // Мы проверим это через косвенный тест, если бы мы вызывали парсер для элементов массива.
    
    nlohmann::json snapshot = {
        {"Type", "notification_snapshot"},
        {"Data", {
            {"notifications", { // Строчный регистр ключа
                {
                    {"type", "BigOrderBookAmount"},
                    {"ticker", "XRPUSDT"},
                    {"exchangeId", 8}
                }
            }}
        }}
    };
    
    const auto& data = snapshot["Data"];
    const nlohmann::json* n_ptr = nullptr;
    if (data.contains("Notifications")) n_ptr = &data["Notifications"];
    else if (data.contains("notifications")) n_ptr = &data["notifications"];
    
    ASSERT_NE(n_ptr, nullptr);
    ASSERT_TRUE(n_ptr->is_array());
    
    auto n = MetaScalpCodec::parse_notification((*n_ptr)[0]);
    EXPECT_EQ(n.kind, NotificationKind::BigOrderBookAmount);
    EXPECT_EQ(n.ticker, "XRP_USDT");
}
