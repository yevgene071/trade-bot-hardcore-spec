#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "utils/TickerSymbol.hpp"
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace trade_bot {

using namespace api;

bool MetaScalpCodec::has_required(const nlohmann::json& j, std::string_view field) {
    if (j.contains(field)) return true;
    std::string alt(field);
    if (!alt.empty()) {
        if (std::isupper(alt[0])) alt[0] = std::tolower(alt[0]);
        else alt[0] = std::toupper(alt[0]);
        if (j.contains(alt)) return true;
    }
    return false;
}

void MetaScalpCodec::check_required(const nlohmann::json& j, std::string_view field) {
    if (!has_required(j, field)) {
        throw CodecError(std::string("Missing required field: ") + std::string(field));
    }
}

Side MetaScalpCodec::parse_side(const nlohmann::json& v) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        // orderbook_update encodes side in `Type` as Bid/Ask; trades/orders
        // use Buy/Sell. Accept both (bid == buy side, ask == sell side).
        if (s == "Buy"  || s == "buy"  || s == "Bid" || s == "bid" || s == "BestBid" || s == "bestbid") return Side::Buy;
        if (s == "Sell" || s == "sell" || s == "Ask" || s == "ask" || s == "BestAsk" || s == "bestask") return Side::Sell;
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

std::expected<OrderType, std::string> MetaScalpCodec::parse_order_type_checked(const nlohmann::json& v) {
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
    return std::unexpected("Unknown OrderType: " + v.dump());
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
    // R5: unknown status → Closed (safe: won't reactivate a cancelled/rejected order as New)
    return OrderStatus::Closed;
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
    // R5: unknown status → Closed (safe: won't keep a dead position marked as New)
    return PositionStatus::Closed;
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
    
    // T4-PORTABILITY: Cross-platform UTC to time_t conversion (#135)
    auto time_utc = [](std::tm* tm_ptr) {
#if defined(_WIN32)
        return _mkgmtime(tm_ptr);
#else
        return timegm(tm_ptr);
#endif
    };
    
    auto tp = std::chrono::system_clock::from_time_t(time_utc(&tm));

    // R7: parse up to 6 sub-second digits (microsecond precision)
    size_t tz_start_idx = 19;
    if (ts_str.size() > 20 && ts_str[19] == '.') {
        int64_t sub_us = 0;
        int digits = 0;
        size_t i = 20;
        for (; i < ts_str.size() && std::isdigit(static_cast<unsigned char>(ts_str[i])); ++i) {
            if (digits < 6) {
                sub_us = sub_us * 10 + (ts_str[i] - '0');
                ++digits;
            }
        }
        while (digits < 6) { sub_us *= 10; ++digits; }
        tp += std::chrono::microseconds(sub_us);
        tz_start_idx = i;
    }

    // R4: handle TZ offset (Z = UTC, ±HH:MM = apply offset to get UTC)
    for (size_t i = tz_start_idx; i < ts_str.size(); ++i) {
        if (ts_str[i] == 'Z') break;  // already UTC
        if ((ts_str[i] == '+' || ts_str[i] == '-') && i + 5 <= ts_str.size()) {
            int sign = (ts_str[i] == '+') ? 1 : -1;
            int tz_h = p2(&ts_str[i + 1]);
            int tz_m = p2(&ts_str[i + 4]);
            tp -= std::chrono::minutes(sign * (tz_h * 60 + tz_m));
            break;
        }
    }

    return tp;
}

std::expected<OrderUpdate, std::string> MetaScalpCodec::parse_order_update(const nlohmann::json& j) {
    if (!has_required(j, fields::kOrderId)) return std::unexpected("Missing required field: OrderId");
    if (!has_required(j, fields::kTicker))  return std::unexpected("Missing required field: Ticker");

    auto type = parse_order_type_checked(get_val<nlohmann::json>(j, fields::kType, nlohmann::json()));
    if (!type) return std::unexpected(type.error());

    return OrderUpdate {
        .order_id = get_val<int64_t>(j, fields::kOrderId, 0L),
        .ticker = normalize_ticker(get_val<std::string>(j, fields::kTicker, "")),
        .side = parse_side(get_val<nlohmann::json>(j, fields::kSide, nlohmann::json())),
        .type = *type,
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .filled_price = get_val<double>(j, fields::kFilledPrice, 0.0),
        .size = get_val<double>(j, fields::kSize, 0.0),
        .filled_size = get_val<double>(j, fields::kFilledSize, 0.0),
        .fee = get_val<double>(j, fields::kFee, 0.0),
        .fee_currency = get_val<std::string>(j, fields::kFeeCurrency, ""),
        .status = parse_order_status(get_val<nlohmann::json>(j, fields::kStatus, nlohmann::json())),
        .time = parse_timestamp(get_val<std::string>(j, fields::kTime, "")),
        .client_order_id = get_val<std::string>(j, fields::kClientId, "")
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
        .ticker = normalize_ticker(get_val<std::string>(j, fields::kTicker, "")),
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
        .order_id = get_val<int64_t>(j, fields::kOrderId, 0L),
        .execution_time_ms = get_val<double>(j, fields::kExecutionTimeMs, 0.0)
    };
}

std::expected<PositionUpdate, std::string> MetaScalpCodec::parse_position_update(const nlohmann::json& j) {
    if (!has_required(j, fields::kId))     return std::unexpected("Missing required field: Id");
    if (!has_required(j, fields::kTicker)) return std::unexpected("Missing required field: Ticker");

    return PositionUpdate {
        .connection_id = get_val<int>(j, fields::kConnectionId, 0),
        .position_id = get_val<int64_t>(j, fields::kId, 0L),
        .ticker = normalize_ticker(get_val<std::string>(j, fields::kTicker, "")),
        .side = parse_side(get_val<nlohmann::json>(j, fields::kSide, nlohmann::json())),
        .size = get_val<double>(j, fields::kSize, 0.0),
        .avg_price = get_val<double>(j, fields::kAvgPrice, 0.0),
        .avg_price_fix = get_val<double>(j, fields::kAvgPriceFix, 0.0),
        .avg_price_dyn = get_val<double>(j, fields::kAvgPriceDyn, 0.0),
        .price_increment = get_val<double>(j, fields::kPriceIncrement, 0.0),
        .status = parse_position_status(get_val<nlohmann::json>(j, fields::kStatus, nlohmann::json()))
    };
}

std::expected<BalanceUpdate, std::string> MetaScalpCodec::parse_balance_update(const nlohmann::json& j) {
    std::vector<BalanceEntry> balances;
    std::string key = fields::kBalances;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty() && std::isupper(static_cast<unsigned char>(alt[0]))) {
            alt[0] = std::tolower(static_cast<unsigned char>(alt[0]));
            if (j.contains(alt)) ja_ptr = &j[alt];
        }
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

std::expected<std::vector<Trade>, std::string> MetaScalpCodec::parse_trade_update(const nlohmann::json& j) {
    std::vector<Trade> trades;
    std::string key = fields::kTrades;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty() && std::isupper(static_cast<unsigned char>(alt[0]))) {
            alt[0] = std::tolower(static_cast<unsigned char>(alt[0]));
            if (j.contains(alt)) ja_ptr = &j[alt];
        }
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

std::expected<OrderBookSnapshot, std::string> MetaScalpCodec::parse_orderbook_snapshot(const nlohmann::json& j, const Ticker& ticker) {
    std::vector<PriceLevel> asks, bids;
    
    auto parse_levels = [&](const std::string& key, std::vector<PriceLevel>& target, Side side) {
        const nlohmann::json* ja_ptr = nullptr;
        if (j.contains(key)) ja_ptr = &j[key];
        else {
            std::string alt = key;
            if (!alt.empty() && std::isupper(static_cast<unsigned char>(alt[0]))) {
                alt[0] = std::tolower(static_cast<unsigned char>(alt[0]));
                if (j.contains(alt)) ja_ptr = &j[alt];
            }
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

    // T4-LATENCY: Use server timestamp if available, fallback to now()
    std::string ts_str = get_val<std::string>(j, fields::kTime, "");
    auto ts = ts_str.empty() ? std::chrono::system_clock::now() : parse_timestamp(ts_str);

    return OrderBookSnapshot {
        .ticker = normalize_ticker(ticker),
        .asks = asks,
        .bids = bids,
        .ts = ts
    };
}

std::expected<OrderBookUpdate, std::string> MetaScalpCodec::parse_orderbook_update(const nlohmann::json& j, const Ticker& ticker) {
    std::vector<PriceLevel> changes;
    std::string key = fields::kUpdates;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty() && std::isupper(static_cast<unsigned char>(alt[0]))) {
            alt[0] = std::tolower(static_cast<unsigned char>(alt[0]));
            if (j.contains(alt)) ja_ptr = &j[alt];
        }
    }

    if (ja_ptr && ja_ptr->is_array()) {
        const auto& ja = *ja_ptr;
        changes.reserve(ja.size());
        for (const auto& item : ja) {
            // MetaScalp API: orderbook_update levels carry side in `Type`
            // (numeric 1=bid / 2=ask), NOT `Side`. Reading the wrong field
            // yields Side::None, which apply_change_ silently drops — the
            // book then never updates past the initial snapshot.
            changes.push_back(PriceLevel {
                .price = get_val<double>(item, fields::kPrice, 0.0),
                .size = get_val<double>(item, fields::kSize, 0.0),
                .side = parse_side(get_val<nlohmann::json>(item, fields::kType, nlohmann::json()))
            });
        }
    }
    // R1: prefer server timestamp over local now() for deterministic replay
    std::string ts_str = get_val<std::string>(j, fields::kTime, "");
    auto ts = ts_str.empty() ? std::chrono::system_clock::now() : parse_timestamp(ts_str);
    return OrderBookUpdate {
        .ticker = normalize_ticker(ticker),
        .changes = changes,
        .ts = ts
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
    Ticker raw_name = get_val<std::string>(j, fields::kName, "");
    return TickerInfo {
        .name = normalize_ticker(raw_name),
        .base_asset = get_val<std::string>(j, fields::kBaseAsset, ""),
        .quote_asset = get_val<std::string>(j, fields::kQuoteAsset, ""),
        .is_trading_allowed = get_val<bool>(j, fields::kIsTradingAllowed, true),
        .price_increment = get_val<double>(j, fields::kPriceIncrement, 0.0),
        .size_increment = get_val<double>(j, fields::kSizeIncrement, 0.0),
        .min_size = get_val<double>(j, fields::kMinSize, 0.0),
        .max_size = get_val<double>(j, fields::kMaxSize, 0.0)
    };
}

Ticker MetaScalpCodec::normalize_ticker(const Ticker& raw) {
    return to_internal_ticker(raw);
}

std::expected<FinresUpdate, std::string> MetaScalpCodec::parse_finres_update(const nlohmann::json& j) {
    std::vector<FinresEntry> finreses;
    std::string key = fields::kFinreses;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty() && std::isupper(static_cast<unsigned char>(alt[0]))) {
            alt[0] = std::tolower(static_cast<unsigned char>(alt[0]));
            if (j.contains(alt)) ja_ptr = &j[alt];
        }
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
    NotificationKind kind = NotificationKind::Trade;
    std::string type_str = get_val<std::string>(j, fields::kType, "");
    if (!type_str.empty()) {
        kind = parse_notification_type(type_str);
    } else {
        // Fallback: try reading as integer (backward compat with old tests)
        kind = static_cast<NotificationKind>(get_val<int>(j, fields::kKind, 0));
    }

    std::string ticker_raw = get_val<std::string>(j, fields::kTicker, "");
    // Normalize ticker (e.g. BTCUSDT -> BTC_USDT) for consistency with internal universe
    Ticker ticker = normalize_ticker(ticker_raw);

    return Notification {
        .kind = kind,
        .exchange_id = get_val<int>(j, fields::kExchangeId, 0),
        .market_type = parse_market_type(get_val<nlohmann::json>(j, fields::kMarketType, nlohmann::json())),
        .ticker = ticker,
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .size = get_val<double>(j, fields::kSize, 0.0),
        .level_id = (kind == NotificationKind::SignalLevel) ? get_val<int64_t>(j, fields::kSize, 0) : 0,
        .timestamp = parse_timestamp(get_val<std::string>(j, fields::kDate, ""))
    };
}

NotificationKind MetaScalpCodec::parse_notification_type(const std::string& s) {
    std::string type = s;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c){ return std::tolower(c); });

    if (type == "trade")               return NotificationKind::Trade;
    if (type == "signallevel")         return NotificationKind::SignalLevel;
    if (type == "bigorderbookamount")  return NotificationKind::BigOrderBookAmount;
    if (type == "bigorderbookamount2") return NotificationKind::BigOrderBookAmount2;
    if (type == "bigtick")             return NotificationKind::BigTick;
    if (type == "screenernewcoin" || type == "screener") return NotificationKind::ScreenerNewCoin;
    // Unknown type — fall back to Trade (safe default, ignored in routing)
    return NotificationKind::Trade;
}

