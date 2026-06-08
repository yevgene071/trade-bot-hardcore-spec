#include "MarketDataFeed.hpp"
#include "MetaScalpCodec.hpp"
#include "config/Config.hpp"
#include "logger/Logger.hpp"
#include "perf/TraceContext.hpp"
#include "perf/TraceTimeBuffer.hpp"
#include "perf/PerfRegistry.hpp"
#include "utils/TickerSymbol.hpp"
#include <algorithm>
#include <chrono>
#include <iterator>

namespace trade_bot {

// Internal keys use BASE_QUOTE. MetaScalp SDK v1.0.7 REST/WS examples use
// exchange symbols without separators, e.g. BTCUSDT.
static std::string to_ms(const std::string& t) {
    return to_metascalp_symbol(t);
}

static std::string from_ms(const std::string& t) {
    if (t.empty()) return "";
    return to_internal_ticker(t);
}

MarketDataFeed::MarketDataFeed(std::shared_ptr<IWsClient> ws_client, int connection_id)
    : m_ws_client(std::move(ws_client))
    , m_connection_id(connection_id) {

    // Seed CoW snapshots with empty maps so lock-free readers never deref null.
    m_merged_snapshot.store(std::make_shared<const MergedMap>(), std::memory_order_relaxed);
    m_global_listeners_snapshot.store(std::make_shared<const ListenerList>(), std::memory_order_relaxed);
    m_funding_snapshot.store(std::make_shared<const FundingMap>(), std::memory_order_relaxed);
    m_mark_price_snapshot.store(std::make_shared<const MarkPriceMap>(), std::memory_order_relaxed);


    m_ws_client->set_on_message([this](const nlohmann::json& j, uint64_t recv_ns, TraceId trace_id) {
        // Store trace_id → recv_ns for end-to-end latency tracking
        trace_times().store(trace_id, recv_ns);
        
        // Push trace context for downstream pipeline
        TraceContextScope trace_scope(trace_id, recv_ns);
        
        if (m_record_tap) {
            m_record_tap(j, recv_ns);
        }
        handle_message(j);
    });

    m_ws_client->set_on_connect([this]() {
        if (m_active) {
            resubscribe_all();
        }
    });
}

void MarketDataFeed::add_listener(IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.push_back(listener);
    rebuild_listener_snapshot_();
}

void MarketDataFeed::add_listener(const Ticker& ticker, IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ticker_listeners[from_ms(ticker)].push_back(listener);
    rebuild_listener_snapshot_();
}

void MarketDataFeed::remove_listener(IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
    for (auto it = m_ticker_listeners.begin(); it != m_ticker_listeners.end(); ) {
        it->second.erase(std::remove(it->second.begin(), it->second.end(), listener), it->second.end());
        if (it->second.empty()) {
            it = m_ticker_listeners.erase(it);
        } else {
            ++it;
        }
    }
    rebuild_listener_snapshot_();
}

void MarketDataFeed::remove_listener(const Ticker& ticker, IMarketDataListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Ticker key = from_ms(ticker);
    if (auto it = m_ticker_listeners.find(key); it != m_ticker_listeners.end()) {
        it->second.erase(std::remove(it->second.begin(), it->second.end(), listener), it->second.end());
        if (it->second.empty()) {
            m_ticker_listeners.erase(it);
        }
    }
    rebuild_listener_snapshot_();
}

void MarketDataFeed::set_orderbook_snapshot_fetcher(OrderBookSnapshotFetcher fetcher) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot_fetcher = std::move(fetcher);
}

