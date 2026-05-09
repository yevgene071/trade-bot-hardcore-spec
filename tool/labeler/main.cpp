// T2-LABELING: Interactive CLI labeler for signal detectors.
// Replays an NDJSON market-data dump, runs all detectors offline,
// and lets the operator mark each fired signal as TP / FP / skip.
//
// Usage:
//   labeler --dump replay/dumps/btcusdt_5min.ndjson --ticker BTCUSDT
//           [--detector density|iceberg|tape|level|approach]
//           [--out replay/labels]

#include "transport/MetaScalpCodec.hpp"
#include "transport/IHttpClient.hpp"
#include "transport/ClusterSnapshotClient.hpp"
#include "marketdata/OrderBook.hpp"
#include "marketdata/TradeStream.hpp"
#include "marketdata/ClusterSnapshot.hpp"
#include "features/FeatureExtractor.hpp"
#include "signals/SignalBus.hpp"
#include "signals/Signal.hpp"
#include "signals/DensityDetector.hpp"
#include "signals/IcebergDetector.hpp"
#include "signals/TapeAnalyzer.hpp"
#include "signals/LevelDetector.hpp"
#include "signals/ApproachAnalyzer.hpp"
#include "universe/TickerUniverse.hpp"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Null HTTP client — labeler runs fully offline, no MetaScalp required.
class NullHttpClient final : public trade_bot::IHttpClient {
public:
    trade_bot::HttpResponse get(const std::string&) override              { return {0, "", {}}; }
    trade_bot::HttpResponse post(const std::string&, const std::string&) override { return {0, "", {}}; }
    trade_bot::HttpResponse put(const std::string&, const std::string&) override  { return {0, "", {}}; }
    trade_bot::HttpResponse del(const std::string&) override              { return {0, "", {}}; }
    void set_timeout_ms(int) override {}
};

std::string kind_to_str(trade_bot::SignalKind k) {
    using K = trade_bot::SignalKind;
    switch (k) {
        case K::DensityDetected:   return "DensityDetected";
        case K::DensityRemoved:    return "DensityRemoved";
        case K::DensityEating:     return "DensityEating";
        case K::IcebergSuspected:  return "IcebergSuspected";
        case K::TapeBurst:         return "TapeBurst";
        case K::TapeFade:          return "TapeFade";
        case K::TapeFlush:         return "TapeFlush";
        case K::TapeDistribution:  return "TapeDistribution";
        case K::LevelFormed:       return "LevelFormed";
        case K::LevelApproach:     return "LevelApproach";
        case K::LevelRejection:    return "LevelRejection";
        case K::LevelBreak:        return "LevelBreak";
        case K::LeaderMove:        return "LeaderMove";
        default:                   return "Unknown";
    }
}

std::string kind_to_detector(trade_bot::SignalKind k) {
    using K = trade_bot::SignalKind;
    switch (k) {
        case K::DensityDetected: case K::DensityRemoved: case K::DensityEating:
            return "density";
        case K::IcebergSuspected:
            return "iceberg";
        case K::TapeBurst: case K::TapeFade: case K::TapeFlush: case K::TapeDistribution:
            return "tape";
        case K::LevelFormed: case K::LevelApproach: case K::LevelRejection: case K::LevelBreak:
            return "level";
        case K::LeaderMove:
            return "leader";
        default:
            return "unknown";
    }
}

