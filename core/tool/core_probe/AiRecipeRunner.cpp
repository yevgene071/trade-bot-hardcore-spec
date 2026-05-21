#include "AiRecipeRunner.hpp"
#include "TraceLogger.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

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

/// Second-pass over the trace JSONL to collect sample trace_ids per
/// rejection reason.  Deterministic: keeps the first N per reason.
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
    auto strat_it = opts.ai_recipe_args.find("strategy");
    if (strat_it != opts.ai_recipe_args.end()) {
        out["strategy_filter"] = strat_it->second;
    }
    auto tkr_it = opts.ai_recipe_args.find("ticker");
    if (tkr_it != opts.ai_recipe_args.end()) {
        out["ticker_filter"] = tkr_it->second;
    }

    out["plans_generated"] = s.total_plans();
    out["plans_accepted"]  = s.plans_accepted();
    out["plans_rejected"]  = s.plans_rejected();

    // Top reject reasons + sample trace_ids from JSONL (best-effort)
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
    return out;
}

json AiRecipeRunner::recipe_why_rejected(const CliOptions& opts,
                                         const SummaryCollector& s) {
    json out;
    out["recipe"] = "why-rejected";
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
