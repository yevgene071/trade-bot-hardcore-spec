#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <cstdint>

namespace trade_bot {

using Ticker = std::string;

constexpr double kBpsBase = 10000.0;

enum class Side {
    None,
    Buy,
    Sell
};

enum class OrderType {
    Limit,
    Stop,
    StopLoss,
    TakeProfit,
    Market
};

enum class OrderStatus {
    New,
    Open,
    Closed
};

enum class PositionStatus {
    New,
    Open,
    Closed
};

struct PriceLevel {
    double price;
    double size;
    Side side;

    bool operator==(const PriceLevel&) const = default;
};

struct Trade {
    double price;
    double size;
    Side side;
    std::chrono::system_clock::time_point timestamp;

    bool operator==(const Trade&) const = default;
};

struct OrderBookSnapshot {
    Ticker ticker;
    std::vector<PriceLevel> asks;
    std::vector<PriceLevel> bids;
    std::chrono::system_clock::time_point ts;

    bool operator==(const OrderBookSnapshot&) const = default;
};

struct OrderBookUpdate {
    Ticker ticker;
    std::vector<PriceLevel> changes;
    std::chrono::system_clock::time_point ts;

    bool operator==(const OrderBookUpdate&) const = default;
};

struct OrderUpdate {
    int64_t order_id;
    Ticker ticker;
    Side side;
    OrderType type;
    double price;
    double filled_price;
    double size;
    double filled_size;
    double fee;
    std::string fee_currency;
    OrderStatus status;
    std::chrono::system_clock::time_point time;

    bool operator==(const OrderUpdate&) const = default;
};

struct RestOrder {
    int64_t id;
    std::optional<std::string> client_id;
    Ticker ticker;
    Side side;
    OrderType type;
    double price;
    double size;
    double filled_size;
    double filled_price;
    double remaining_size;
    OrderStatus status;
    std::optional<double> trigger_price;
    std::chrono::system_clock::time_point create_date;

    bool operator==(const RestOrder&) const = default;
};

struct PlaceOrderResult {
    std::string status;
    std::string client_id;
    double execution_time_ms;

    bool operator==(const PlaceOrderResult&) const = default;
};

struct PositionUpdate {
    int connection_id;
    int64_t position_id;
    Ticker ticker;
    Side side;
    double size;
    double avg_price;
    double avg_price_fix;
    double avg_price_dyn;
    PositionStatus status;

    bool operator==(const PositionUpdate&) const = default;
};

struct BalanceEntry {
    std::string coin;
    double total;
    double free;
    double locked;

    bool operator==(const BalanceEntry&) const = default;
};

struct BalanceUpdate {
    int connection_id;
    std::vector<BalanceEntry> balances;

    bool operator==(const BalanceUpdate&) const = default;
};

struct FinresEntry {
    std::string currency;
    double result;
    double fee;
    double funds;
    double available;
    double blocked;
    
    bool operator==(const FinresEntry&) const = default;
};

struct FinresUpdate {
    int connection_id;
    std::vector<FinresEntry> finreses;
    
    bool operator==(const FinresUpdate&) const = default;
};

struct SignalLevelTriggered {
    Ticker ticker;
    double price;
    std::chrono::system_clock::time_point timestamp;
    
    bool operator==(const SignalLevelTriggered&) const = default;
};

struct ClusterItem {
    double price;
    double ask_size;
    double bid_size;
    
    bool operator==(const ClusterItem&) const = default;
};

struct ClusterSnapshot {
    Ticker ticker;
    std::string timeframe;
    int zoom_index;
    std::vector<ClusterItem> items;
    std::chrono::system_clock::time_point ts;
    
    bool operator==(const ClusterSnapshot&) const = default;
};

enum class NotificationKind {
    Trade,
    SignalLevel,
    BigOrderBookAmount,
    BigOrderBookAmount2,
    BigTick,
    ScreenerNewCoin
};

struct Notification {
    NotificationKind kind;
    int exchange_id;
    int market_type;
    Ticker ticker;
    double price;
    double size;
    std::chrono::system_clock::time_point timestamp;
    
    bool operator==(const Notification&) const = default;
};

struct OrderbookSettings {
    Ticker ticker;
    double large_amount_usd;
    double large_amount_usd2;
    
    bool operator==(const OrderbookSettings&) const = default;
};

struct SignalLevel {
    int id;
    Ticker ticker;
    double price;
    bool triggered;
    std::chrono::system_clock::time_point created_at;
};

struct ConnectionInfo {
    int id;
    std::string name;
    std::string state;
    bool view_mode;
    
    bool operator==(const ConnectionInfo&) const = default;
};

struct TickerInfo {
    Ticker name;
    std::string base_asset;
    std::string quote_asset;
    bool is_trading_allowed;
    double price_increment;
    double size_increment;
    double min_size;
    double max_size;
    
    bool operator==(const TickerInfo&) const = default;
};

struct PlaceOrderRequest {
    Ticker ticker;
    Side side;
    double price;
    double size;
    OrderType type;
    bool reduce_only = false;
    
    bool operator==(const PlaceOrderRequest&) const = default;
};

} // namespace trade_bot