void append_label(const std::string& path, const nlohmann::json& entry) {
    std::ofstream out(path, std::ios::app);
    out << entry.dump() << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string dump_path;
    std::string ticker;
    std::string filter_detector; // empty = all
    std::string out_dir = "replay/labels";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--dump"     && i + 1 < argc) dump_path        = argv[++i];
        else if (a == "--ticker"   && i + 1 < argc) ticker           = argv[++i];
        else if (a == "--detector" && i + 1 < argc) filter_detector  = argv[++i];
        else if (a == "--out"      && i + 1 < argc) out_dir          = argv[++i];
    }

    if (dump_path.empty() || ticker.empty()) {
        std::cerr << "Usage: labeler --dump <ndjson> --ticker <TICKER>"
                     " [--detector density|iceberg|tape|level|approach]"
                     " [--out replay/labels]\n";
        return 1;
    }

    std::ifstream dump(dump_path);
    if (!dump) { std::cerr << "Cannot open dump: " << dump_path << "\n"; return 1; }

    std::filesystem::create_directories(out_dir);

    auto label_path = [&](const std::string& det) {
        return out_dir + "/" + ticker + "_" + det + "_labels.jsonl";
    };

    // --- Infrastructure (offline, no MetaScalp) ---
    NullHttpClient null_http;
    trade_bot::ClusterSnapshotClient null_cluster(null_http, "", 0);
    trade_bot::ClusterSnapshotManager cluster_mgr(null_cluster); // not started → no polling

    trade_bot::TickerUniverse universe;
    trade_bot::OrderBook      book(ticker, 0.01, 1e-6);
    trade_bot::TradeStream    stream(ticker);
    trade_bot::FeatureExtractor extractor(ticker);
    extractor.set_sources(&book, &stream);

    trade_bot::SignalBus bus;

    auto density  = std::make_unique<trade_bot::DensityDetector>(ticker, bus, book, universe);
    auto iceberg  = std::make_unique<trade_bot::IcebergDetector>(ticker, bus, book, universe);
    auto tape     = std::make_unique<trade_bot::TapeAnalyzer>(ticker, bus, book, stream);
    auto level    = std::make_unique<trade_bot::LevelDetector>(ticker, bus, book, cluster_mgr);
    auto approach = std::make_unique<trade_bot::ApproachAnalyzer>(ticker, bus, book, *level);

    std::vector<trade_bot::IDetector*> detectors{
        density.get(), iceberg.get(), tape.get(), level.get(), approach.get()
    };

    // Collect signals fired during a tick so we can prompt after the tick.
    struct Pending { trade_bot::Signal sig; int64_t recv_ts_ns; };
    std::vector<Pending> pending;
    int64_t current_recv_ns = 0;

    bus.subscribe([&](const trade_bot::Signal& s) {
        if (!filter_detector.empty() && kind_to_detector(s.kind) != filter_detector) return;
        pending.push_back({s, current_recv_ns});
    });

    // --- Replay loop ---
    using namespace std::chrono;
    system_clock::time_point last_tick{};
    bool first = true;
    size_t msg_count = 0;
    size_t labeled  = 0;

    std::cout << "=== trade_bot labeler ===\n"
              << "  dump:     " << dump_path << "\n"
              << "  ticker:   " << ticker    << "\n"
              << "  detector: " << (filter_detector.empty() ? "all" : filter_detector) << "\n"
              << "  out:      " << out_dir   << "\n\n"
              << "Controls:  y=true_positive  n=false_positive  s=skip  "
                 "m=mark_missed_positive  q=quit\n\n";

    std::string line;
    while (std::getline(dump, line)) {
        if (line.empty()) continue;

        nlohmann::json j;
        try { j = nlohmann::json::parse(line); } catch (...) { continue; }

        current_recv_ns = j.value("recv_ts_ns", int64_t{0});
        auto ts = system_clock::time_point(nanoseconds(current_recv_ns));

        if (first) { last_tick = ts; first = false; }

        // Dispatch market data for this ticker
        if (j.contains("message")) {
            const auto& msg  = j["message"];
            std::string type = msg.value("Type", "");
            const auto& data = msg.contains("Data") ? msg["Data"] : nlohmann::json::object();
            try {
                if (type == "trade_update" && data.value("Ticker", "") == ticker) {
                    auto trades = trade_bot::MetaScalpCodec::parse_trade_update(data);
                    for (const auto& tr : trades) {
                        stream.on_trade(tr);
                        for (auto* d : detectors) d->on_trade(tr);
                    }
                } else if (type == "orderbook_snapshot" && data.value("Ticker", "") == ticker) {
                    auto snap = trade_bot::MetaScalpCodec::parse_orderbook_snapshot(data, ticker);
                    book.apply_snapshot(snap);
                    // Notify detectors about full reset
                    trade_bot::OrderBookUpdate dummy{ticker, {}, ts};
                    for (auto* d : detectors) d->on_book_update(dummy);
                } else if (type == "orderbook_update" && data.value("Ticker", "") == ticker) {
                    auto upd = trade_bot::MetaScalpCodec::parse_orderbook_update(data, ticker);
                    book.apply_update(upd);
                    for (auto* d : detectors) d->on_book_update(upd);
                }
            } catch (...) {}
        }

        // Drive 10 Hz tick cadence from replay timestamps
        while (ts - last_tick >= milliseconds(100)) {
            last_tick += milliseconds(100);
            stream.update(last_tick);
            auto frame = extractor.extract(last_tick);
            for (auto* d : detectors) d->on_frame(frame);
        }

        ++msg_count;

        // Process any signals that fired during this message
        for (const auto& ps : pending) {
            const auto& s   = ps.sig;
            std::string det = kind_to_detector(s.kind);
            auto ts_ns      = duration_cast<nanoseconds>(s.timestamp.time_since_epoch()).count();
            auto ts_sec     = system_clock::to_time_t(s.timestamp);
            char ts_buf[32];
            struct tm tm_info{};
            gmtime_r(&ts_sec, &tm_info);
            strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

            std::cout << "\033[33m[" << ts_buf << "] " << kind_to_str(s.kind)
                      << " @ " << s.ticker
                      << "  price=" << s.price
                      << "  conf=" << s.confidence << "\033[0m\n";
            if (!s.payload.is_null() && !s.payload.empty())
                std::cout << "  payload: " << s.payload.dump() << "\n";
            std::cout << "  > ";
            std::cout.flush();

            char ch = 's';
            // Read a single non-newline character
            while (std::cin.get(ch) && ch == '\n') {}

            if (ch == 'q') {
                std::cout << "\nQuitting. Labeled: " << labeled << "\n";
                return 0;
            }

            std::string notes;
            if (ch == 'y' || ch == 'n') {
                std::cout << "  notes (Enter to skip): ";
                std::getline(std::cin, notes);
                nlohmann::json entry{
                    {"timestamp_ns",     ts_ns},
                    {"recv_ts_ns",       ps.recv_ts_ns},
                    {"signal_kind",      kind_to_str(s.kind)},
                    {"is_true_positive", ch == 'y'},
                    {"evidence_notes",   notes}
                };
                append_label(label_path(det), entry);
                ++labeled;
            } else if (ch == 'm') {
                // Manually mark a positive the detector fired but operator wants to override
                std::cout << "  notes for missed positive: ";
                std::getline(std::cin, notes);
                nlohmann::json entry{
                    {"timestamp_ns",     ts_ns},
                    {"recv_ts_ns",       ps.recv_ts_ns},
                    {"signal_kind",      kind_to_str(s.kind)},
                    {"is_true_positive", true},
                    {"manually_added",   true},
                    {"evidence_notes",   notes}
                };
                append_label(label_path(det), entry);
                ++labeled;
            }
            // 's' = skip, write nothing
        }
        pending.clear();
    }

    std::cout << "\nReplay complete. Messages processed: " << msg_count
              << "  Labels saved: " << labeled << "\n";
    return 0;
}
