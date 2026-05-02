#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <iomanip>
#include <sstream>

namespace trade_bot {

using namespace api;

void MetaScalpCodec::check_required(const nlohmann::json& j, const std::string& field) {
    if (!j.contains(field)) {
        throw CodecError("Missing required field: " + field);
    }
}

Side MetaScalpCodec::parse_side(const nlohmann::json& v) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s == "Buy") return Side::Buy;
        if (s == "Sell") return Side::Sell;
    } else if (v.is_number()) {
        int i = v.get<int>();
        if (i == 1) return Side::Buy;
        if (i == 2) return Side::Sell;
    }
    return Side::None;
}

OrderType MetaScalpCodec::parse_order_type(const nlohmann::json& v) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s == "Limit") return OrderType::Limit;
        if (s == "Stop") return OrderType::Stop;
        if (s == "StopLoss") return OrderType::StopLoss;
        if (s == "TakeProfit") return OrderType::TakeProfit;
        if (s == "Market") return OrderType::Market;
    } else if (v.is_number()) {
        int i = v.get<int>();
        switch (i) {
            case 0: return OrderType::Limit;
            case 1: return OrderType::Stop;
            case 2: return OrderType::StopLoss;
            case 3: return OrderType::TakeProfit;
            case 4: return OrderType::Market;
        }
    }
    throw CodecError("Unknown OrderType: " + v.dump());
}

OrderStatus MetaScalpCodec::parse_order_status(const nlohmann::json& v) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s == "New") return OrderStatus::New;
        if (s == "Open") return OrderStatus::Open;
        if (s == "Closed") return OrderStatus::Closed;
    } else if (v.is_number()) {
        int i = v.get<int>();
        switch (i) {
            case 0: return OrderStatus::New;
            case 1: return OrderStatus::Open;
            case 2: return OrderStatus::Closed;
        }
    }
    return OrderStatus::New;
}

PositionStatus MetaScalpCodec::parse_position_status(const nlohmann::json& v) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s == "New") return PositionStatus::New;
        if (s == "Open") return PositionStatus::Open;
        if (s == "Closed") return PositionStatus::Closed;
    } else if (v.is_number()) {
        int i = v.get<int>();
        switch (i) {
            case 0: return PositionStatus::New;
            case 1: return PositionStatus::Open;
            case 2: return PositionStatus::Closed;
        }
    }
    return PositionStatus::New;
}

std::chrono::system_clock::time_point MetaScalpCodec::parse_timestamp(const std::string& ts_str) {
    std::tm tm = {};
    std::stringstream ss(ts_str);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    
    auto tp = std::chrono::system_clock::from_time_t(timegm(&tm));
    
    size_t dot = ts_str.find('.');
    if (dot != std::string::npos) {
        try {
            int ms = std::stoi(ts_str.substr(dot + 1, 3));
            tp += std::chrono::milliseconds(ms);
        } catch (...) {}
    }
    
    return tp;
}

OrderUpdate MetaScalpCodec::parse_order_update(const nlohmann::json& j) {
    check_required(j, fields::kOrderId);
    check_required(j, fields::kTicker);
    
    return OrderUpdate {
        .order_id = j.value(fields::kOrderId, 0L),
        .ticker = j.value(fields::kTicker, ""),
        .side = parse_side(j.value(fields::kSide, nlohmann::json())),
        .type = parse_order_type(j.value(fields::kType, nlohmann::json())),
        .price = j.value(fields::kPrice, 0.0),
        .filled_price = j.value(fields::kFilledPrice, 0.0),
        .size = j.value(fields::kSize, 0.0),
        .filled_size = j.value(fields::kFilledSize, 0.0),
        .fee = j.value(fields::kFee, 0.0),
        .fee_currency = j.value(fields::kFeeCurrency, ""),
        .status = parse_order_status(j.value(fields::kStatus, nlohmann::json())),
        .time = parse_timestamp(j.value(fields::kTime, ""))
    };
}

