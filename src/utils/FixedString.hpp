#pragma once

#include <string_view>
#include <array>
#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>

#include <fmt/core.h>

namespace trade_bot {

/**
 * A fixed-capacity string that does not allocate on the heap.
 * Useful for hot paths where std::string allocations would cause jitter.
 */
template <std::size_t N>
class FixedString {
public:
    FixedString() {
        data_[0] = '\0';
    }

    FixedString(const char* str) {
        assign(str);
    }

    FixedString(std::string_view sv) {
        assign(sv);
    }

    void assign(std::string_view sv) {
        std::size_t len = std::min(sv.length(), N - 1);
        std::memcpy(data_.data(), sv.data(), len);
        data_[len] = '\0';
        size_ = len;
    }

    const char* c_str() const { return data_.data(); }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    template <typename... Args>
    static FixedString format(const char* fmt, Args... args) {
        FixedString fs;
        int len = std::snprintf(fs.data_.data(), N, fmt, args...);
        if (len >= 0) {
            fs.size_ = std::min(static_cast<std::size_t>(len), N - 1);
        }
        return fs;
    }

    operator std::string_view() const {
        return std::string_view(data_.data(), size_);
    }

    bool operator==(std::string_view other) const {
        return std::string_view(*this) == other;
    }

private:
    std::array<char, N> data_;
    std::size_t size_ = 0;
};

// JSON serialization support
template <std::size_t N>
void to_json(nlohmann::json& j, const FixedString<N>& s) {
    j = std::string_view(s);
}

template <std::size_t N>
void from_json(const nlohmann::json& j, FixedString<N>& s) {
    s.assign(j.get<std::string_view>());
}

} // namespace trade_bot

// fmt formatter support
template <std::size_t N>
struct fmt::formatter<trade_bot::FixedString<N>> {
    constexpr auto parse(fmt::format_parse_context& ctx) -> decltype(ctx.begin()) {
        // Accept any format spec (silently ignored — FixedString outputs its content only).
        auto it = ctx.begin();
        while (it != ctx.end() && *it != '}') ++it;
        return it;
    }

    auto format(const trade_bot::FixedString<N>& s, fmt::format_context& ctx) const
        -> fmt::format_context::iterator {
        return fmt::format_to(ctx.out(), "{}", static_cast<std::string_view>(s));
    }
};