// Writer side only — caller MUST hold m_mutex. Rebuilds the full merged
// listener map plus the global-only list and publishes them via atomic
// store-release so lock-free readers observe a consistent snapshot.
void MarketDataFeed::rebuild_listener_snapshot_() {
    // Global-only list (used for ticker-less dispatch, e.g. errors/mark price).
    auto global_list = std::make_shared<ListenerList>(m_listeners.begin(), m_listeners.end());

    auto merged = std::make_shared<MergedMap>();
    // Pre-build the per-ticker merged lists: specific listeners first, then
    // global listeners (deduplicated), mirroring the previous lazy behaviour.
    for (const auto& [ticker, specific] : m_ticker_listeners) {
        auto targets = std::make_shared<ListenerList>();
        for (auto* l : specific) targets->push_back(l);
        for (auto* l : m_listeners) {
            if (std::find(targets->begin(), targets->end(), l) == targets->end()) {
                targets->push_back(l);
            }
        }
        (*merged)[ticker] = std::shared_ptr<const ListenerList>(std::move(targets));
    }
    // The empty ticker maps to the global-only list.
    (*merged)[Ticker{}] = std::shared_ptr<const ListenerList>(global_list);

    m_global_listeners_snapshot.store(std::shared_ptr<const ListenerList>(std::move(global_list)),
                                      std::memory_order_release);
    m_merged_snapshot.store(std::shared_ptr<const MergedMap>(std::move(merged)),
                            std::memory_order_release);
}

void MarketDataFeed::subscribe_ticker(const Ticker& ticker) {
    const Ticker key = from_ms(ticker);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribed_tickers.insert(key);
    }

    const std::string ms_ticker = to_ms(key);
    LOG_INFO("Subscribing to {}", key);
    const int64_t depth_levels = Config::get_or<int64_t>("feed.depth_levels", 50);
    const double depth_percent = Config::get_or<double>("feed.depth_percent", 0.5);
    const bool fetch_snapshot_on_subscribe =
        Config::get_or<bool>("feed.fetch_snapshot_on_subscribe", true);
    const bool use_rest_seed =
        !fetch_snapshot_on_subscribe &&
        try_rest_orderbook_snapshot_(key, 0, static_cast<int>(depth_levels), depth_percent);
    
    auto send_sub = [&](const char* type) {
        m_ws_client->send(nlohmann::json{
            {"Type", type},
            {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ms_ticker}, {"ZoomIndex", 0}}}
        }.dump());
    };
    send_sub("trade_subscribe");
    send_orderbook_subscribe_(key, use_rest_seed);
    send_sub("funding_subscribe");
    send_sub("mark_price_subscribe");
}

void MarketDataFeed::force_resync_orderbook(const Ticker& ticker) {
    const int64_t depth_levels = Config::get_or<int64_t>("feed.depth_levels", 50);
    const double depth_percent = Config::get_or<double>("feed.depth_percent", 0.5);
    const Ticker key = from_ms(ticker);
    if (try_rest_orderbook_snapshot_(key, 0, static_cast<int>(depth_levels), depth_percent)) {
        return;
    }
    send_orderbook_subscribe_(key, false);
}

void MarketDataFeed::unsubscribe_ticker(const Ticker& ticker) {
    const Ticker key = from_ms(ticker);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribed_tickers.erase(key);
    }

    const std::string ms_ticker = to_ms(key);
    auto send_unsub = [&](const char* type) {
        m_ws_client->send(nlohmann::json{
            {"Type", type},
            {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ms_ticker}}}
        }.dump());
    };
    send_unsub("trade_unsubscribe");
    send_unsub("orderbook_unsubscribe");
    send_unsub("funding_unsubscribe");
    send_unsub("mark_price_unsubscribe");
}

std::optional<FundingData> MarketDataFeed::get_funding(const Ticker& ticker) const {
    auto snap = m_funding_snapshot.load(std::memory_order_acquire);
    auto it = snap->find(from_ms(ticker));
    if (it == snap->end()) return std::nullopt;
    return it->second;
}

double MarketDataFeed::get_mark_price(const Ticker& ticker) const {
    auto snap = m_mark_price_snapshot.load(std::memory_order_acquire);
    auto it = snap->find(from_ms(ticker));
    return it != snap->end() ? it->second : 0.0;
}

absl::btree_map<Ticker, double> MarketDataFeed::get_all_mark_prices() const {
    auto snap = m_mark_price_snapshot.load(std::memory_order_acquire);
    return *snap;
}

void MarketDataFeed::start() {
    m_active.store(true, std::memory_order_relaxed);
    resubscribe_all();
}