RestOrder MetaScalpCodec::parse_rest_order(const nlohmann::json& j) {
    check_required(j, fields::kId);
    check_required(j, fields::kTicker);

    std::optional<std::string> client_id;
    if (j.contains(fields::kClientId) && !j[fields::kClientId].is_null()) {
        client_id = j[fields::kClientId].get<std::string>();
    }

    std::optional<double> trigger_price;
    if (j.contains(fields::kTriggerPrice) && !j[fields::kTriggerPrice].is_null()) {
        trigger_price = j[fields::kTriggerPrice].get<double>();
    }

    return RestOrder {
        .id = j.value(fields::kId, 0L),
        .client_id = client_id,
        .ticker = j.value(fields::kTicker, ""),
        .side = parse_side(j.value(fields::kSide, nlohmann::json())),
        .type = parse_order_type(j.value(fields::kType, nlohmann::json())),
        .price = j.value(fields::kPrice, 0.0),
        .size = j.value(fields::kSize, 0.0),
        .filled_size = j.value(fields::kFilledSize, 0.0),
        .filled_price = j.value(fields::kFilledPrice, 0.0),
        .remaining_size = j.value(fields::kRemainingSize, 0.0),
        .status = parse_order_status(j.value(fields::kStatus, nlohmann::json())),
        .trigger_price = trigger_price,
        .create_date = parse_timestamp(j.value(fields::kCreateDate, ""))
    };
}

PlaceOrderResult MetaScalpCodec::parse_place_order_result(const nlohmann::json& j) {
    check_required(j, fields::kStatus);
    return PlaceOrderResult {
        .status = j.value(fields::kStatus, ""),
        .client_id = j.value(fields::kClientId, ""),
        .execution_time_ms = j.value(fields::kExecutionTimeMs, 0.0)
    };
}

PositionUpdate MetaScalpCodec::parse_position_update(const nlohmann::json& j) {
    check_required(j, fields::kPositionId);
    check_required(j, fields::kTicker);
    
    return PositionUpdate {
        .connection_id = j.value(fields::kConnectionId, 0),
        .position_id = j.value(fields::kPositionId, 0L),
        .ticker = j.value(fields::kTicker, ""),
        .side = parse_side(j.value(fields::kSide, nlohmann::json())),
        .size = j.value(fields::kSize, 0.0),
        .avg_price = j.value(fields::kAvgPrice, 0.0),
        .avg_price_fix = j.value(fields::kAvgPriceFix, 0.0),
        .avg_price_dyn = j.value(fields::kAvgPriceDyn, 0.0),
        .status = parse_position_status(j.value(fields::kStatus, nlohmann::json()))
    };
}

BalanceUpdate MetaScalpCodec::parse_balance_update(const nlohmann::json& j) {
    std::vector<BalanceEntry> balances;
    if (j.contains(fields::kBalances) && j[fields::kBalances].is_array()) {
        for (const auto& item : j[fields::kBalances]) {
            balances.push_back({
                .coin = item.value(fields::kCoin, ""),
                .total = item.value(fields::kTotal, 0.0),
                .free = item.value(fields::kFree, 0.0),
                .locked = item.value(fields::kLocked, 0.0)
            });
        }
    }
    return BalanceUpdate {
        .connection_id = j.value(fields::kConnectionId, 0),
        .balances = balances
    };
}

std::vector<Trade> MetaScalpCodec::parse_trade_update(const nlohmann::json& j) {
    std::vector<Trade> trades;
    if (j.contains(fields::kTrades) && j[fields::kTrades].is_array()) {
        for (const auto& item : j[fields::kTrades]) {
            trades.push_back({
                .price = item.value(fields::kPrice, 0.0),
                .size = item.value(fields::kSize, 0.0),
                .side = parse_side(item.value(fields::kSide, nlohmann::json())),
                .timestamp = parse_timestamp(item.value(fields::kTime, ""))
            });
        }
    }
    return trades;
}

OrderBookSnapshot MetaScalpCodec::parse_orderbook_snapshot(const nlohmann::json& j, const Ticker& ticker) {
    std::vector<PriceLevel> asks, bids;
    if (j.contains(fields::kAsks) && j[fields::kAsks].is_array()) {
        for (const auto& item : j[fields::kAsks]) {
            asks.push_back({item.value(fields::kPrice, 0.0), item.value(fields::kSize, 0.0), Side::Sell});
        }
    }
    if (j.contains(fields::kBids) && j[fields::kBids].is_array()) {
        for (const auto& item : j[fields::kBids]) {
            bids.push_back({item.value(fields::kPrice, 0.0), item.value(fields::kSize, 0.0), Side::Buy});
        }
    }
    return OrderBookSnapshot {
        .ticker = ticker,
        .asks = asks,
        .bids = bids,
        .ts = std::chrono::system_clock::now() // MetaScalp snapshots usually don't have server ts in payload
    };
}

OrderBookUpdate MetaScalpCodec::parse_orderbook_update(const nlohmann::json& j, const Ticker& ticker) {
    std::vector<PriceLevel> changes;
    if (j.contains(fields::kUpdates) && j[fields::kUpdates].is_array()) {
        for (const auto& item : j[fields::kUpdates]) {
            Side side = parse_side(item.value(fields::kSide, nlohmann::json()));
            changes.push_back({item.value(fields::kPrice, 0.0), item.value(fields::kSize, 0.0), side});
        }
    }
    return OrderBookUpdate {
        .ticker = ticker,
        .changes = changes,
        .ts = std::chrono::system_clock::now()
    };
}

