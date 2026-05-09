#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <iomanip>
#include <sstream>
#include <algorithm>

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
    if (ts_str.size() < 19) [[unlikely]] return {};

    // Faster than stringstream + std::get_time
    auto p2 = [](const char* p) { 
        return static_cast<int>((p[0] - '0') * 10 + (p[1] - '0')); 
    };
    
    std::tm tm = {};
    try {
        tm.tm_year = ((ts_str[0] - '0') * 1000 + (ts_str[1] - '0') * 100 + (ts_str[2] - '0') * 10 + (ts_str[3] - '0')) - 1900;
        tm.tm_mon  = p2(&ts_str[5]) - 1;
        tm.tm_mday = p2(&ts_str[8]);
        tm.tm_hour = p2(&ts_str[11]);
        tm.tm_min  = p2(&ts_str[14]);
        tm.tm_sec  = p2(&ts_str[17]);
    } catch (...) {
        return {};
    }
    
    auto tp = std::chrono::system_clock::from_time_t(timegm(&tm));
    
    if (ts_str.size() > 20 && ts_str[19] == '.') {
        int ms = 0;
        if (std::isdigit(ts_str[20])) ms += (ts_str[20] - '0') * 100;
        if (ts_str.size() > 21 && std::isdigit(ts_str[21])) ms += (ts_str[21] - '0') * 10;
        if (ts_str.size() > 22 && std::isdigit(ts_str[22])) ms += (ts_str[22] - '0');
        tp += std::chrono::milliseconds(ms);
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

PositionUpdate MetaScalpCodec::parse_position_update(const nlohmann::json& j) {
    check_required(j, fields::kId);
    check_required(j, fields::kTicker);
    
    return PositionUpdate {
        .connection_id = j.value(fields::kConnectionId, 0),
        .position_id = j.value(fields::kId, 0L),
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
        const auto& ja = j[fields::kBalances];
        balances.reserve(ja.size());
        std::transform(ja.begin(), ja.end(), std::back_inserter(balances), [](const auto& item) {
            return BalanceEntry {
                .coin = item.value(fields::kCoin, ""),
                .total = item.value(fields::kTotal, 0.0),
                .free = item.value(fields::kFree, 0.0),
                .locked = item.value(fields::kLocked, 0.0)
            };
        });
    }
    return BalanceUpdate {
        .connection_id = j.value(fields::kConnectionId, 0),
        .balances = balances
    };
}

std::vector<Trade> MetaScalpCodec::parse_trade_update(const nlohmann::json& j) {
    std::vector<Trade> trades;
    if (j.contains(fields::kTrades) && j[fields::kTrades].is_array()) {
        const auto& ja = j[fields::kTrades];
        trades.resize(ja.size());
        std::transform(ja.begin(), ja.end(), trades.begin(), [&](const auto& item) {
            return Trade {
                .price = item.value(fields::kPrice, 0.0),
                .size = item.value(fields::kSize, 0.0),
                .side = parse_side(item.value(fields::kSide, nlohmann::json())),
                .timestamp = parse_timestamp(item.value(fields::kTime, ""))
            };
        });
    }
    return trades;
}

OrderBookSnapshot MetaScalpCodec::parse_orderbook_snapshot(const nlohmann::json& j, const Ticker& ticker) {
    std::vector<PriceLevel> asks, bids;
    if (j.contains(fields::kAsks) && j[fields::kAsks].is_array()) {
        const auto& ja = j[fields::kAsks];
        asks.reserve(ja.size());
        std::transform(ja.begin(), ja.end(), std::back_inserter(asks), [](const auto& item) {
            return PriceLevel{item.value(fields::kPrice, 0.0), item.value(fields::kSize, 0.0), Side::Sell};
        });
    }
    if (j.contains(fields::kBids) && j[fields::kBids].is_array()) {
        const auto& ja = j[fields::kBids];
        bids.reserve(ja.size());
        std::transform(ja.begin(), ja.end(), std::back_inserter(bids), [](const auto& item) {
            return PriceLevel{item.value(fields::kPrice, 0.0), item.value(fields::kSize, 0.0), Side::Buy};
        });
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
        const auto& ja = j[fields::kUpdates];
        changes.reserve(ja.size());
        std::transform(ja.begin(), ja.end(), std::back_inserter(changes), [&](const auto& item) {
            return PriceLevel {
                .price = item.value(fields::kPrice, 0.0),
                .size = item.value(fields::kSize, 0.0),
                .side = parse_side(item.value(fields::kSide, nlohmann::json()))
            };
        });
    }
    return OrderBookUpdate {
        .ticker = ticker,
        .changes = changes,
        .ts = std::chrono::system_clock::now()
    };
}

ConnectionInfo MetaScalpCodec::parse_connection_info(const nlohmann::json& j) {
    return ConnectionInfo {
        .id = j.value(fields::kId, 0),
        .name = j.value(fields::kName, ""),
        .state = j.value(fields::kState, ""),
        .view_mode = j.value(fields::kViewMode, false)
    };
}

TickerInfo MetaScalpCodec::parse_ticker_info(const nlohmann::json& j) {
    return TickerInfo {
        .name = j.value(fields::kName, ""),
        .base_asset = j.value(fields::kBaseAsset, ""),
        .quote_asset = j.value(fields::kQuoteAsset, ""),
        .is_trading_allowed = j.value(fields::kIsTradingAllowed, true),
        .price_increment = j.value(fields::kPriceIncrement, 0.0),
        .size_increment = j.value(fields::kSizeIncrement, 0.0),
        .min_size = j.value(fields::kMinSize, 0.0),
        .max_size = j.value(fields::kMaxSize, 0.0)
    };
}

FinresUpdate MetaScalpCodec::parse_finres_update(const nlohmann::json& j) {
    std::vector<FinresEntry> finreses;
    if (j.contains(fields::kFinreses) && j[fields::kFinreses].is_array()) {
        const auto& ja = j[fields::kFinreses];
        finreses.reserve(ja.size());
        std::transform(ja.begin(), ja.end(), std::back_inserter(finreses), [](const auto& item) {
            return FinresEntry {
                .currency = item.value(fields::kCurrency, ""),
                .result = item.value(fields::kResult, 0.0),
                .fee = item.value(fields::kFee, 0.0),
                .funds = item.value(fields::kFunds, 0.0),
                .available = item.value(fields::kAvailable, 0.0),
                .blocked = item.value(fields::kBlocked, 0.0)
            };
        });
    }
    return FinresUpdate {
        .connection_id = j.value(fields::kConnectionId, 0),
        .finreses = finreses
    };
}

Notification MetaScalpCodec::parse_notification(const nlohmann::json& j) {
    return Notification {
        .kind = static_cast<NotificationKind>(j.value(fields::kKind, 0)),
        .exchange_id = j.value(fields::kExchangeId, 0),
        .market_type = j.value(fields::kMarketType, 0),
        .ticker = j.value(fields::kTicker, ""),
        .price = j.value(fields::kPrice, 0.0),
        .size = j.value(fields::kSize, 0.0),
        .timestamp = parse_timestamp(j.value(fields::kDate, ""))
    };
}

} // namespace trade_bot
