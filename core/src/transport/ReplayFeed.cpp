#include "ReplayFeed.hpp"

#include "MetaScalpCodec.hpp"
#include "logger/Logger.hpp"
#include "perf/TraceContext.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <print>

namespace trade_bot {

ReplayFeed::ReplayFeed(std::string ndjson_path,
                       std::shared_ptr<IClock> clock,
                       double speed_multiplier)
    : path_(std::move(ndjson_path))
    , clock_(std::move(clock))
    , speed_(speed_multiplier) {}

void ReplayFeed::add_listener(IMarketDataListener* listener) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) {
        listeners_.push_back(listener);
    }
}

void ReplayFeed::remove_listener(IMarketDataListener* listener) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                     listeners_.end());
}

void ReplayFeed::stop() { running_.store(false); }

ReplayFeed::Stats ReplayFeed::stats() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return stats_;
}

std::vector<IMarketDataListener*> ReplayFeed::snapshot_listeners_() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return listeners_;
}

ReplayFeed::Stats ReplayFeed::run() {
    std::ifstream in(path_);
    if (!in) {
        throw std::runtime_error("ReplayFeed: cannot open " + path_);
    }

    // Pre-scan up to the first 1000 lines to find the first trade_update and determine system_time_offset_ns_
    system_time_offset_ns_ = 0;
    {
        std::ifstream pre_in(path_);
        std::string pre_line;
        size_t lines_scanned = 0;
        while (lines_scanned < 1000 && std::getline(pre_in, pre_line)) {
            if (pre_line.empty()) continue;
            nlohmann::json envelope;
            try {
                envelope = nlohmann::json::parse(pre_line);
            } catch (...) {
                continue;
            }
            lines_scanned++;
            if (!envelope.contains("recv_ts_ns") || !envelope.contains("message")) continue;
            const int64_t recv_ts_ns = envelope.at("recv_ts_ns").get<int64_t>();
            const auto& msg = envelope.at("message");
            if (!msg.is_object() || !msg.contains("Type") || !msg.contains("Data")) continue;
            const std::string type = msg.at("Type").get<std::string>();
            if (type == "trade_update") {
                const auto& data = msg.at("Data");
                auto trades = MetaScalpCodec::parse_trade_update(data);
                if (trades && !trades->empty()) {
                    const auto& trade = trades->front();
                    const int64_t trade_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        trade.timestamp.time_since_epoch()).count();
                    system_time_offset_ns_ = trade_ts_ns - recv_ts_ns;
                    break;
                }
            }
        }
    }

    running_.store(true);

    bool first = true;
    int64_t recv_ts0_ns = 0;
    auto wall_start = clock_->now();

    std::string line;
    while (running_.load() && std::getline(in, line)) {
        if (line.empty()) continue;

        nlohmann::json envelope;
        try {
            envelope = nlohmann::json::parse(line);
        } catch (const std::exception& ex) {
            LOG_WARN("ReplayFeed: bad JSON line skipped: {}", ex.what());
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            ++stats_.parse_errors;
            continue;
        }

        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            ++stats_.messages_read;
        }

        if (!envelope.contains("recv_ts_ns") || !envelope.contains("message")) {
            LOG_WARN("ReplayFeed: line missing recv_ts_ns/message — skipped");
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            ++stats_.parse_errors;
            continue;
        }

        const int64_t recv_ts_ns = envelope.at("recv_ts_ns").get<int64_t>();
        if (first) {
            recv_ts0_ns = recv_ts_ns;
            wall_start  = clock_->now();
            first = false;
        }

        // Sleep until the scaled offset is reached.
        if (speed_ > 0.0 && !first) {
            const auto offset_ns = std::chrono::nanoseconds(recv_ts_ns - recv_ts0_ns);
            const auto scaled    = std::chrono::nanoseconds(
                static_cast<int64_t>(static_cast<double>(offset_ns.count()) / speed_));
            clock_->sleep_until(wall_start + scaled);
        }
        const auto& msg = envelope.at("message");
        const std::string raw = msg.is_string() ? msg.get<std::string>() : msg.dump();

        uint64_t trace_id = recv_ts_ns != 0 ? recv_ts_ns : 1;
        TraceContextScope trace_scope(trace_id, recv_ts_ns);
        dispatch_message_(raw, recv_ts_ns);
    }

    running_.store(false);
    return stats();
}