ClusterSnapshot MetaScalpCodec::parse_cluster_snapshot(const nlohmann::json& j) {
    check_required(j, fields::kTicker);
    check_required(j, fields::kItems);

    std::vector<ClusterItem> items;
    if (j.contains(fields::kItems) && j[fields::kItems].is_array()) {
        for (const auto& item : j[fields::kItems]) {
            items.push_back({
                .price = item.value(fields::kPrice, 0.0),
                .ask_size = item.value(fields::kAskSize, 0.0),
                .bid_size = item.value(fields::kBidSize, 0.0)
            });
        }
    }

    return ClusterSnapshot {
        .ticker = j.value(fields::kTicker, ""),
        .timeframe = j.value(fields::kTimeFrame, ""),
        .zoom_index = j.value(fields::kZoomIndex, 0),
        .items = items,
        .ts = std::chrono::system_clock::now()
    };
}

Notification MetaScalpCodec::parse_notification(const nlohmann::json& j) {
    const std::string type_str = j.value("Type", "");
    NotificationKind kind;
    if (type_str == "Trade") kind = NotificationKind::Trade;
    else if (type_str == "SignalLevel") kind = NotificationKind::SignalLevel;
    else if (type_str == "BigOrderBookAmount") kind = NotificationKind::BigOrderBookAmount;
    else if (type_str == "BigOrderBookAmount2") kind = NotificationKind::BigOrderBookAmount2;
    else if (type_str == "BigTick") kind = NotificationKind::BigTick;
    else if (type_str == "ScreenerNewCoin") kind = NotificationKind::ScreenerNewCoin;
    else throw CodecError("Unknown Notification type: " + type_str);

    return Notification {
        .kind = kind,
        .exchange_id = j.value(fields::kExchangeId, 0),
        .market_type = j.value(fields::kMarketType, 0),
        .ticker = j.value(fields::kTicker, ""),
        .price = j.value(fields::kPrice, 0.0),
        .size = j.value(fields::kSize, 0.0),
        .timestamp = parse_timestamp(j.value(fields::kDate, ""))
    };
}

FinresUpdate MetaScalpCodec::parse_finres_update(const nlohmann::json& j) {
    std::vector<FinresEntry> finreses;
    if (j.contains(fields::kFinreses) && j[fields::kFinreses].is_array()) {
        for (const auto& item : j[fields::kFinreses]) {
            finreses.push_back({
                .currency = item.value(fields::kCurrency, ""),
                .result = item.value(fields::kResult, 0.0),
                .fee = item.value(fields::kFee, 0.0),
                .funds = item.value(fields::kFunds, 0.0),
                .available = item.value(fields::kAvailable, 0.0),
                .blocked = item.value(fields::kBlocked, 0.0)
            });
        }
    }
    return FinresUpdate {
        .connection_id = j.value(fields::kConnectionId, 0),
        .finreses = finreses
    };
}

SignalLevelTriggered MetaScalpCodec::parse_signal_level_triggered(const nlohmann::json& j) {
    check_required(j, fields::kTicker);
    check_required(j, fields::kPrice);
    
    return SignalLevelTriggered {
        .ticker = j.value(fields::kTicker, ""),
        .price = j.value(fields::kPrice, 0.0),
        .timestamp = parse_timestamp(j.value(fields::kTime, ""))
    };
}

ConnectionInfo MetaScalpCodec::parse_connection_info(const nlohmann::json& j) {
    check_required(j, fields::kId);
    return ConnectionInfo {
        .id = j.value(fields::kId, 0),
        .name = j.value(fields::kName, ""),
        .state = j.value(fields::kState, ""),
        .view_mode = j.value(fields::kViewMode, false)
    };
}

TickerInfo MetaScalpCodec::parse_ticker_info(const nlohmann::json& j) {
    check_required(j, fields::kName);
    return TickerInfo {
        .name = j.value(fields::kName, ""),
        .base_asset = j.value(fields::kBaseAsset, ""),
        .quote_asset = j.value(fields::kQuoteAsset, ""),
        .is_trading_allowed = j.value(fields::kIsTradingAllowed, false),
        .price_increment = j.value(fields::kPriceIncrement, 0.0),
        .size_increment = j.value(fields::kSizeIncrement, 0.0),
        .min_size = j.value(fields::kMinSize, 0.0),
        .max_size = j.value(fields::kMaxSize, 0.0)
    };
}

} // namespace trade_bot
