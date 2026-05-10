#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace trade_bot {

using namespace api;

void MetaScalpCodec::check_required(const nlohmann::json& j, std::string_view field) {
    if (j.contains(field)) return;
    std::string alt(field);
    if (!alt.empty()) {
        if (std::isupper(alt[0])) alt[0] = std::tolower(alt[0]);
        else alt[0] = std::toupper(alt[0]);
        if (j.contains(alt)) return;
    }
    throw CodecError("Missing required field: " + std::string(field));
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
        .order_id = get_val<int64_t>(j, fields::kOrderId, 0L),
        .ticker = get_val<std::string>(j, fields::kTicker, ""),
        .side = parse_side(get_val<nlohmann::json>(j, fields::kSide, nlohmann::json())),
        .type = parse_order_type(get_val<nlohmann::json>(j, fields::kType, nlohmann::json())),
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .filled_price = get_val<double>(j, fields::kFilledPrice, 0.0),
        .size = get_val<double>(j, fields::kSize, 0.0),
        .filled_size = get_val<double>(j, fields::kFilledSize, 0.0),
        .fee = get_val<double>(j, fields::kFee, 0.0),
        .fee_currency = get_val<std::string>(j, fields::kFeeCurrency, ""),
        .status = parse_order_status(get_val<nlohmann::json>(j, fields::kStatus, nlohmann::json())),
        .time = parse_timestamp(get_val<std::string>(j, fields::kTime, ""))
    };
}

RestOrder MetaScalpCodec::parse_rest_order(const nlohmann::json& j) {
    check_required(j, fields::kId);
    check_required(j, fields::kTicker);

    std::optional<std::string> client_id;
    std::string cid = get_val<std::string>(j, fields::kClientId, "");
    if (!cid.empty()) client_id = cid;

    std::optional<double> trigger_price;
    double tp_val = get_val<double>(j, fields::kTriggerPrice, -1.0);
    if (tp_val >= 0) trigger_price = tp_val;

    return RestOrder {
        .id = get_val<int64_t>(j, fields::kId, 0L),
        .client_id = client_id,
        .ticker = get_val<std::string>(j, fields::kTicker, ""),
        .side = parse_side(get_val<nlohmann::json>(j, fields::kSide, nlohmann::json())),
        .type = parse_order_type(get_val<nlohmann::json>(j, fields::kType, nlohmann::json())),
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .size = get_val<double>(j, fields::kSize, 0.0),
        .filled_size = get_val<double>(j, fields::kFilledSize, 0.0),
        .filled_price = get_val<double>(j, fields::kFilledPrice, 0.0),
        .remaining_size = get_val<double>(j, fields::kRemainingSize, 0.0),
        .status = parse_order_status(get_val<nlohmann::json>(j, fields::kStatus, nlohmann::json())),
        .trigger_price = trigger_price,
        .create_date = parse_timestamp(get_val<std::string>(j, fields::kCreateDate, ""))
    };
}

PlaceOrderResult MetaScalpCodec::parse_place_order_result(const nlohmann::json& j) {
    return PlaceOrderResult {
        .status = get_val<std::string>(j, "Status", ""),
        .client_id = get_val<std::string>(j, fields::kClientId, ""),
        .execution_time_ms = get_val<double>(j, fields::kExecutionTimeMs, 0.0)
    };
}

PositionUpdate MetaScalpCodec::parse_position_update(const nlohmann::json& j) {
    check_required(j, fields::kId);
    check_required(j, fields::kTicker);
    
    return PositionUpdate {
        .connection_id = get_val<int>(j, fields::kConnectionId, 0),
        .position_id = get_val<int64_t>(j, fields::kId, 0L),
        .ticker = get_val<std::string>(j, fields::kTicker, ""),
        .side = parse_side(get_val<nlohmann::json>(j, fields::kSide, nlohmann::json())),
        .size = get_val<double>(j, fields::kSize, 0.0),
        .avg_price = get_val<double>(j, fields::kAvgPrice, 0.0),
        .avg_price_fix = get_val<double>(j, fields::kAvgPriceFix, 0.0),
        .avg_price_dyn = get_val<double>(j, fields::kAvgPriceDyn, 0.0),
        .status = parse_position_status(get_val<nlohmann::json>(j, fields::kStatus, nlohmann::json()))
    };
}