namespace {

// Helpers to call listener callbacks while keeping listeners snapshot stable.
template <class Fn>
void for_each_listener(std::vector<IMarketDataListener*>& ls, Fn&& fn) {
    for (auto* l : ls) {
        if (l) fn(l);
    }
}

}  // namespace

void ReplayFeed::dispatch_message_(const std::string& raw_message, int64_t recv_ts_ns) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(raw_message);
    } catch (const std::exception& ex) {
        LOG_WARN("ReplayFeed: dispatch parse error: {}", ex.what());
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        ++stats_.parse_errors;
        return;
    }

    if (!j.contains("Type") || !j.contains("Data")) {
        LOG_WARN("ReplayFeed: malformed envelope (missing Type/Data)");
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        ++stats_.parse_errors;
        return;
    }

    const std::string type = j.at("Type").get<std::string>();
    const auto& data = j.at("Data");

    auto listeners = snapshot_listeners_();
    if (listeners.empty()) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        ++stats_.messages_dispatched;
        return;
    }

    try {
        if (type == "trade_update") {
            const Ticker ticker = MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, "");
            auto trades = MetaScalpCodec::parse_trade_update(data);
            if (!trades) throw CodecError(trades.error());
            for_each_listener(listeners, [&](IMarketDataListener* l) {
                l->on_trades(ticker, *trades);
            });
        } else if (type == "orderbook_snapshot") {
            const Ticker ticker = MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, "");
            auto snap = MetaScalpCodec::parse_orderbook_snapshot(data, ticker);
            if (!snap) throw CodecError(snap.error());
            if (system_time_offset_ns_ != 0) {
                snap->ts = std::chrono::system_clock::time_point(
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::nanoseconds(recv_ts_ns + system_time_offset_ns_)
                    )
                );
            }
            for_each_listener(listeners, [&](IMarketDataListener* l) {
                l->on_orderbook_snapshot(*snap);
            });
        } else if (type == "orderbook_update") {
            const Ticker ticker = MetaScalpCodec::get_val<std::string>(data, api::fields::kTicker, "");
            auto upd = MetaScalpCodec::parse_orderbook_update(data, ticker);
            if (!upd) throw CodecError(upd.error());
            if (system_time_offset_ns_ != 0) {
                upd->ts = std::chrono::system_clock::time_point(
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::nanoseconds(recv_ts_ns + system_time_offset_ns_)
                    )
                );
            }
            for_each_listener(listeners, [&](IMarketDataListener* l) {
                l->on_orderbook_update(*upd);
            });
        } else if (type == "order_update") {
            auto upd = MetaScalpCodec::parse_order_update(data);
            if (!upd) throw CodecError(upd.error());
            for_each_listener(listeners, [&](IMarketDataListener* l) {
                l->on_order_update(*upd);
            });
        } else if (type == "position_update") {
            auto upd = MetaScalpCodec::parse_position_update(data);
            if (!upd) throw CodecError(upd.error());
            for_each_listener(listeners, [&](IMarketDataListener* l) {
                l->on_position_update(*upd);
            });
        } else if (type == "balance_update") {
            auto upd = MetaScalpCodec::parse_balance_update(data);
            if (!upd) throw CodecError(upd.error());
            for_each_listener(listeners, [&](IMarketDataListener* l) {
                l->on_balance_update(*upd);
            });
        } else {
            // Unknown / unsupported types are silently skipped — counted as
            // dispatched so the test can verify total throughput.
        }
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        ++stats_.messages_dispatched;
    } catch (const std::exception& ex) {
        LOG_WARN("ReplayFeed: codec error on type={}: {}", type, ex.what());
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        ++stats_.parse_errors;
    }
}

}  // namespace trade_bot