int MetaScalpCodec::parse_market_type(const nlohmann::json& v) {
    if (v.is_number()) {
        return v.get<int>();
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s == "Spot")            return 0;
        if (s == "Futures")         return 1;
        if (s == "UsdtFutures")     return 2;
        if (s == "CoinFutures")    return 3;
        if (s == "InverseFutures") return 4;
        if (s == "UsdtPerpetual")  return 5;
        if (s == "UsdcPerpetual")  return 6;
        if (s == "Margin")         return 7;
        if (s == "Options")        return 8;
        if (s == "Stock")          return 9;
    }
    return 0;
}

SignalLevel MetaScalpCodec::parse_signal_level(const nlohmann::json& j) {
    const bool triggered = get_val<bool>(j, fields::kIsTriggered,
        get_val<bool>(j, fields::kTriggered, false));
    std::string ts = get_val<std::string>(j, fields::kTriggerTime, "");
    if (ts.empty()) ts = get_val<std::string>(j, fields::kTime, "");
    return SignalLevel {
        .id = get_val<int64_t>(j, fields::kId, 0),
        .ticker = normalize_ticker(get_val<std::string>(j, fields::kTicker, "")),
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .triggered = triggered,
        .created_at = parse_timestamp(ts)
    };
}

OrderbookSettings MetaScalpCodec::parse_orderbook_settings(const nlohmann::json& j) {
    const nlohmann::json* settings = &j;
    if (j.contains(fields::kSettings) && j[fields::kSettings].is_object()) {
        settings = &j[fields::kSettings];
    } else if (j.contains("settings") && j["settings"].is_object()) {
        settings = &j["settings"];
    }

    auto ticker = get_val<std::string>(j, fields::kTicker, "");
    if (ticker.empty()) ticker = get_val<std::string>(*settings, fields::kTicker, "");

    return OrderbookSettings {
        .ticker = normalize_ticker(ticker),
        .large_amount_usd = get_val<double>(*settings, fields::kLargeAmountUsd, 0.0),
        .large_amount_usd2 = get_val<double>(*settings, fields::kLargeAmountUsd2, 0.0)
    };
}