BalanceUpdate MetaScalpCodec::parse_balance_update(const nlohmann::json& j) {
    std::vector<BalanceEntry> balances;
    std::string key = fields::kBalances;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        alt[0] = std::tolower(alt[0]);
        if (j.contains(alt)) ja_ptr = &j[alt];
    }

    if (ja_ptr && ja_ptr->is_array()) {
        const auto& ja = *ja_ptr;
        balances.reserve(ja.size());
        for (const auto& item : ja) {
            balances.push_back(BalanceEntry {
                .coin = get_val<std::string>(item, fields::kCoin, ""),
                .total = get_val<double>(item, fields::kTotal, 0.0),
                .free = get_val<double>(item, fields::kFree, 0.0),
                .locked = get_val<double>(item, fields::kLocked, 0.0)
            });
        }
    }
    return BalanceUpdate {
        .connection_id = get_val<int>(j, fields::kConnectionId, 0),
        .balances = balances
    };
}

std::vector<Trade> MetaScalpCodec::parse_trade_update(const nlohmann::json& j) {
    std::vector<Trade> trades;
    std::string key = fields::kTrades;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        alt[0] = std::tolower(alt[0]);
        if (j.contains(alt)) ja_ptr = &j[alt];
    }

    if (ja_ptr && ja_ptr->is_array()) {
        const auto& ja = *ja_ptr;
        trades.reserve(ja.size());
        for (const auto& item : ja) {
            trades.push_back(Trade {
                .price = get_val<double>(item, fields::kPrice, 0.0),
                .size = get_val<double>(item, fields::kSize, 0.0),
                .side = parse_side(get_val<nlohmann::json>(item, fields::kSide, nlohmann::json())),
                .timestamp = parse_timestamp(get_val<std::string>(item, fields::kTime, ""))
            });
        }
    }
    return trades;
}

OrderBookSnapshot MetaScalpCodec::parse_orderbook_snapshot(const nlohmann::json& j, const Ticker& ticker) {
    std::vector<PriceLevel> asks, bids;
    
    auto parse_levels = [&](const std::string& key, std::vector<PriceLevel>& target, Side side) {
        const nlohmann::json* ja_ptr = nullptr;
        if (j.contains(key)) ja_ptr = &j[key];
        else {
            std::string alt = key;
            if (!alt.empty()) alt[0] = std::tolower(alt[0]);
            if (j.contains(alt)) ja_ptr = &j[alt];
        }
        
        if (ja_ptr && ja_ptr->is_array()) {
            const auto& ja = *ja_ptr;
            target.reserve(ja.size());
            for (const auto& item : ja) {
                target.push_back(PriceLevel{
                    get_val<double>(item, fields::kPrice, 0.0), 
                    get_val<double>(item, fields::kSize, 0.0), 
                    side
                });
            }
        }
    };

    parse_levels(fields::kAsks, asks, Side::Sell);
    parse_levels(fields::kBids, bids, Side::Buy);

    return OrderBookSnapshot {
        .ticker = ticker,
        .asks = asks,
        .bids = bids,
        .ts = std::chrono::system_clock::now()
    };
}

OrderBookUpdate MetaScalpCodec::parse_orderbook_update(const nlohmann::json& j, const Ticker& ticker) {
    std::vector<PriceLevel> changes;
    std::string key = fields::kUpdates;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty()) alt[0] = std::tolower(alt[0]);
        if (j.contains(alt)) ja_ptr = &j[alt];
    }

    if (ja_ptr && ja_ptr->is_array()) {
        const auto& ja = *ja_ptr;
        changes.reserve(ja.size());
        for (const auto& item : ja) {
            changes.push_back(PriceLevel {
                .price = get_val<double>(item, fields::kPrice, 0.0),
                .size = get_val<double>(item, fields::kSize, 0.0),
                .side = parse_side(get_val<nlohmann::json>(item, fields::kSide, nlohmann::json()))
            });
        }
    }
    return OrderBookUpdate {
        .ticker = ticker,
        .changes = changes,
        .ts = std::chrono::system_clock::now()
    };
}

ConnectionInfo MetaScalpCodec::parse_connection_info(const nlohmann::json& j) {
    std::string state_str;
    auto state_val = get_val<nlohmann::json>(j, fields::kState, nlohmann::json());
    if (state_val.is_number()) {
        int state_int = state_val.get<int>();
        switch (state_int) {
            case 0: state_str = "Disconnected"; break;
            case 1: state_str = "Connecting"; break;
            case 2: state_str = "Connected"; break;
            case 3: state_str = "Error"; break;
            default: state_str = "Unknown(" + std::to_string(state_int) + ")"; break;
        }
    } else if (state_val.is_string()) {
        state_str = state_val.get<std::string>();
    }

    return ConnectionInfo {
        .id = get_val<int>(j, fields::kId, 0),
        .name = get_val<std::string>(j, fields::kName, ""),
        .state = state_str,
        .view_mode = get_val<bool>(j, fields::kViewMode, false)
    };
}

