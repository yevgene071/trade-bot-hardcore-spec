#pragma once

#include <cstddef>

namespace trade_bot {

/**
 * Wrapper to prevent false sharing by padding the value to a cache line (usually 64 bytes).
 */
template <typename T>
struct alignas(64) CachelinePadded {
    T value;
};

} // namespace trade_bot
