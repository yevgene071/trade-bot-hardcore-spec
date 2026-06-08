#include "AiRecipeRunner.hpp"
#include "TraceLogger.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <optional>

namespace trade_bot::probe {

using nlohmann::json;

std::vector<std::string> AiRecipeRunner::available_recipes() {
    return {
        "why-no-trade",
        "why-rejected",
        "slow-stage",
        "invariant-summary",
        "signal-strategy-gap",
    };
}

namespace {

/// Sort a (label, count) map descending by count, returning top-N as JSON array
/// of {name, count}.  Stable for deterministic output: ties broken by label.
json top_n(const std::map<std::string, uint64_t>& m, size_t n) {
    std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    json arr = json::array();
    for (size_t i = 0; i < v.size() && i < n; ++i) {
        arr.push_back({{"name", v[i].first}, {"count", v[i].second}});
    }
    return arr;
}

/// Case-insensitive substring match helper.
static bool matches_filter(const std::string& value, const std::string& filter) {
    if (filter.empty()) return true;
    std::string lower_v = value;
    std::string lower_f = filter;
    std::transform(lower_v.begin(), lower_v.end(), lower_v.begin(), ::tolower);
    std::transform(lower_f.begin(), lower_f.end(), lower_f.begin(), ::tolower);
    return lower_v.find(lower_f) != std::string::npos;
}

/// Aggregated counters from a filtered second pass over JSONL.
struct FilteredAggregates {
    uint64_t plans_generated = 0;
    uint64_t plans_accepted  = 0;
    uint64_t plans_rejected  = 0;
    std::map<std::string, uint64_t> rejections_by_reason;
    /// Sample trace_ids per rejection reason (first N per reason).
    std::map<std::string, std::vector<uint64_t>> sample_trace_ids;
};

/// Comprehensive second pass over JSONL with optional strategy/ticker filters.
///
/// Strategy filter: pre-scans strategy events for matching trace_ids, then
/// filters risk events by those trace_ids (risk events lack a strategy field).
/// Ticker filter: straightforward match on top-level "ticker" field.
///
/// When both filters are nullopt, returns empty aggregates (caller should use
/// SummaryCollector for the fast path).
FilteredAggregates filtered_second_pass(const std::string& jsonl_path,
                                        const std::optional<std::string>& strategy_filter,
                                        const std::optional<std::string>& ticker_filter,
                                        size_t samples_per_reason) {
    FilteredAggregates out;
    if (jsonl_path.empty()) return out;
    if (!strategy_filter && !ticker_filter) return out; // fast path: use SummaryCollector

    // ── Pre-pass: collect trace_ids from strategy events matching strategy_filter ──
    std::set<uint64_t> matched_trace_ids;
    if (strategy_filter) {
        std::ifstream pre_in(jsonl_path);
        if (!pre_in) return out;
        std::string line;
        while (std::getline(pre_in, line)) {
            if (line.empty()) continue;
            try {
                auto j = json::parse(line);
                if (!j.is_object()) continue;
                if (j.value("stage", "") != "strategy") continue;
                if (ticker_filter && !matches_filter(j.value("ticker", ""), *ticker_filter)) continue;
                // Payload fields are merged at top level by to_machine_string() —
                // no nested "payload" key exists in JSONL.
                std::string strat_name = j.value("strategy", "");
                if (matches_filter(strat_name, *strategy_filter)) {
                    matched_trace_ids.insert(j.value("trace_id", uint64_t{0}));
                }
            } catch (...) { /* skip malformed */ }
        }
        if (matched_trace_ids.empty()) return out; // no matches → all zeros
    }

    // ── Main pass: accumulate strategy + risk events ──
    std::ifstream in(jsonl_path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            if (!j.is_object()) continue;

            std::string stage = j.value("stage", "");
            if (stage != "strategy" && stage != "risk") continue;

            // Ticker filter
            if (ticker_filter && !matches_filter(j.value("ticker", ""), *ticker_filter)) continue;

            // Strategy filter for risk events: cross-reference via trace_id
            if (strategy_filter && stage == "risk") {
                if (!matched_trace_ids.count(j.value("trace_id", uint64_t{0}))) continue;
            }

            if (stage == "strategy") {
                // Also filter strategy events by strategy name when filter is active.
                // (Risk events are filtered via matched_trace_ids above.)
                if (strategy_filter) {
                    // Payload fields merged at top level by to_machine_string().
                    std::string strat_name = j.value("strategy", "");
                    if (!matches_filter(strat_name, *strategy_filter)) continue;
                }
                ++out.plans_generated;
            } else if (stage == "risk") {
                bool accepted = j.value("accepted", false);
                if (accepted) {
                    ++out.plans_accepted;
                } else {
                    ++out.plans_rejected;
                    std::string reason = j.value("reason", "");
                    if (!reason.empty()) {
                        ++out.rejections_by_reason[reason];
                        auto& bucket = out.sample_trace_ids[reason];
                        if (bucket.size() < samples_per_reason) {
                            bucket.push_back(j.value("trace_id", uint64_t{0}));
                        }
                    }
                }
            }
        } catch (...) {
            // ignore malformed lines — non-fatal in second-pass analysis
        }
    }
    return out;
}

/// Second-pass over the trace JSONL to collect sample trace_ids per
/// rejection reason.  Deterministic: keeps the first N per reason.
/// When strategy/ticker filters are provided, only matching events are included.
std::map<std::string, std::vector<uint64_t>>
sample_trace_ids_per_reason(const std::string& jsonl_path,
                            const std::set<std::string>& wanted_reasons,
                            size_t per_reason) {
    std::map<std::string, std::vector<uint64_t>> out;
    if (jsonl_path.empty() || wanted_reasons.empty()) return out;
    std::ifstream in(jsonl_path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            if (!j.is_object()) continue;
            if (j.value("stage", "") != "risk") continue;
            if (j.value("accepted", false)) continue;
            std::string reason = j.value("reason", "");
            if (!wanted_reasons.count(reason)) continue;
            auto& bucket = out[reason];
            if (bucket.size() < per_reason) {
                bucket.push_back(j.value("trace_id", uint64_t{0}));
            }
        } catch (...) {
            // ignore malformed lines — non-fatal in second-pass analysis
        }
    }
    return out;
}

} // namespace

