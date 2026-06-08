#include "DiffRunner.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include <cmath>
#include <nlohmann/json.hpp>

namespace trade_bot::probe {

namespace {

using json = nlohmann::json;

// Composite key for indexing events: (trace_id, stage, ticker)
using EventKey = std::tuple<uint64_t, std::string, std::string>;

// Whitelisted comparison fields per stage.
// Only these fields are compared to avoid noise from timestamps and order-dependent data.
const std::map<std::string, std::vector<std::string>>& whitelisted_fields() {
    static const std::map<std::string, std::vector<std::string>> fields = {
        {"book",     {"mid", "spread_bps", "imbalance"}},
        {"trades",   {"price", "size", "side"}},
        {"features", {"mid", "spread_bps", "imbalance", "volatility_bps", "leader_correlation"}},
        {"signal",   {"kind", "price", "confidence", "side", "size_usd"}},
        {"strategy", {"strategy", "side", "entry_price", "stop_price", "tp1_price"}},
        {"risk",     {"accepted", "reason"}},
        {"executor", {"action", "entry_price", "exit_price", "size_filled", "pnl_usd"}},
        {"account",  {"equity_usd", "realized_pnl_usd"}},
    };
    return fields;
}

bool values_equal(const json& a, const json& b, double epsilon = 1e-9) {
    if (a.type() != b.type()) return false;
    if (a.is_number_float() && b.is_number_float()) {
        return std::abs(a.get<double>() - b.get<double>()) < epsilon;
    }
    return a == b;
}

struct DiffEntry {
    EventKey key;
    std::string field;
    std::string left_val;
    std::string right_val;
};

std::map<EventKey, std::vector<json>> load_jsonl(const std::string& path, size_t& count) {
    std::map<EventKey, std::vector<json>> index;
    std::ifstream file(path);
    if (!file.is_open()) return index;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            std::string stage = j.value("stage", "");
            // Skip meta and summary — they contain timestamps and are expected to differ
            if (stage == "meta" || stage == "summary") continue;

            uint64_t trace_id = j.value("trace_id", uint64_t(0));
            std::string ticker = j.value("ticker", "");
            EventKey key{trace_id, stage, ticker};
            index[key].push_back(std::move(j));
            ++count;
        } catch (...) {
            // Skip malformed lines
        }
    }
    return index;
}

} // namespace

int DiffRunner::run(const CliOptions& opts) {
    if (opts.left_path.empty() || opts.right_path.empty()) {
        std::cerr << "Error: --left and --right are required for diff subcommand.\n";
        return 1;
    }

    size_t left_count = 0, right_count = 0;
    auto left_index = load_jsonl(opts.left_path, left_count);
    auto right_index = load_jsonl(opts.right_path, right_count);

    if (left_index.empty()) {
        std::cerr << "Error: could not load or parse left file: " << opts.left_path << "\n";
        return 1;
    }
    if (right_index.empty()) {
        std::cerr << "Error: could not load or parse right file: " << opts.right_path << "\n";
        return 1;
    }

    // Build union of all keys
    std::set<EventKey> all_keys;
    for (const auto& [k, _] : left_index) all_keys.insert(k);
    for (const auto& [k, _] : right_index) all_keys.insert(k);

    std::vector<DiffEntry> diffs;
    size_t extra_left = 0, extra_right = 0;

    for (const auto& key : all_keys) {
        auto lit = left_index.find(key);
        auto rit = right_index.find(key);

        if (lit == left_index.end()) {
            extra_right += (rit != right_index.end() ? rit->second.size() : 0);
            continue;
        }
        if (rit == right_index.end()) {
            extra_left += lit->second.size();
            continue;
        }

        // Compare whitelisted fields — compare pairwise within the event vectors.
        // If vector sizes differ, flag the size mismatch and compare up to min size.
        const auto& [trace_id, stage, ticker] = key;
        auto wit = whitelisted_fields().find(stage);
        if (wit == whitelisted_fields().end()) continue;

        const auto& left_vec = lit->second;
        const auto& right_vec = rit->second;
        size_t n = std::min(left_vec.size(), right_vec.size());

        if (left_vec.size() != right_vec.size()) {
            diffs.push_back({key, "_count", std::to_string(left_vec.size()), std::to_string(right_vec.size())});
        }

        for (size_t ei = 0; ei < n; ++ei) {
            for (const auto& field : wit->second) {
                auto lval = left_vec[ei].value(field, json());
                auto rval = right_vec[ei].value(field, json());
                if (!values_equal(lval, rval)) {
                    diffs.push_back({key, field, lval.dump(), rval.dump()});
                }
            }
        }
    }

    bool has_differences = !diffs.empty() || extra_left > 0 || extra_right > 0;

    if (opts.machine) {
        // Machine mode: output as JSONL
        json result;
        result["stage"] = "diff_result";
        result["left_file"] = opts.left_path;
        result["right_file"] = opts.right_path;
        result["left_events"] = left_count;
        result["right_events"] = right_count;
        result["extra_in_left"] = extra_left;
        result["extra_in_right"] = extra_right;
        result["field_differences"] = diffs.size();
        result["identical"] = !has_differences;

        json diff_arr = json::array();
        for (const auto& d : diffs) {
            const auto& [trace_id, stage, ticker] = d.key;
            diff_arr.push_back({
                {"trace_id", trace_id},
                {"stage", stage},
                {"ticker", ticker},
                {"field", d.field},
                {"left", d.left_val},
                {"right", d.right_val}
            });
        }
        result["differences"] = diff_arr;
        std::cout << result.dump() << "\n";
    } else {
        // Human mode
        std::cout << "=== core_probe diff ===\n"
                  << "  left:  " << opts.left_path << " (" << left_count << " events)\n"
                  << "  right: " << opts.right_path << " (" << right_count << " events)\n\n";

        if (!has_differences) {
            std::cout << "Result: IDENTICAL ✓\n";
        } else {
            std::cout << "Result: DIFFERENCES FOUND\n\n";

            if (extra_left > 0) {
                std::cout << "  Events only in left:  " << extra_left << "\n";
            }
            if (extra_right > 0) {
                std::cout << "  Events only in right: " << extra_right << "\n";
            }

            if (!diffs.empty()) {
                std::cout << "\n  Field differences (" << diffs.size() << "):\n";
                size_t shown = 0;
                for (const auto& d : diffs) {
                    const auto& [trace_id, stage, ticker] = d.key;
                    std::cout << "    [trace=" << trace_id << "] [" << ticker << "] ["
                              << stage << "] " << d.field << ": "
                              << d.left_val << " → " << d.right_val << "\n";
                    if (++shown >= 50) {
                        std::cout << "    ... and " << (diffs.size() - shown) << " more\n";
                        break;
                    }
                }
            }
        }
    }

    return has_differences ? 2 : 0;
}

} // namespace trade_bot::probe