SignalLevelTriggered MetaScalpCodec::parse_signal_level_triggered(const nlohmann::json& j) {
    // R3: use server timestamp if present for replay determinism
    std::string ts_str = get_val<std::string>(j, fields::kTriggerTime, "");
    if (ts_str.empty()) ts_str = get_val<std::string>(j, fields::kTime, "");
    return SignalLevelTriggered {
        .ticker = normalize_ticker(get_val<std::string>(j, fields::kTicker, "")),
        .price = get_val<double>(j, fields::kPrice, 0.0),
        .timestamp = ts_str.empty() ? std::chrono::system_clock::now() : parse_timestamp(ts_str)
    };
}

ClusterSnapshot MetaScalpCodec::parse_cluster_snapshot(const nlohmann::json& j) {
    check_required(j, fields::kTicker);
    check_required(j, fields::kTimeFrame);
    check_required(j, fields::kItems);

    std::vector<ClusterItem> items;
    std::string key = fields::kItems;
    const nlohmann::json* ja_ptr = nullptr;
    if (j.contains(key)) ja_ptr = &j[key];
    else {
        std::string alt = key;
        if (!alt.empty() && std::isupper(static_cast<unsigned char>(alt[0]))) {
            alt[0] = std::tolower(static_cast<unsigned char>(alt[0]));
            if (j.contains(alt)) ja_ptr = &j[alt];
        }
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
        .ticker     = normalize_ticker(get_val<std::string>(j, fields::kTicker, "")),
        .timeframe  = get_val<std::string>(j, fields::kTimeFrame, ""),
        .zoom_index = get_val<int>(j, fields::kZoomIndex, 0),
        .items      = std::move(items),
        // R3: use server timestamp if present for replay determinism
        .ts         = [&]() {
            std::string ts_str = get_val<std::string>(j, fields::kTime, "");
            return ts_str.empty() ? std::chrono::system_clock::now() : parse_timestamp(ts_str);
        }()
    };
}

} // namespace trade_bot