// ── Recipes ──────────────────────────────────────────────────────────────────

json AiRecipeRunner::recipe_why_no_trade(const CliOptions& opts,
                                         const SummaryCollector& s) {
    json out;
    out["recipe"] = "why-no-trade";

    // Extract filter args
    std::optional<std::string> strategy_filter;
    std::optional<std::string> ticker_filter;
    auto strat_it = opts.ai_recipe_args.find("strategy");
    if (strat_it != opts.ai_recipe_args.end() && !strat_it->second.empty()) {
        strategy_filter = strat_it->second;
        out["strategy_filter"] = strat_it->second;
    }
    auto tkr_it = opts.ai_recipe_args.find("ticker");
    if (tkr_it != opts.ai_recipe_args.end() && !tkr_it->second.empty()) {
        ticker_filter = tkr_it->second;
        out["ticker_filter"] = tkr_it->second;
    }

    if (strategy_filter || ticker_filter) {
        // ── Filtered path: second pass over JSONL ──
        auto agg = filtered_second_pass(opts.jsonl_out, strategy_filter, ticker_filter, 3);
        out["plans_generated"] = agg.plans_generated;
        out["plans_accepted"]  = agg.plans_accepted;
        out["plans_rejected"]  = agg.plans_rejected;
        out["top_reject_reasons"] = top_n(agg.rejections_by_reason, 5);
        if (!agg.sample_trace_ids.empty()) {
            json sj = json::object();
            for (const auto& [r, ids] : agg.sample_trace_ids) sj[r] = ids;
            out["sample_trace_ids"] = sj;
        }
        // Hint
        std::string hint;
        if (agg.plans_generated == 0) {
            hint = "No plans matched the filter(s). Check --ai-recipe-arg "
                   "strategy/ticker values against actual trace data.";
        } else if (agg.plans_accepted == 0 && agg.plans_rejected > 0) {
            hint = "All filtered plans were rejected by Risk. Top reason: ";
            std::pair<std::string, uint64_t> best{"", 0};
            for (const auto& [k, v] : agg.rejections_by_reason) if (v > best.second) best = {k, v};
            hint += (best.first.empty() ? "(unknown)" : best.first) + ".";
        } else {
            hint = "Some filtered plans accepted. If volume looks low, check --risk-observe.";
        }
        out["hint"] = hint;
    } else {
        // ── Fast path: use in-memory SummaryCollector ──
        out["plans_generated"] = s.total_plans();
        out["plans_accepted"]  = s.plans_accepted();
        out["plans_rejected"]  = s.plans_rejected();

        out["top_reject_reasons"] = top_n(s.rejections_by_reason(), 5);

        std::set<std::string> wanted;
        for (const auto& [name, _] : s.rejections_by_reason()) wanted.insert(name);
        auto samples = sample_trace_ids_per_reason(opts.jsonl_out, wanted, 3);
        if (!samples.empty()) {
            json sj = json::object();
            for (const auto& [r, ids] : samples) sj[r] = ids;
            out["sample_trace_ids"] = sj;
        }

        // Hint
        std::string hint;
        if (s.total_plans() == 0) {
            hint = "No plans were generated. Check signals: ";
            if (s.total_signals() == 0) {
                hint += "no signals fired either — check detectors and input data.";
            } else {
                hint += "signals fire but no strategy reacts. Review --strategies "
                        "filter and per-strategy preconditions.";
            }
        } else if (s.plans_accepted() == 0) {
            hint = "Plans generated but all rejected by Risk. Top reason: ";
            auto top = s.rejections_by_reason();
            std::pair<std::string, uint64_t> best{"", 0};
            for (const auto& [k, v] : top) if (v > best.second) best = {k, v};
            hint += (best.first.empty() ? "(unknown)" : best.first) + ".";
        } else {
            hint = "Some plans accepted. If volume looks low, check --risk-observe "
                   "to compare with-vs-without risk gating.";
        }
        out["hint"] = hint;
    }
    return out;
}

