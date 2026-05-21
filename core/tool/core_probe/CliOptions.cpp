#include "CliOptions.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace trade_bot::probe {

namespace {
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> elems;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty()) {
            elems.push_back(item);
        }
    }
    return elems;
}
} // namespace

CliOptions CliOptions::parse(int argc, char* argv[]) {
    CliOptions opts;

    if (argc < 2) {
        return opts;
    }

    std::string first_arg = argv[1];
    if (first_arg == "-h" || first_arg == "--help") {
        opts.print_help();
        std::exit(0);
    } else if (first_arg == "--version") {
        opts.show_version = true;
        return opts;
    } else if (first_arg == "--list-stages") {
        opts.list_stages = true;
        return opts;
    }

    // Parse subcommand
    if (first_arg == "replay") {
        opts.cmd = Subcommand::Replay;
    } else if (first_arg == "live") {
        opts.cmd = Subcommand::Live;
    } else if (first_arg == "synth") {
        opts.cmd = Subcommand::Synth;
    } else if (first_arg == "diff") {
        opts.cmd = Subcommand::Diff;
    } else if (first_arg == "assert") {
        opts.cmd = Subcommand::Assert;
    } else if (first_arg == "schema") {
        opts.cmd = Subcommand::Schema;
    } else {
        std::cerr << "Unknown subcommand or option: " << first_arg << "\n";
        opts.print_help();
        std::exit(1);
    }

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if ((arg == "--dump" || arg == "-d") && i + 1 < argc) {
            opts.dump_path = argv[++i];
        } else if ((arg == "--ticker" || arg == "-t") && i + 1 < argc) {
            std::string t_arg = argv[++i];
            for (auto& t : split(t_arg, ',')) {
                opts.tickers.push_back(t);
            }
        } else if (arg == "--ws-url" && i + 1 < argc) {
            opts.ws_url = argv[++i];
        } else if (arg == "--scenario" && i + 1 < argc) {
            opts.scenario = argv[++i];
        } else if (arg == "--left" && i + 1 < argc) {
            opts.left_path = argv[++i];
        } else if (arg == "--right" && i + 1 < argc) {
            opts.right_path = argv[++i];
        } else if (arg == "--expect" && i + 1 < argc) {
            opts.expectations.push_back(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            opts.config_path = argv[++i];
        } else if (arg == "--detectors" && i + 1 < argc) {
            opts.detectors = split(argv[++i], ',');
        } else if (arg == "--strategies" && i + 1 < argc) {
            opts.strategies = split(argv[++i], ',');
        } else if (arg == "--no-strategy") {
            opts.no_strategy = true;
        } else if (arg == "--no-risk") {
            opts.no_risk = true;
        } else if (arg == "--no-executor") {
            opts.no_executor = true;
        } else if (arg == "--risk-observe") {
            opts.risk_observe = true;
        } else if (arg == "--stages" && i + 1 < argc) {
            opts.stages = split(argv[++i], ',');
        } else if (arg == "--mute" && i + 1 < argc) {
            opts.mute_stages = split(argv[++i], ',');
        } else if (arg == "--trace" && i + 1 < argc) {
            opts.trace_id_filter = std::stoull(argv[++i]);
        } else if (arg == "--throttle-book" && i + 1 < argc) {
            opts.throttle_book_ms = std::stoul(argv[++i]);
        } else if (arg == "--quiet" || arg == "-q") {
            opts.quiet = true;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg == "--machine") {
            opts.machine = true;
            opts.no_color = true;
        } else if (arg == "--no-color") {
            opts.no_color = true;
        } else if (arg == "--jsonl-out" && i + 1 < argc) {
            opts.jsonl_out = argv[++i];
        } else if (arg == "--no-jsonl") {
            opts.no_jsonl = true;
        } else if (arg == "--no-stdout") {
            opts.no_stdout = true;
        } else if (arg == "--include-payload") {
            opts.include_payload = true;
        } else if (arg == "--ai-recipe" && i + 1 < argc) {
            opts.ai_recipe = argv[++i];
        } else if (arg == "--ai-recipe-arg" && i + 1 < argc) {
            std::string arg_val = argv[++i];
            auto parts = split(arg_val, '=');
            if (parts.size() == 2) {
                opts.ai_recipe_args[parts[0]] = parts[1];
            } else if (parts.size() == 1) {
                opts.ai_recipe_args[parts[0]] = "";
            }
        } else if (arg == "--invariants" && i + 1 < argc) {
            opts.invariants_mode = argv[++i];
        } else if (arg == "--invariant-set" && i + 1 < argc) {
            opts.invariant_set = split(argv[++i], ',');
        } else if (arg == "--speed" && i + 1 < argc) {
            opts.speed_multiplier = std::stod(argv[++i]);
        } else if (arg == "--limit" && i + 1 < argc) {
            opts.limit_messages = std::stoull(argv[++i]);
        } else if (arg == "--start-ts" && i + 1 < argc) {
            opts.start_ts_iso = argv[++i];
        } else if (arg == "--equity-usd" && i + 1 < argc) {
            opts.equity_usd = std::stod(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            opts.print_help();
            std::exit(0);
        } else {
            std::cerr << "Warning: unknown option " << arg << " ignored.\n";
        }
    }

    return opts;
}

void CliOptions::print_help() const {
    std::cout << "core_probe — AI-friendly diagnostic CLI for core/ pipeline tracing & error tracking\n\n"
              << "Usage:\n"
              << "  core_probe replay --dump <ndjson> --ticker T[,T...] [common flags]\n"
              << "  core_probe live   --ws-url <url>  --ticker T[,T...] [common flags]\n"
              << "  core_probe synth  --scenario <name>                 [common flags]\n"
              << "  core_probe diff   --left <jsonl1> --right <jsonl2>  [--keys ...]\n"
              << "  core_probe assert --dump <ndjson> --ticker T --expect EXPR[,EXPR...]\n"
              << "  core_probe schema                                    # Prints JSON schema of all stages\n\n"
              << "Common Flags:\n"
              << "  --config <path>           Path to config.toml (default: config.toml)\n"
              << "  --detectors d1,d2...     Only run specified detectors (e.g. density,iceberg,tape)\n"
              << "  --strategies s1,s2...    Only run specified strategies (e.g. bounce,breakout)\n"
              << "  --no-strategy             Disable strategies (stop at SignalBus)\n"
              << "  --no-risk                 Disable risk check (plans submitted directly to executor)\n"
              << "  --no-executor             Disable executor (stop at TradePlan)\n"
              << "  --risk-observe            Risk manager observes only; doesn't block plans\n"
              << "  --stages s1,s2...        Only log these stages\n"
              << "  --mute s1,s2...          Mute these stages\n"
              << "  --trace <id>             Show only events with specific trace_id\n"
              << "  --throttle-book <Nms>    Throttle OrderBook logs to at most once per N ms per ticker\n"
              << "  --quiet, -q               Mute raw data (equivalent to default quiet profile)\n"
              << "  --verbose, -v             Log absolutely everything including raw websocket frames\n"
              << "  --machine                 AI-friendly: JSONL in stdout, no colors, no tables\n"
              << "  --no-color                Disable ANSI colors\n"
              << "  --jsonl-out <path>        Path to write JSONL file\n"
              << "  --no-jsonl                Do not write JSONL file to disk\n"
              << "  --no-stdout               Do not print trace events to stdout\n"
              << "  --include-payload         Include full payloads in JSONL output\n"
              << "  --ai-recipe <name>        Run an AI recipe after the run (e.g. why-no-trade)\n"
              << "  --ai-recipe-arg k=v       Arguments for the AI recipe\n"
              << "  --invariants on|off|strict Invariant check mode (default: on)\n"
              << "  --invariant-set s1,s2...  Only check specific invariant groups\n"
              << "  --speed <float>           Replay speed multiplier (0.0 = as-fast-as-possible, 1.0 = real-time)\n"
              << "  --limit <N>               Stop after N messages\n"
              << "  --start-ts <iso8601>      Skip replay until specified timestamp\n"
              << "  --equity-usd <float>      Initial equity for virtual paper account (default: 100000)\n"
              << "  --list-stages             List all available stages and exit\n"
              << "  --version                 Print version and git SHA\n";
}

} // namespace trade_bot::probe
