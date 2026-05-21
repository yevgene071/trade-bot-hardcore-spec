#include "AssertRunner.hpp"
#include "ProbePipeline.hpp"
#include "TraceLogger.hpp"

#include "transport/IClock.hpp"
#include "transport/ReplayFeed.hpp"

#include <chrono>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace trade_bot::probe {

namespace {

// Returns system time but skips all sleep_until calls → max-speed replay.
class NullSleepClock final : public trade_bot::IClock {
public:
    time_point now() const override { return std::chrono::system_clock::now(); }
    void sleep_until(time_point) override {}
};

// Parse assertion DSL: "<path> <op> <number>"
struct Expectation {
    std::string path; // e.g. "signals.DensityDetected" or "invariants.total"
    std::string op;   // ==, !=, >=, <=, >, <
    double value;
    std::string raw;  // original expression for diagnostics
};

bool parse_expectation(const std::string& expr, Expectation& out) {
    // Match: identifier(.identifier)* <op> number
    static const std::regex re(R"(^\s*([a-zA-Z_][a-zA-Z0-9_]*(?:\.[a-zA-Z0-9_]+)*)\s*(==|!=|>=|<=|>|<)\s*(-?[0-9]+(?:\.[0-9]+)?)\s*$)");
    std::smatch m;
    if (!std::regex_match(expr, m, re)) return false;
    out.path = m[1].str();
    out.op = m[2].str();
    out.value = std::stod(m[3].str());
    out.raw = expr;
    return true;
}

// Resolve a dotted path against the summary JSON.
// Supports: signals.<Kind>, risk.rejected.<Reason>, invariants.total,
//           invariants.<code>, executor.*, plans.*, messages_*, latency.*
bool resolve_path(const nlohmann::json& summary, const std::string& path, double& out) {
    // Split path by dots
    std::vector<std::string> parts;
    std::istringstream iss(path);
    std::string part;
    while (std::getline(iss, part, '.')) {
        parts.push_back(part);
    }
    if (parts.empty()) return false;

    // Navigate the JSON
    const nlohmann::json* cursor = &summary;
    for (const auto& p : parts) {
        if (cursor->is_object() && cursor->contains(p)) {
            cursor = &(*cursor)[p];
        } else {
            // Check flattened keys like "messages_parsed"
            std::string flat_key;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) flat_key += "_";
                flat_key += parts[i];
            }
            if (summary.contains(flat_key) && summary[flat_key].is_number()) {
                out = summary[flat_key].get<double>();
                return true;
            }
            return false;
        }
    }
    if (cursor->is_number()) {
        out = cursor->get<double>();
        return true;
    }
    return false;
}

bool evaluate(const std::string& op, double actual, double expected) {
    if (op == "==") return actual == expected;
    if (op == "!=") return actual != expected;
    if (op == ">=") return actual >= expected;
    if (op == "<=") return actual <= expected;
    if (op == ">")  return actual > expected;
    if (op == "<")  return actual < expected;
    return false;
}

} // namespace

int AssertRunner::run(const CliOptions& opts) {
    if (opts.dump_path.empty()) {
        std::cerr << "Error: --dump is required for assert subcommand.\n";
        return 1;
    }
    if (opts.tickers.empty()) {
        std::cerr << "Error: --ticker is required for assert subcommand.\n";
        return 1;
    }
    if (opts.expectations.empty()) {
        std::cerr << "Error: at least one --expect is required for assert subcommand.\n";
        return 1;
    }

    // Parse all expectations first
    std::vector<Expectation> expectations;
    for (const auto& expr : opts.expectations) {
        Expectation exp;
        if (!parse_expectation(expr, exp)) {
            std::cerr << "Error: invalid expectation expression: \"" << expr << "\"\n"
                      << "  Expected format: <path> <op> <number>\n"
                      << "  Example: signals.DensityDetected >= 10\n";
            return 1;
        }
        expectations.push_back(exp);
    }

    // Run replay silently (suppress stdout, no JSONL file)
    CliOptions silent_opts = opts;
    silent_opts.no_stdout = true;
    silent_opts.no_jsonl = true;
    silent_opts.machine = false;

    // Initialize trace logger
    auto& logger = TraceLogger::instance();
    logger.init(silent_opts);

    // Build pipeline and run replay
    ProbePipeline pipeline(silent_opts);
    pipeline.summary().set_source("replay file=" + opts.dump_path);

    auto clock = std::make_shared<NullSleepClock>();
    ReplayFeed feed(opts.dump_path, clock, 0.0);

    for (auto* listener : pipeline.get_listeners()) {
        feed.add_listener(listener);
    }
    if (!opts.no_executor) {
        feed.add_listener(&pipeline.paper_executor());
    }

    auto stats = feed.run();

    for (size_t i = 0; i < stats.messages_dispatched; ++i) {
        pipeline.summary().record_message();
    }
    for (size_t i = 0; i < stats.parse_errors; ++i) {
        pipeline.summary().record_parse_error();
    }

    pipeline.finalize();
    logger.shutdown();

    // Evaluate expectations against summary
    auto summary = pipeline.summary().to_json();

    bool all_passed = true;
    nlohmann::json results = nlohmann::json::array();

    for (const auto& exp : expectations) {
        double actual = 0.0;
        bool resolved = resolve_path(summary, exp.path, actual);
        bool passed = resolved && evaluate(exp.op, actual, exp.value);

        nlohmann::json r;
        r["expression"] = exp.raw;
        r["path"] = exp.path;
        r["op"] = exp.op;
        r["expected"] = exp.value;
        r["actual"] = resolved ? actual : 0.0;
        r["resolved"] = resolved;
        r["passed"] = passed;
        results.push_back(r);

        if (!passed) all_passed = false;
    }

    // Output results
    if (opts.machine) {
        nlohmann::json output;
        output["stage"] = "assert_result";
        output["all_passed"] = all_passed;
        output["results"] = results;
        std::cout << output.dump() << "\n";
    } else {
        std::cout << "=== core_probe assert ===\n\n";
        for (const auto& r : results) {
            bool passed = r["passed"].get<bool>();
            bool resolved = r["resolved"].get<bool>();
            std::string status = passed ? "✓ PASS" : "✗ FAIL";
            std::cout << "  " << status << "  " << r["expression"].get<std::string>();
            if (resolved) {
                std::cout << "  (actual: " << r["actual"].get<double>() << ")";
            } else {
                std::cout << "  (path not found)";
            }
            std::cout << "\n";
        }
        std::cout << "\n" << (all_passed ? "All expectations passed." : "Some expectations FAILED.") << "\n";
    }

    return all_passed ? 0 : 3;
}

} // namespace trade_bot::probe