json AiRecipeRunner::recipe_why_rejected(const CliOptions& opts,
                                         const SummaryCollector& s) {
    json out;
    out["recipe"] = "why-rejected";

    // Extract filter args
    std::optional<std::string> strategy_filter;
    std::optional<std::string> ticker_filter;
    auto strat_it = opts.ai_recipe_args.find("strategy");
    if (strat_it != opts.ai_recipe_args.end() && !strat_it->second.empty()) {
        strategy_filter = strat_it->second;
        out["strategy_filter"] = strat_it->second;
    }
    auto tkr_it = opts.ai_recipe_args.find("ticker");
    if (tkr_it != opts.ai_recipe_args.end() && !tkr_it->second.empty()) {
        ticker_filter = tkr_it->second;
        out["ticker_filter"] = tkr_it->second;
    }

    if (strategy_filter || ticker_filter) {
        // ── Filtered path: second pass over JSONL ──
        auto agg = filtered_second_pass(opts.jsonl_out, strategy_filter, ticker_filter, 5);
        out["plans_rejected"] = agg.plans_rejected;
        out["rejections_by_reason"] = agg.rejections_by_reason;
        out["top_reject_reasons"] = top_n(agg.rejections_by_reason, 10);
        if (!agg.sample_trace_ids.empty()) {
            json sj = json::object();
            for (const auto& [r, ids] : agg.sample_trace_ids) sj[r] = ids;
            out["sample_trace_ids"] = sj;
        }
    } else {
        // ── Fast path: use in-memory SummaryCollector ──
        out["plans_rejected"] = s.plans_rejected();
        out["rejections_by_reason"] = s.rejections_by_reason();
        out["top_reject_reasons"] = top_n(s.rejections_by_reason(), 10);

        std::set<std::string> wanted;
        for (const auto& [name, _] : s.rejections_by_reason()) wanted.insert(name);
        auto samples = sample_trace_ids_per_reason(opts.jsonl_out, wanted, 5);
        if (!samples.empty()) {
            json sj = json::object();
            for (const auto& [r, ids] : samples) sj[r] = ids;
            out["sample_trace_ids"] = sj;
        }
    }
    return out;
}

