#include "DumpRecorder.hpp"
#include "logger/Logger.hpp"
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <ext/stdio_filebuf.h>

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
    out_.flush();
    // Ensure pending data reaches disk before closing
    if (auto* buf = dynamic_cast<__gnu_cxx::stdio_filebuf<char>*>(out_.rdbuf())) {
        ::fdatasync(fileno(buf->file()));
    }
    out_.close();
    write_count_ = 0;
    LOG_INFO("DumpRecorder: stopped, file {}", path_);
}

std::string DumpRecorder::path() const {
    std::lock_guard lk(mtx_);
    return path_;
}

void DumpRecorder::record(const nlohmann::json& msg, int64_t recv_ts_ns) {
    std::lock_guard lk(mtx_);
    if (!active_.load()) return;
    if (!out_.is_open()) return;
    
    nlohmann::json line{{"recv_ts_ns", recv_ts_ns}, {"message", msg}};
    out_ << line.dump() << "\n";
    // Periodic fsync to limit data loss window on crash
    if (++write_count_ % 1000 == 0) {
        out_.flush();
        if (auto* buf = dynamic_cast<__gnu_cxx::stdio_filebuf<char>*>(out_.rdbuf())) {
            ::fdatasync(fileno(buf->file()));
        }
    }
}

} // namespace trade_bot
