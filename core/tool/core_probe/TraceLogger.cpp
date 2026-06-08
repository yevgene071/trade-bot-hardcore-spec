#include "TraceLogger.hpp"
#include "TraceFormatter.hpp"
#include <iostream>
#include <filesystem>
#include <chrono>

namespace trade_bot::probe {

TraceLogger::~TraceLogger() {
    shutdown();
}

void TraceLogger::init(const CliOptions& opts) {
    opts_ = opts;

    // Forward include_payload flag to TraceFormatter
    TraceFormatter::set_include_payload(opts_.include_payload);
    
    // Configure stage filters
    for (const auto& s : opts_.stages) {
        stage_filter_.insert(s);
    }
    for (const auto& s : opts_.mute_stages) {
        mute_filter_.insert(s);
    }

    // Configure JSONL output file
    if (!opts_.no_jsonl) {
        std::string path = opts_.jsonl_out;
        if (path.empty()) {
            std::filesystem::create_directories("logs");
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            path = "logs/core_probe-" + std::to_string(now_ms) + ".jsonl";
        } else {
            auto parent = std::filesystem::path(path).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
        }
        file_sink_ = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::binary);
        if (!file_sink_->is_open()) {
            std::cerr << "Warning: failed to open JSONL output file: " << path << "\n";
            file_sink_.reset();
        }
    }

    // Start background writer thread
    running_ = true;
    writer_thread_ = std::thread(&TraceLogger::writer_loop, this);
}

void TraceLogger::shutdown() {
    if (!running_) return;
    
    running_ = false;
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    
    // Flush remaining files
    if (file_sink_) {
        file_sink_->flush();
        file_sink_.reset();
    }
}

void TraceLogger::enqueue(TraceEvent&& ev) noexcept {
    if (!running_) return;

    total_count_.fetch_add(1, std::memory_order_relaxed);
    
    // Prevent memory bloat: limit maximum elements in queue to e.g. 500,000
    // Try to enqueue.
    bool ok = queue_.enqueue(std::move(ev));
    if (!ok) {
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool TraceLogger::should_log(const TraceEvent& ev) {
    // 1. Filter by trace ID if specified
    if (opts_.trace_id_filter && ev.trace_id != *opts_.trace_id_filter) {
        return false;
    }

    // 2. Filter by stages
    if (!stage_filter_.empty() && !stage_filter_.contains(ev.stage)) {
        return false;
    }
    if (mute_filter_.contains(ev.stage)) {
        return false;
    }

    // 3. Profiles: quiet vs verbose
    if (opts_.quiet) {
        // quiet profile: only signal, strategy, risk, executor, account, invariant, error, perf, summary, meta
        static const std::unordered_set<std::string> quiet_allowed = {
            "meta", "signal", "strategy", "risk", "executor", "account", "invariant", "error", "perf", "summary"
        };
        if (!quiet_allowed.contains(ev.stage)) {
            return false;
        }
    } else if (!opts_.verbose) {
        // standard profile: mute raw events to avoid flooding, unless verbose
        if (ev.stage == "ws_raw" || ev.stage == "parse") {
            return false;
        }
    }

    // 4. Book throttling
    if (ev.stage == "book" && opts_.throttle_book_ms > 0) {
        uint64_t now_ns = ev.ts_ns;
        auto it = last_book_log_ns_.find(ev.ticker);
        if (it != last_book_log_ns_.end()) {
            uint64_t diff_ms = (now_ns - it->second) / 1'000'000;
            if (diff_ms < opts_.throttle_book_ms) {
                return false;
            }
        }
        last_book_log_ns_[ev.ticker] = now_ns;
    }

    return true;
}

void TraceLogger::writer_loop() {
    TraceEvent ev;
    while (running_ || queue_.size_approx() > 0) {
        bool popped = queue_.try_dequeue(ev);
        if (popped) {
            if (should_log(ev)) {
                // Formatting
                std::string machine_str;
                std::string human_str;
                
                if (opts_.machine) {
                    machine_str = TraceFormatter::to_machine_string(ev);
                } else {
                    human_str = TraceFormatter::to_human_string(ev, opts_.no_color);
                    if (file_sink_) {
                        machine_str = TraceFormatter::to_machine_string(ev);
                    }
                }

                // Write to stdout
                if (!opts_.no_stdout) {
                    if (opts_.machine) {
                        std::cout << machine_str << "\n";
                    } else {
                        std::cout << human_str << "\n";
                    }
                }

                // Write to JSONL
                if (file_sink_) {
                    if (machine_str.empty()) {
                        machine_str = TraceFormatter::to_machine_string(ev);
                    }
                    file_sink_->write(machine_str.c_str(), machine_str.size());
                    file_sink_->put('\n');
                }
            }
        } else {
            // Sleep slightly when queue is empty to yield CPU
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

void TraceLogger::log_meta(const std::string& schema_version, const std::string& git_sha, const std::string& config_path) {
    TraceEvent ev;
    ev.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ev.stage = "meta";
    ev.severity = "info";
    ev.message = "Initializing core_probe tracing system";
    ev.payload = {
        {"schema_version", schema_version},
        {"git_sha", git_sha},
        {"config", config_path}
    };
    enqueue(std::move(ev));
}

void TraceLogger::log_error(uint64_t trace_id, const std::string& ticker, const std::string& msg, const nlohmann::json& payload) {
    TraceEvent ev;
    ev.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ev.trace_id = trace_id;
    ev.ticker = ticker;
    ev.stage = "error";
    ev.severity = "error";
    ev.message = msg;
    ev.payload = payload;
    enqueue(std::move(ev));
}

void TraceLogger::log_invariant(uint64_t trace_id, const std::string& ticker, const std::string& severity, const std::string& code, const std::string& msg, const nlohmann::json& details, const std::string& hint) {
    TraceEvent ev;
    ev.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ev.trace_id = trace_id;
    ev.ticker = ticker;
    ev.stage = "invariant";
    ev.severity = severity;
    ev.message = "[" + code + "] " + msg + " — hint: " + hint;
    ev.payload = {
        {"severity", severity},
        {"code", code},
        {"details", details},
        {"hint", hint}
    };
    enqueue(std::move(ev));
}

} // namespace trade_bot::probe
