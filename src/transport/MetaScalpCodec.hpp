#pragma once

#include "domain/Types.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

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
    static OrderUpdate parse_order_update(const nlohmann::json& j);
    static RestOrder parse_rest_order(const nlohmann::json& j);
    static PlaceOrderResult parse_place_order_result(const nlohmann::json& j);
    static PositionUpdate parse_position_update(const nlohmann::json& j);
    static BalanceUpdate parse_balance_update(const nlohmann::json& j);
    static std::vector<Trade> parse_trade_update(const nlohmann::json& j);
    static OrderBookSnapshot parse_orderbook_snapshot(const nlohmann::json& j, const Ticker& ticker);
    static OrderBookUpdate parse_orderbook_update(const nlohmann::json& j, const Ticker& ticker);
    static ClusterSnapshot parse_cluster_snapshot(const nlohmann::json& j);
    static Notification    parse_notification(const nlohmann::json& j);
    static SignalLevel     parse_signal_level(const nlohmann::json& j);
    static OrderbookSettings parse_orderbook_settings(const nlohmann::json& j);
    static FinresUpdate parse_finres_update(const nlohmann::json& j);
    static SignalLevelTriggered parse_signal_level_triggered(const nlohmann::json& j);
    
    static ConnectionInfo parse_connection_info(const nlohmann::json& j);
    static TickerInfo parse_ticker_info(const nlohmann::json& j);

    static Side parse_side(const nlohmann::json& v);
    static OrderType parse_order_type(const nlohmann::json& v);
    static OrderStatus parse_order_status(const nlohmann::json& v);
    static PositionStatus parse_position_status(const nlohmann::json& v);
    static std::chrono::system_clock::time_point parse_timestamp(const std::string& ts_str);

private:
    static void check_required(const nlohmann::json& j, const std::string& field);
};

} // namespace trade_bot
