#include "AccountStatePersister.hpp"

#include "executor/IExecutor.hpp"
#include "logger/Logger.hpp"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

namespace trade_bot {

AccountStatePersister::AccountStatePersister(std::string path) : path_(std::move(path)) {
    auto dir = std::filesystem::path(path_).parent_path();
    if (!dir.empty())
        std::filesystem::create_directories(dir);

    // Advisory exclusive lock on the .lock sibling. Two bot processes
    // pointing at the same journal would otherwise race their tmp+rename
    // and silently corrupt state. Issue #128.
    const std::string lock_path = path_ + ".lock";
    lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd_ < 0) {
        LOG_WARN("AccountStatePersister: cannot open lock file {} — running without lock",
                 lock_path);
    } else {
        // Retry up to 3s in 300ms steps — handles quick restart after SIGTERM
        bool acquired = false;
        for (int i = 0; i < 10; ++i) {
            if (::flock(lock_fd_, LOCK_EX | LOCK_NB) == 0) {
                acquired = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if (!acquired) {
            ::close(lock_fd_);
            lock_fd_ = -1;
            throw std::runtime_error("AccountStatePersister: another instance is using " + path_);
        }
    }
}

AccountStatePersister::~AccountStatePersister() {
    if (lock_fd_ >= 0) {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
    }
}

void AccountStatePersister::save(const PersistedData& data) {
    nlohmann::json j;
    j["schema_version"] = 1;
    j["last_persist_ts_utc"] =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    j["last_reset_day_utc"] = data.last_reset_day_utc;
    j["account_state"] = data.account_state;
    j["active_trades"] = data.active_trades;
    j["kill_switch_triggered"] = data.kill_switch_triggered;
    j["kill_switch_reason"] = data.kill_switch_reason;

    std::lock_guard lock(save_mtx_);

    std::string tmp_path = path_ + ".tmp";
    std::string content = j.dump(2);

    // Write tmp + fdatasync so the content survives a power loss before rename.
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_ERROR("AccountStatePersister: open tmp failed: {}", tmp_path);
        return;
    }
    if (::write(fd, content.data(), content.size()) < 0) {
        LOG_ERROR("AccountStatePersister: write tmp failed: {}", tmp_path);
        ::close(fd);
        return;
    }
    ::fdatasync(fd);
    ::close(fd);

    std::filesystem::rename(tmp_path, path_);

    // fsync the directory so the rename itself is durable.
    auto dir = std::filesystem::path(path_).parent_path().string();
    if (dir.empty())
        dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

std::optional<AccountStatePersister::PersistedData> AccountStatePersister::load() {
    if (!std::filesystem::exists(path_))
        return std::nullopt;

    try {
        std::ifstream in(path_);
        nlohmann::json j;
        in >> j;

        if (int ver = j.value("schema_version", 0); ver != 1) {
            LOG_ERROR("AccountStatePersister: unsupported schema_version={}, refusing to load {}",
                      ver, path_);
            return std::nullopt;
        }

        PersistedData data;
        data.last_reset_day_utc = j.value("last_reset_day_utc", "");
        data.account_state = j["account_state"].get<AccountState>();
        data.active_trades = j["active_trades"].get<std::vector<ActiveTrade>>();
        data.kill_switch_triggered = j.value("kill_switch_triggered", false);
        data.kill_switch_reason = j.value("kill_switch_reason", "");
        return data;
    } catch (const std::exception& e) {
        LOG_ERROR("AccountStatePersister: failed to load state from {}: {}", path_, e.what());
        return std::nullopt;
    }
}

// JSON helpers
void to_json(nlohmann::json& j, const SignalPayload& p) {
    j = nlohmann::json{{"side", p.side},
                       {"size", p.size},
                       {"size_usd", p.size_usd},
                       {"total_eaten_usd", p.total_eaten_usd},
                       {"original_size", p.original_size},
                       {"remaining_size", p.remaining_size},
                       {"eaten_ratio", p.eaten_ratio},
                       {"remaining_ratio", p.remaining_ratio},
                       {"first_price", p.first_price},
                       {"last_price", p.last_price},
                       {"width_bps", p.width_bps},
                       {"total_size_usd", p.total_size_usd},
                       {"stop_anchor_price", p.stop_anchor_price},
                       {"lag_pct", p.lag_pct},
                       {"correlation", p.correlation},
                       {"dist_bps", p.dist_bps},
                       {"delta_bps", p.delta_bps},
                       {"leader_move_pct", p.leader_move_pct},
                       {"our_move_pct", p.our_move_pct},
                       {"expected_move_pct", p.expected_move_pct},
                       {"lag_ms", p.lag_ms},
                       {"ratio", p.ratio},
                       {"intensity", p.intensity},
                       {"peak_rate", p.peak_rate},
                       {"current_rate", p.current_rate},
                       {"cusum", p.cusum},
                       {"volatility_bps", p.volatility_bps},
                       {"volume_usd_30s", p.volume_usd_30s},
                       {"max_range_bps", p.max_range_bps},
                       {"touches", p.touches},
                       {"prints", p.prints},
                       {"age_ms", p.age_ms},
                       {"refill_events", p.refill_events},
                       {"fake", p.fake},
                       {"id", p.id},
                       {"source", p.source}};
}

void from_json(const nlohmann::json& j, SignalPayload& p) {
    p.side.assign(j.value("side", ""));
    p.size = j.value("size", 0.0);
    p.size_usd = j.value("size_usd", 0.0);
    p.total_eaten_usd = j.value("total_eaten_usd", 0.0);
    p.original_size = j.value("original_size", 0.0);
    p.remaining_size = j.value("remaining_size", 0.0);
    p.eaten_ratio = j.value("eaten_ratio", 0.0);
    p.remaining_ratio = j.value("remaining_ratio", 0.0);
    p.first_price = j.value("first_price", 0.0);
    p.last_price = j.value("last_price", 0.0);
    p.width_bps = j.value("width_bps", 0.0);
    p.total_size_usd = j.value("total_size_usd", 0.0);
    p.stop_anchor_price = j.value("stop_anchor_price", 0.0);
    p.lag_pct = j.value("lag_pct", 0.0);
    p.correlation = j.value("correlation", 0.0);
    p.dist_bps = j.value("dist_bps", 0.0);
    p.delta_bps = j.value("delta_bps", 0.0);
    p.leader_move_pct = j.value("leader_move_pct", 0.0);
    p.our_move_pct = j.value("our_move_pct", 0.0);
    p.expected_move_pct = j.value("expected_move_pct", 0.0);
    p.lag_ms = j.value("lag_ms", 0.0);
    p.ratio = j.value("ratio", 0.0);
    p.intensity = j.value("intensity", 0.0);
    p.peak_rate = j.value("peak_rate", 0.0);
    p.current_rate = j.value("current_rate", 0.0);
    p.cusum = j.value("cusum", 0.0);
    p.volatility_bps = j.value("volatility_bps", 0.0);
    p.volume_usd_30s = j.value("volume_usd_30s", 0.0);
    p.max_range_bps = j.value("max_range_bps", 0.0);
    p.touches = j.value("touches", 0);
    p.prints = j.value("prints", 0);
    p.age_ms = j.value("age_ms", 0);
    p.refill_events = j.value("refill_events", 0);
    p.fake = j.value("fake", false);
    p.id.assign(j.value("id", ""));
    p.source.assign(j.value("source", ""));
}

void to_json(nlohmann::json& j, const AccountState& s) {
    j = nlohmann::json{{"equity_usd", s.equity_usd},
                       {"starting_equity_usd", s.starting_equity_usd},
                       {"realized_pnl_today_usd", s.realized_pnl_today_usd},
                       {"finres_day_start_result_usd", s.finres_day_start_result_usd},
                       {"unrealized_pnl_usd", s.unrealized_pnl_usd},
                       {"free_balance_usd", s.free_balance_usd},
                       {"active_positions", s.active_positions},
                       {"active_tickers", s.active_tickers},
                       {"kill_switch_triggered", s.kill_switch_triggered}};
}

void from_json(const nlohmann::json& j, AccountState& s) {
    s.equity_usd = j.at("equity_usd").get<double>();
    s.starting_equity_usd = j.at("starting_equity_usd").get<double>();
    s.realized_pnl_today_usd = j.at("realized_pnl_today_usd").get<double>();
    s.finres_day_start_result_usd = j.at("finres_day_start_result_usd").get<double>();
    s.unrealized_pnl_usd = j.at("unrealized_pnl_usd").get<double>();
    s.free_balance_usd = j.at("free_balance_usd").get<double>();
    s.active_positions = j.at("active_positions").get<int>();
    s.active_tickers = j.at("active_tickers").get<std::vector<Ticker>>();
    s.kill_switch_triggered = j.at("kill_switch_triggered").get<bool>();
}

void to_json(nlohmann::json& j, const Signal& s) {
    j = nlohmann::json{{"kind", static_cast<int>(s.kind)},
                       {"ticker", s.ticker},
                       {"price", s.price},
                       {"confidence", s.confidence},
                       {"payload", s.payload}};
}

void from_json(const nlohmann::json& j, Signal& s) {
    s.kind = static_cast<SignalKind>(j.at("kind").get<int>());
    s.ticker = j.at("ticker").get<Ticker>();
    s.price = j.at("price").get<double>();
    s.confidence = j.at("confidence").get<double>();
    s.payload = j.at("payload").get<SignalPayload>();
}

void to_json(nlohmann::json& j, const TradePlan& p) {
    j = nlohmann::json{{"ticker", p.ticker},
                       {"side", static_cast<int>(p.side)},
                       {"entry_type", static_cast<int>(p.entry_type)},
                       {"entry_price", p.entry_price},
                       {"stop_price", p.stop_price},
                       {"tp1_price", p.tp1_price},
                       {"tp2_price", p.tp2_price ? nlohmann::json(*p.tp2_price) : nlohmann::json{}},
                       {"tp1_size_ratio", p.tp1_size_ratio},
                       {"size_coin", p.size_coin},
                       {"risk_usd", p.risk_usd},
                       {"strategy_name", p.strategy_name},
                       {"reason", p.reason},
                       {"evidence", p.evidence},
                       {"no_progress_timeout_sec", p.no_progress_timeout_sec},
                       {"post_entry_grace_sec", p.post_entry_grace_sec},
                       {"min_follow_through_bps", p.min_follow_through_bps},
                       {"entry_correlation", p.entry_correlation},
                       {"leader_entry_lag_pct", p.leader_entry_lag_pct},
                       {"correlation_exit_threshold", p.correlation_exit_threshold},
                       {"leader_exit_reversal_bps", p.leader_exit_reversal_bps},
                       {"density_price_for_stop", p.density_price_for_stop},
                       {"approach_count", p.approach_count},
                       {"trace_id", p.trace_id}};
}

void from_json(const nlohmann::json& j, TradePlan& p) {
    p.ticker = j.at("ticker").get<Ticker>();
    p.side = static_cast<Side>(j.at("side").get<int>());
    p.entry_type = static_cast<OrderType>(j.at("entry_type").get<int>());
    p.entry_price = j.at("entry_price").get<double>();
    p.stop_price = j.at("stop_price").get<double>();
    p.tp1_price = j.at("tp1_price").get<double>();
    if (j.contains("tp2_price") && !j.at("tp2_price").is_null()) {
        p.tp2_price = j.at("tp2_price").get<double>();
    } else {
        p.tp2_price = std::nullopt;
    }
    p.tp1_size_ratio = j.at("tp1_size_ratio").get<double>();
    p.size_coin = j.at("size_coin").get<double>();
    p.risk_usd = j.at("risk_usd").get<double>();
    p.strategy_name = j.at("strategy_name").get<FixedString<32>>();
    p.reason = j.at("reason").get<FixedString<128>>();
    p.evidence = j.at("evidence").get<std::vector<Signal>>();
    p.no_progress_timeout_sec = j.value("no_progress_timeout_sec", p.no_progress_timeout_sec);
    p.post_entry_grace_sec = j.value("post_entry_grace_sec", p.post_entry_grace_sec);
    p.min_follow_through_bps = j.value("min_follow_through_bps", p.min_follow_through_bps);
    p.entry_correlation = j.value("entry_correlation", p.entry_correlation);
    p.leader_entry_lag_pct = j.value("leader_entry_lag_pct", p.leader_entry_lag_pct);
    p.correlation_exit_threshold =
        j.value("correlation_exit_threshold", p.correlation_exit_threshold);
    p.leader_exit_reversal_bps = j.value("leader_exit_reversal_bps", p.leader_exit_reversal_bps);
    p.density_price_for_stop = j.value("density_price_for_stop", p.density_price_for_stop);
    p.approach_count = j.value("approach_count", p.approach_count);
    p.trace_id = j.value("trace_id", p.trace_id);
}

void to_json(nlohmann::json& j, const IExecutor::ClosedTrade& t) {
    j = nlohmann::json{{"plan", t.plan},
                       {"entry_price", t.entry_price},
                       {"exit_price", t.exit_price},
                       {"size_filled", t.size_filled},
                       {"pnl_usd", t.pnl_usd},
                       {"reason", t.reason}};
}

void from_json(const nlohmann::json& j, IExecutor::ClosedTrade& t) {
    t.plan = j.at("plan").get<TradePlan>();
    t.entry_price = j.at("entry_price").get<double>();
    t.exit_price = j.at("exit_price").get<double>();
    t.size_filled = j.at("size_filled").get<double>();
    t.pnl_usd = j.at("pnl_usd").get<double>();
    t.reason = j.at("reason").get<FixedString<32>>();
}

void to_json(nlohmann::json& j, const ActiveTrade& t) {
    j = nlohmann::json{{"plan", t.plan},
                       {"entry_order_id", t.entry_order_id},
                       {"stop_order_id", t.stop_order_id},
                       {"tp1_order_id", t.tp1_order_id},
                       {"tp2_order_id", t.tp2_order_id},
                       {"state", static_cast<int>(t.state)},
                       {"executed_size", t.executed_size},
                       {"avg_entry_price", t.avg_entry_price},
                       {"avg_price_fix", t.avg_price_fix},
                       {"tp1_filled", t.tp1_filled}};
}

void from_json(const nlohmann::json& j, ActiveTrade& t) {
    t.plan = j.at("plan").get<TradePlan>();
    t.entry_order_id = j.value("entry_order_id", int64_t{0});
    t.stop_order_id = j.value("stop_order_id", int64_t{0});
    t.tp1_order_id = j.value("tp1_order_id", int64_t{0});
    t.tp2_order_id = j.value("tp2_order_id", int64_t{0});
    t.state = static_cast<TradeState>(j.at("state").get<int>());
    t.executed_size = j.at("executed_size").get<double>();
    t.avg_entry_price = j.at("avg_entry_price").get<double>();
    t.avg_price_fix = j.value("avg_price_fix", 0.0);
    t.tp1_filled = j.value("tp1_filled", false);
}

} // namespace trade_bot
