#include "AccountStatePersister.hpp"
#include "logger/Logger.hpp"

#include <fstream>
#include <filesystem>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace trade_bot {

AccountStatePersister::AccountStatePersister(std::string path)
    : path_(std::move(path)) {
    auto dir = std::filesystem::path(path_).parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir);

    // Advisory exclusive lock on the .lock sibling. Two bot processes
    // pointing at the same journal would otherwise race their tmp+rename
    // and silently corrupt state. Issue #128.
    const std::string lock_path = path_ + ".lock";
    lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd_ < 0) {
        LOG_WARN("AccountStatePersister: cannot open lock file {} — running without lock",
                 lock_path);
    } else if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock_fd_);
        lock_fd_ = -1;
        throw std::runtime_error(
            "AccountStatePersister: another instance is using " + path_);
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
    j["last_persist_ts_utc"] = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    j["last_reset_day_utc"] = data.last_reset_day_utc;
    j["account_state"] = data.account_state;
    j["active_trades"] = data.active_trades;
    j["kill_switch_triggered"] = data.kill_switch_triggered;
    j["kill_switch_reason"] = data.kill_switch_reason;

    std::string tmp_path = path_ + ".tmp";
    {
        std::ofstream out(tmp_path);
        out << j.dump(2);
    }
    std::filesystem::rename(tmp_path, path_);
}

std::optional<AccountStatePersister::PersistedData> AccountStatePersister::load() {
    if (!std::filesystem::exists(path_)) return std::nullopt;

    try {
        std::ifstream in(path_);
        nlohmann::json j;
        in >> j;

        PersistedData data;
        data.last_reset_day_utc = j.value("last_reset_day_utc", "");
        data.account_state = j["account_state"].get<AccountState>();
        data.active_trades = j["active_trades"].get<std::vector<ActiveTrade>>();
        data.kill_switch_triggered = j.value("kill_switch_triggered", false);
        data.kill_switch_reason = j.value("kill_switch_reason", "");
        return data;
    } catch (const std::exception& e) {
        LOG_ERROR("AccountStatePersister: failed to load state from {}: {}",
                  path_, e.what());
        return std::nullopt;
    }
}

// JSON helpers
void to_json(nlohmann::json& j, const AccountState& s) {
    j = nlohmann::json{
        {"equity_usd", s.equity_usd},
        {"starting_equity_usd", s.starting_equity_usd},
        {"realized_pnl_today_usd", s.realized_pnl_today_usd},
        {"finres_day_start_result_usd", s.finres_day_start_result_usd},
        {"unrealized_pnl_usd", s.unrealized_pnl_usd},
        {"free_balance_usd", s.free_balance_usd},
        {"active_positions", s.active_positions},
        {"active_tickers", s.active_tickers},
        {"kill_switch_triggered", s.kill_switch_triggered}
    };
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
    j = nlohmann::json{
        {"kind", static_cast<int>(s.kind)},
        {"ticker", s.ticker},
        {"price", s.price},
        {"confidence", s.confidence},
        {"payload", s.payload}
    };
}

void from_json(const nlohmann::json& j, Signal& s) {
    s.kind = static_cast<SignalKind>(j.at("kind").get<int>());
    s.ticker = j.at("ticker").get<Ticker>();
    s.price = j.at("price").get<double>();
    s.confidence = j.at("confidence").get<double>();
    s.payload = j.at("payload");
}

void to_json(nlohmann::json& j, const TradePlan& p) {
    j = nlohmann::json{
        {"ticker", p.ticker},
        {"side", static_cast<int>(p.side)},
        {"entry_type", static_cast<int>(p.entry_type)},
        {"entry_price", p.entry_price},
        {"stop_price", p.stop_price},
        {"tp1_price", p.tp1_price},
        {"tp1_size_ratio", p.tp1_size_ratio},
        {"size_coin", p.size_coin},
        {"risk_usd", p.risk_usd},
        {"strategy_name", p.strategy_name},
        {"reason", p.reason},
        {"evidence", p.evidence}
    };
}

void from_json(const nlohmann::json& j, TradePlan& p) {
    p.ticker = j.at("ticker").get<Ticker>();
    p.side = static_cast<Side>(j.at("side").get<int>());
    p.entry_type = static_cast<OrderType>(j.at("entry_type").get<int>());
    p.entry_price = j.at("entry_price").get<double>();
    p.stop_price = j.at("stop_price").get<double>();
    p.tp1_price = j.at("tp1_price").get<double>();
    p.tp1_size_ratio = j.at("tp1_size_ratio").get<double>();
    p.size_coin = j.at("size_coin").get<double>();
    p.risk_usd = j.at("risk_usd").get<double>();
    p.strategy_name = j.at("strategy_name").get<std::string>();
    p.reason = j.at("reason").get<std::string>();
    p.evidence = j.at("evidence").get<std::vector<Signal>>();
}

void to_json(nlohmann::json& j, const ActiveTrade& t) {
    j = nlohmann::json{
        {"plan",            t.plan},
        {"entry_order_id",  t.entry_order_id},
        {"stop_order_id",   t.stop_order_id},
        {"tp1_order_id",    t.tp1_order_id},
        {"tp2_order_id",    t.tp2_order_id},
        {"state",           static_cast<int>(t.state)},
        {"executed_size",   t.executed_size},
        {"avg_entry_price", t.avg_entry_price},
        {"avg_price_fix",   t.avg_price_fix},
        {"tp1_filled",      t.tp1_filled}
    };
}

void from_json(const nlohmann::json& j, ActiveTrade& t) {
    t.plan            = j.at("plan").get<TradePlan>();
    t.entry_order_id  = j.value("entry_order_id", int64_t{0});
    t.stop_order_id   = j.value("stop_order_id",  int64_t{0});
    t.tp1_order_id    = j.value("tp1_order_id",   int64_t{0});
    t.tp2_order_id    = j.value("tp2_order_id",   int64_t{0});
    t.state           = static_cast<TradeState>(j.at("state").get<int>());
    t.executed_size   = j.at("executed_size").get<double>();
    t.avg_entry_price = j.at("avg_entry_price").get<double>();
    t.avg_price_fix   = j.value("avg_price_fix", 0.0);
    t.tp1_filled      = j.value("tp1_filled",    false);
}

} // namespace trade_bot
