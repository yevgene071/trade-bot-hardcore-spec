#include "DumpRecorder.hpp"
#include "logger/Logger.hpp"
#include <filesystem>

namespace trade_bot {

bool DumpRecorder::start(const std::string& path) {
    std::lock_guard lk(mtx_);
    if (active_.load()) stop();

    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path().empty()
            ? std::filesystem::path(".")
            : std::filesystem::path(path).parent_path()
    );

    out_.open(path, std::ios::out | std::ios::trunc);
    if (!out_) {
        LOG_ERROR("DumpRecorder: cannot open {}", path);
        return false;
    }
    path_ = path;
    active_.store(true);
    LOG_INFO("DumpRecorder: recording to {}", path);
    return true;
}

void DumpRecorder::stop() {
    std::lock_guard lk(mtx_);
    if (!active_.load()) return;
    active_.store(false);
    out_.close();
    LOG_INFO("DumpRecorder: stopped, file {}", path_);
}

std::string DumpRecorder::path() const {
    std::lock_guard lk(mtx_);
    return path_;
}

void DumpRecorder::record(const nlohmann::json& msg, int64_t recv_ts_ns) {
    if (!active_.load()) return;
    std::lock_guard lk(mtx_);
    if (!out_) return;
    nlohmann::json line{{"recv_ts_ns", recv_ts_ns}, {"message", msg}};
    out_ << line.dump() << "\n";
    out_.flush();
}

} // namespace trade_bot