TickerInfo MetaScalpCodec::parse_ticker_info(const nlohmann::json& j) {
    return TickerInfo {
        .name = get_val<std::string>(j, fields::kName, ""),
        .base_asset = get_val<std::string>(j, fields::kBaseAsset, ""),
        .quote_asset = get_val<std::string>(j, fields::kQuoteAsset, ""),
        .is_trading_allowed = get_val<bool>(j, fields::kIsTradingAllowed, true),
        .price_increment = get_val<double>(j, fields::kPriceIncrement, 0.0),
        .size_increment = get_val<double>(j, fields::kSizeIncrement, 0.0),
        .min_size = get_val<double>(j, fields::kMinSize, 0.0),
        .max_size = get_val<double>(j, fields::kMaxSize, 0.0)
    };
}

FinresUpdate MetaScalpCodec::parse_finres_update(const nlohmann::json& j) {
    std::vector<FinresEntry> finreses;
    std::string key = fields::kFinreses;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty()) alt[0] = std::tolower(alt[0]);
        if (j.contains(alt)) ja_ptr = &j[alt];
    }

    if (ja_ptr && ja_ptr->is_array()) {
        const auto& ja = *ja_ptr;
        finreses.reserve(ja.size());
        for (const auto& item : ja) {
            finreses.push_back(FinresEntry {
                .currency = get_val<std::string>(item, fields::kCurrency, ""),
                .result = get_val<double>(item, fields::kResult, 0.0),
                .fee = get_val<double>(item, fields::kFee, 0.0),
                .funds = get_val<double>(item, fields::kFunds, 0.0),
                .available = get_val<double>(item, fields::kAvailable, 0.0),
                .blocked = get_val<double>(item, fields::kBlocked, 0.0)
            });
        }
    }
    return FinresUpdate {
        .connection_id = get_val<int>(j, fields::kConnectionId, 0),
        .finreses = finreses
    };
}

Notification MetaScalpCodec::parse_notification(const nlohmann::json& j) {
    return Notification {
        .kind = static_cast<NotificationKind>(get_val<int>(j, fields::kKind, 0)),
        .exchange_id = get_val<int>(j, fields::kExchangeId, 0),
        .market_type = get_val<int>(j, fields::kMarketType, 0),
        .ticker = get_val<std::string>(j, fields::kTicker, ""),
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .size = get_val<double>(j, fields::kSize, 0.0),
        .timestamp = parse_timestamp(get_val<std::string>(j, fields::kDate, ""))
    };
}

SignalLevel MetaScalpCodec::parse_signal_level(const nlohmann::json& j) {
    return SignalLevel {
        .id = get_val<int>(j, fields::kId, 0),
        .ticker = get_val<std::string>(j, fields::kTicker, ""),
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .triggered = get_val<bool>(j, fields::kTriggered, false),
        .created_at = parse_timestamp(get_val<std::string>(j, fields::kTime, ""))
    };
}

OrderbookSettings MetaScalpCodec::parse_orderbook_settings(const nlohmann::json& j) {
    return OrderbookSettings {
        .ticker = get_val<std::string>(j, fields::kTicker, ""),
        .large_amount_usd = get_val<double>(j, fields::kLargeAmountUsd, 0.0),
        .large_amount_usd2 = get_val<double>(j, fields::kLargeAmountUsd2, 0.0)
    };
}

SignalLevelTriggered MetaScalpCodec::parse_signal_level_triggered(const nlohmann::json& j) {
    return SignalLevelTriggered {
        .ticker = get_val<std::string>(j, fields::kTicker, ""),
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .timestamp = std::chrono::system_clock::now() // Usually triggered events are real-time
    };
}

ClusterSnapshot MetaScalpCodec::parse_cluster_snapshot(const nlohmann::json& j) {
    std::vector<ClusterItem> items;
    std::string key = fields::kItems;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty()) alt[0] = std::tolower(alt[0]);
        if (j.contains(alt)) ja_ptr = &j[alt];
    }

    if (ja_ptr && ja_ptr->is_array()) {
        const auto& ja = *ja_ptr;
        items.reserve(ja.size());
        for (const auto& item : ja) {
            items.push_back(ClusterItem {
                .price    = get_val<double>(item, fields::kPrice,   0.0),
                .ask_size = get_val<double>(item, fields::kAskSize, 0.0),
                .bid_size = get_val<double>(item, fields::kBidSize, 0.0)
            });
        }
    }
    return ClusterSnapshot {
        .ticker     = get_val<std::string>(j, fields::kTicker,    ""),
        .timeframe  = get_val<std::string>(j, fields::kTimeFrame, ""),
        .zoom_index = get_val<int>(j, fields::kZoomIndex, 0),
        .items      = std::move(items),
        .ts         = std::chrono::system_clock::now()
    };
}

} // namespace trade_bot