json AiRecipeRunner::recipe_slow_stage(const CliOptions& /*opts*/,
                                       const SummaryCollector& /*s*/) {
    // PerfRegistry has its own format but we don't get it through SummaryCollector.
    // For V1 we expose a stub that points at the latency report.  A future pass
    // can integrate hdr_histogram percentiles directly.
    json out;
    out["recipe"] = "slow-stage";
    out["status"] = "unimplemented_v1";
    out["hint"]   = "Run with --machine and grep for stage='perf' in the JSONL "
                    "summary, or read the textual latency report at end of run.";
    return out;
}

json AiRecipeRunner::recipe_invariant_summary(const CliOptions& /*opts*/,
                                              const SummaryCollector& s) {
    json out;
    out["recipe"] = "invariant-summary";
    out["total"] = s.invariant_total();
    out["by_code"] = s.invariants_by_code();
    out["top_codes"] = top_n(s.invariants_by_code(), 10);
    if (s.invariant_total() == 0) {
        out["hint"] = "No invariants violated. Pipeline data integrity OK.";
    } else {
        out["hint"] = "Invariants triggered. Inspect JSONL for stage='invariant' "
                      "events with the listed codes for trace_id and ticker.";
    }
    return out;
}

json AiRecipeRunner::recipe_signal_strategy_gap(const CliOptions& /*opts*/,
                                                const SummaryCollector& s) {
    json out;
    out["recipe"] = "signal-strategy-gap";
    out["signals_by_kind"]    = s.signals_by_kind();
    out["plans_by_strategy"]  = s.plans_by_strategy();
    out["total_signals"]      = s.total_signals();
    out["total_plans"]        = s.total_plans();

    if (s.total_signals() == 0) {
        out["hint"] = "No signals at all. Detectors silent or input has no qualifying events.";
    } else if (s.total_plans() == 0) {
        out["hint"] = "Signals present but strategies emitted no plans. Check "
                      "per-strategy entry conditions / affinity thresholds.";
    } else {
        double ratio = static_cast<double>(s.total_plans())
                     / static_cast<double>(s.total_signals());
        out["plan_per_signal_ratio"] = ratio;
        out["hint"] = "Compare per-kind signal counts with per-strategy plan "
                      "counts to localize which signal type strategies ignore.";
    }
    return out;
}

// ── Dispatcher ───────────────────────────────────────────────────────────────

int AiRecipeRunner::run(const std::string& recipe_name,
                        const CliOptions& opts,
                        const SummaryCollector& summary) {
    // Allow async TraceLogger writer thread to flush before we consume JSONL.
    // ReplayRunner already calls shutdown() — this is a defensive no-op there.
    json result;
    if (recipe_name == "why-no-trade") {
        result = recipe_why_no_trade(opts, summary);
    } else if (recipe_name == "why-rejected") {
        result = recipe_why_rejected(opts, summary);
    } else if (recipe_name == "slow-stage") {
        result = recipe_slow_stage(opts, summary);
    } else if (recipe_name == "invariant-summary") {
        result = recipe_invariant_summary(opts, summary);
    } else if (recipe_name == "signal-strategy-gap") {
        result = recipe_signal_strategy_gap(opts, summary);
    } else {
        std::cerr << "Error: unknown AI recipe '" << recipe_name << "'.\n"
                  << "Available recipes:\n";
        for (const auto& r : available_recipes()) {
            std::cerr << "  " << r << "\n";
        }
        return 1;
    }

    std::cout << result.dump(2) << "\n";
    return 0;
}

} // namespace trade_bot::probe