void MarketDataFeed::stop() {
    m_active.store(false, std::memory_order_relaxed);
}

void MarketDataFeed::resubscribe_all() {
    LOG_INFO("Resubscribing all tickers and connection updates...");
    
    if (!m_ws_client) {
        LOG_ERROR("m_ws_client is null in resubscribe_all");
        return;
    }

    // Connection level subscribe (orders, positions, balances, finres)
    nlohmann::json conn_sub = {
        {"Type", "subscribe"},
        {"Data", {{"ConnectionId", m_connection_id}}}
    };
    m_ws_client->send(conn_sub.dump());

    std::vector<Ticker> tickers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tickers.reserve(m_subscribed_tickers.size());
        std::copy(m_subscribed_tickers.begin(), m_subscribed_tickers.end(), std::back_inserter(tickers));
    }

    const int64_t depth_levels = Config::get_or<int64_t>("feed.depth_levels", 50);
    const double depth_percent = Config::get_or<double>("feed.depth_percent", 0.5);
    const bool fetch_snapshot_on_subscribe =
        Config::get_or<bool>("feed.fetch_snapshot_on_subscribe", true);

    for (const auto& ticker : tickers) {
        const std::string ms_ticker = to_ms(ticker);
        const bool use_rest_seed =
            !fetch_snapshot_on_subscribe &&
            try_rest_orderbook_snapshot_(ticker, 0, static_cast<int>(depth_levels), depth_percent);
        auto send_sub = [&](const char* type) {
            m_ws_client->send(nlohmann::json{
                {"Type", type},
                {"Data", {{"ConnectionId", m_connection_id}, {"Ticker", ms_ticker}, {"ZoomIndex", 0}}}
            }.dump());
        };
        send_sub("trade_subscribe");
        send_orderbook_subscribe_(ticker, use_rest_seed);
        send_sub("funding_subscribe");
        send_sub("mark_price_subscribe");
    }
}

bool MarketDataFeed::try_rest_orderbook_snapshot_(const Ticker& ticker,
                                                  int zoom_index,
                                                  std::optional<int> depth_levels,
                                                  std::optional<double> depth_percent) {
    const Ticker key = from_ms(ticker);
    OrderBookSnapshotFetcher fetcher;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ws_snapshot_only.contains(key)) return false;
        fetcher = m_snapshot_fetcher;
    }
    if (!fetcher) return false;

    try {
        auto snapshot = fetcher(key, zoom_index, depth_levels, depth_percent);
        snapshot.ticker = from_ms(snapshot.ticker.empty() ? key : snapshot.ticker);
        dispatch_orderbook_snapshot_(snapshot);
        return true;
    } catch (const std::exception& e) {
        const std::string msg = e.what();
        if (msg.find("501") != std::string::npos) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ws_snapshot_only.insert(key);
            LOG_WARN("REST orderbook snapshot unsupported for {}; falling back to WS snapshots", key);
        } else {
            LOG_WARN("REST orderbook snapshot failed for {}: {}; falling back to WS snapshot", key, msg);
        }
        return false;
    }
}

void MarketDataFeed::send_orderbook_subscribe_(const Ticker& ticker, bool fetch_snapshot_false) {
    const Ticker key = from_ms(ticker);
    const int64_t depth_levels = Config::get_or<int64_t>("feed.depth_levels", 50);
    const double depth_percent = Config::get_or<double>("feed.depth_percent", 0.5);
    nlohmann::json data = {
        {"ConnectionId", m_connection_id},
        {"Ticker", to_ms(key)},
        {"ZoomIndex", 0},
        {"DepthLevels", depth_levels},
        {"DepthPercent", depth_percent}
    };
    if (fetch_snapshot_false) {
        data["FetchSnapshot"] = false;
    }
    m_ws_client->send(nlohmann::json{{"Type", "orderbook_subscribe"}, {"Data", std::move(data)}}.dump());
}

