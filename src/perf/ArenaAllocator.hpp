#pragma once

#include <memory_resource>
#include <cstddef>

namespace trade_bot {

/**
 * Wrapper for std::pmr::monotonic_buffer_resource for arena allocation.
 * Ideal for per-ticker processing in hot paths.
 */
class ArenaAllocator {
public:
    explicit ArenaAllocator(size_t initial_size_bytes = 256 * 1024)
        : buffer_(initial_size_bytes)
        , resource_(buffer_.data(), buffer_.size()) {}

    std::pmr::memory_resource* resource() { return &resource_; }

    void reset() {
        resource_.release();
    }

private:
    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
};

} // namespace trade_bot
