#pragma once

#include "domain/Types.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <cctype>
#include <expected>
#include <string>

namespace trade_bot {

class CodecError : public std::runtime_error {
public:
    explicit CodecError(const std::string& msg) : std::runtime_error(msg) {}
};

namespace api::fields {
    inline constexpr auto kConnectionId = "ConnectionId";
    inline constexpr auto kTicker = "Ticker";
    inline constexpr auto kOrderId = "OrderId";
    inline constexpr auto kSide = "Side";
    inline constexpr auto kType = "Type";
    inline constexpr auto kPrice = "Price";
    inline constexpr auto kFilledPrice = "FilledPrice";
    inline constexpr auto kSize = "Size";
    inline constexpr auto kFilledSize = "FilledSize";
    inline constexpr auto kFee = "Fee";
    inline constexpr auto kFeeCurrency = "FeeCurrency";
    inline constexpr auto kStatus = "Status";
    inline constexpr auto kTime = "Time";
    inline constexpr auto kClientId = "ClientId";
    inline constexpr auto kId = "Id";
    inline constexpr auto kRemainingSize = "RemainingSize";
    inline constexpr auto kTriggerPrice = "TriggerPrice";
    inline constexpr auto kCreateDate = "CreateDate";
    inline constexpr auto kAvgPrice = "AvgPrice";
    inline constexpr auto kAvgPriceFix = "AvgPriceFix";
    inline constexpr auto kAvgPriceDyn = "AvgPriceDyn";
    inline constexpr auto kPositionId = "PositionId";
    inline constexpr auto kCoin = "Coin";
    inline constexpr auto kTotal = "Total";
    inline constexpr auto kFree = "Free";
    inline constexpr auto kLocked = "Locked";
    inline constexpr auto kBalances = "Balances";
    inline constexpr auto kTrades = "Trades";
    inline constexpr auto kAsks = "Asks";
    inline constexpr auto kBids = "Bids";
    inline constexpr auto kUpdates = "Updates";
    inline constexpr auto kExecutionTimeMs = "ExecutionTimeMs";
    inline constexpr auto kFinreses = "Finreses";
    inline constexpr auto kCurrency = "Currency";
    inline constexpr auto kResult = "Result";
    inline constexpr auto kFunds = "Funds";
    inline constexpr auto kAvailable = "Available";
    inline constexpr auto kBlocked = "Blocked";
    inline constexpr auto kBestAsk = "BestAsk";
    inline constexpr auto kBestBid = "BestBid";
    inline constexpr auto kName = "Name";
    inline constexpr auto kState = "State";
    inline constexpr auto kViewMode = "ViewMode";
    inline constexpr auto kBaseAsset = "BaseAsset";
    inline constexpr auto kQuoteAsset = "QuoteAsset";
    inline constexpr auto kIsTradingAllowed = "IsTradingAllowed";
    inline constexpr auto kPriceIncrement = "PriceIncrement";
    inline constexpr auto kSizeIncrement = "SizeIncrement";
    inline constexpr auto kMinSize = "MinSize";
    inline constexpr auto kMaxSize = "MaxSize";
    inline constexpr auto kReduceOnly = "ReduceOnly";
    inline constexpr auto kItems = "Items";
    inline constexpr auto kTimeFrame = "TimeFrame";
    inline constexpr auto kZoomIndex = "ZoomIndex";
    inline constexpr auto kAskSize = "AskSize";
    inline constexpr auto kBidSize = "BidSize";
    inline constexpr auto kKind = "Kind";
    inline constexpr auto kExchangeId = "ExchangeId";
    inline constexpr auto kMarketType = "MarketType";
    inline constexpr auto kDate = "Date";
    inline constexpr auto kLargeAmountUsd = "LargeAmountUsd";
    inline constexpr auto kLargeAmountUsd2 = "LargeAmountUsd2";
    inline constexpr auto kTriggered = "Triggered";
}

class MetaScalpCodec {
public:
    static std::expected<OrderUpdate, std::string> parse_order_update(const nlohmann::json& j);
    static RestOrder parse_rest_order(const nlohmann::json& j);
    static PlaceOrderResult parse_place_order_result(const nlohmann::json& j);
    static std::expected<PositionUpdate, std::string> parse_position_update(const nlohmann::json& j);
    static std::expected<BalanceUpdate, std::string> parse_balance_update(const nlohmann::json& j);
    static std::expected<std::vector<Trade>, std::string> parse_trade_update(const nlohmann::json& j);
    static std::expected<OrderBookSnapshot, std::string> parse_orderbook_snapshot(const nlohmann::json& j, const Ticker& ticker);
    static std::expected<OrderBookUpdate, std::string> parse_orderbook_update(const nlohmann::json& j, const Ticker& ticker);
    static ClusterSnapshot parse_cluster_snapshot(const nlohmann::json& j);
    static Notification    parse_notification(const nlohmann::json& j);
    static SignalLevel     parse_signal_level(const nlohmann::json& j);
    static OrderbookSettings parse_orderbook_settings(const nlohmann::json& j);

    static std::expected<FinresUpdate, std::string> parse_finres_update(const nlohmann::json& j);
    static SignalLevelTriggered parse_signal_level_triggered(const nlohmann::json& j);
    static NotificationKind parse_notification_type(const std::string& type_str);
    static int parse_market_type(const nlohmann::json& v);
    
    static ConnectionInfo parse_connection_info(const nlohmann::json& j);
    static TickerInfo parse_ticker_info(const nlohmann::json& j);

    /// Normalize ticker to underscore format (e.g. "BTCUSDT" → "BTC_USDT").
    /// Ensures consistency between REST API ticker names and WebSocket notification tickers.
    static Ticker normalize_ticker(const Ticker& raw);

    static Side parse_side(const nlohmann::json& v);

    static OrderType parse_order_type(const nlohmann::json& v);
    static OrderStatus parse_order_status(const nlohmann::json& v);
    static PositionStatus parse_position_status(const nlohmann::json& v);
    static std::chrono::system_clock::time_point parse_timestamp(const std::string& ts_str);
    static std::chrono::system_clock::time_point parse_iso8601(const std::string& s) { return parse_timestamp(s); }

    template<typename T>
    static T get_val(const nlohmann::json& j, const std::string& key, T default_val = T()) {
        auto get_from = [&](const std::string& k) -> T {
            const auto it = j.find(k);
            if (it != j.end() && !it->is_null()) {
                try {
                    return it->get<T>();
                } catch (...) {
                    return default_val;
                }
            }
            return default_val;
        };

        if (j.contains(key)) return get_from(key);
        std::string alt = key;
        if (!alt.empty()) {
            if (std::isupper(alt[0])) alt[0] = std::tolower(alt[0]);
            else alt[0] = std::toupper(alt[0]);
            if (j.contains(alt)) return get_from(alt);
        }
        return default_val;
    }

private:
    /// Returns true if `field` (or its alternate-case variant) is present.
    static bool has_required(const nlohmann::json& j, std::string_view field);
    /// Throws CodecError if required field is missing.
    static void check_required(const nlohmann::json& j, std::string_view field);
    /// Non-throwing OrderType parse for use inside std::expected parsers.
    static std::expected<OrderType, std::string> parse_order_type_checked(const nlohmann::json& v);
};

} // namespace trade_bot