void MarketDataFeed::dispatch_orderbook_snapshot_(const OrderBookSnapshot& snapshot) {
    const Ticker ticker = from_ms(snapshot.ticker);
    auto normalized = snapshot;
    normalized.ticker = ticker;
    auto targets = get_target_listeners(ticker);
    for (auto* l : *targets) l->on_orderbook_snapshot(normalized);
}

std::shared_ptr<const MarketDataFeed::ListenerList> MarketDataFeed::get_target_listeners(const Ticker& ticker) {
    // Hot path: single atomic load-acquire, no mutex, no allocation.
    auto merged = m_merged_snapshot.load(std::memory_order_acquire);
    if (merged) {
        auto it = merged->find(ticker);
        if (it != merged->end()) {
            return it->second;
        }
    }
    // Ticker has no specific listeners registered — fall back to the
    // global-only list (covers tickers that only have global listeners).
    return m_global_listeners_snapshot.load(std::memory_order_acquire);
}

void MarketDataFeed::handle_message(const nlohmann::json& j) {
    if (spdlog::should_log(spdlog::level::trace)) { LOG_TRACE("Raw WS: {}", j.dump()); }

    // Get recv_ns from trace context for codec latency measurement
    uint64_t recv_ns = current_trace_context().recv_ns;
    static auto& codec_hist = PerfRegistry::instance().get_or_create(kStageWsToCodec);
    
    auto type_it = j.find("Type");
    if (type_it == j.end() || !type_it->is_string()) {
        // A missing OR non-string Type is malformed. Previously the
        // get_ref<const std::string&> below threw nlohmann::type_error for a
        // non-string Type *outside* any try/catch — an unguarded throw on the
        // WS thread that aborted the process. Reject without throwing instead.
        if (spdlog::should_log(spdlog::level::debug)) { LOG_DEBUG("WS message without valid Type field: {}", j.dump()); }
        return;
    }

    // Avoid std::string copy — borrow reference directly from the parsed JSON
    const std::string& type = type_it->get_ref<const std::string&>();

    if (spdlog::should_log(spdlog::level::debug)) { LOG_DEBUG("WS message: {}", type); }

    static const nlohmann::json kEmptyData = nlohmann::json::object();
    auto data_it = j.find("Data");
    const nlohmann::json& data = (data_it != j.end()) ? *data_it : kEmptyData;

    {
        // Hot-path messages are handled by PipelineProcessor via raw WS bytes.
        // Skip them here to avoid double-processing when both paths are active.
        if (m_skip_hot_path_ && (type == "trade_update" || type == "orderbook_update")) {
            return;
        }

        if (type == "trade_update") {
            auto trades = MetaScalpCodec::parse_trade_update(data);
            if (!trades) { LOG_WARN("parse_trade_update failed: {}", trades.error()); return; }
            record_delta_us(codec_hist, recv_ns);
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, ""));
            auto targets = get_target_listeners(ticker);
            for (auto* l : *targets) l->on_trades(ticker, *trades);
        } else if (type == "orderbook_snapshot") {
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, ""));
            static std::once_flag snap_dbg;
            std::call_once(snap_dbg, [&]{
                LOG_INFO("RAW orderbook_snapshot sample: {}", data.dump().substr(0, 1200));
            });
            auto snap = MetaScalpCodec::parse_orderbook_snapshot(data, ticker);
            if (!snap) { LOG_WARN("parse_orderbook_snapshot failed: {}", snap.error()); return; }
            record_delta_us(codec_hist, recv_ns);
            LOG_DEBUG("OB snapshot {}: bids={} asks={}", ticker, snap->bids.size(), snap->asks.size());
            dispatch_orderbook_snapshot_(*snap);
        } else if (type == "orderbook_update") {
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, ""));
            static std::once_flag upd_dbg;
            std::call_once(upd_dbg, [&]{
                LOG_INFO("RAW orderbook_update sample: {}", data.dump().substr(0, 1200));
            });
            auto upd = MetaScalpCodec::parse_orderbook_update(data, ticker);
            if (!upd) { LOG_WARN("parse_orderbook_update failed: {}", upd.error()); return; }
            record_delta_us(codec_hist, recv_ns);
            auto targets = get_target_listeners(ticker);
            for (auto* l : *targets) l->on_orderbook_update(*upd);
        } else if (type == "order_update") {
            auto upd = MetaScalpCodec::parse_order_update(data);
            if (!upd) { LOG_WARN("parse_order_update failed: {}", upd.error()); return; }
            record_delta_us(codec_hist, recv_ns);
            upd->ticker = from_ms(upd->ticker);
            auto targets = get_target_listeners(upd->ticker);
            for (auto* l : *targets) l->on_order_update(*upd);
        } else if (type == "position_update") {
            auto upd = MetaScalpCodec::parse_position_update(data);
            if (!upd) { LOG_WARN("parse_position_update failed: {}", upd.error()); return; }
            record_delta_us(codec_hist, recv_ns);
            upd->ticker = from_ms(upd->ticker);
            auto targets = get_target_listeners(upd->ticker);
            for (auto* l : *targets) l->on_position_update(*upd);
        } else if (type == "balance_update") {
            auto upd = MetaScalpCodec::parse_balance_update(data);
            if (!upd) { LOG_WARN("parse_balance_update failed: {}", upd.error()); return; }
            record_delta_us(codec_hist, recv_ns);
            auto targets = get_target_listeners("");
            for (auto* l : *targets) l->on_balance_update(*upd);
        } else if (type == "finres_update") {
            auto upd = MetaScalpCodec::parse_finres_update(data);
            if (!upd) { LOG_WARN("parse_finres_update failed: {}", upd.error()); return; }
            record_delta_us(codec_hist, recv_ns);
            auto targets = get_target_listeners("");
            for (auto* l : *targets) l->on_finres_update(*upd);
        } else if (type == "funding_update") {
            // PascalCase fields per MetaScalp API
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, "Ticker", ""));
            if (!ticker.empty()) {
                FundingData fd;
                fd.rate = MetaScalpCodec::get_val<double>(data, "FundingRate", 0.0);
                fd.next_funding_time = MetaScalpCodec::parse_iso8601(
                    MetaScalpCodec::get_val<std::string>(data, "FundingTime", ""));
                record_delta_us(codec_hist, recv_ns);
                fd.updated_at = fd.next_funding_time; // Q2: use exchange-provided time for updated_at where possible
                // CoW write: single WS-thread writer copies the current map,
                // mutates, and publishes via atomic store-release.
                auto next = std::make_shared<FundingMap>(*m_funding_snapshot.load(std::memory_order_acquire));
                (*next)[ticker] = fd;
                m_funding_snapshot.store(std::shared_ptr<const FundingMap>(std::move(next)),
                                         std::memory_order_release);
                LOG_DEBUG("Funding update {}: rate={:.6f}", ticker, fd.rate);
            }
        } else if (type == "mark_price_update") {
            // PascalCase fields per MetaScalp API contract
            Ticker ticker = from_ms(MetaScalpCodec::get_val<std::string>(data, "Ticker", ""));
            double mp = MetaScalpCodec::get_val<double>(data, "MarkPrice", 0.0);
            record_delta_us(codec_hist, recv_ns);
            if (!ticker.empty() && mp > 0.0) {
                // CoW write: single WS-thread writer copies, mutates, publishes.
                auto next = std::make_shared<MarkPriceMap>(*m_mark_price_snapshot.load(std::memory_order_acquire));
                (*next)[ticker] = mp;
                m_mark_price_snapshot.store(std::shared_ptr<const MarkPriceMap>(std::move(next)),
                                            std::memory_order_release);
            }
        } else if (type == "error") {
            std::string msg = MetaScalpCodec::get_val<std::string>(data, "Message", "Unknown error");
            record_delta_us(codec_hist, recv_ns);
            LOG_ERROR("WS API error: {}", msg);
            auto targets = get_target_listeners("");
            for (auto* l : *targets) l->on_error(msg);
        }
    }
}

void MarketDataFeed::set_record_tap(RawTap tap) {
    m_record_tap = std::move(tap);
}

} // namespace trade_bot
