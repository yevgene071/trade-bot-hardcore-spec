#pragma once

#include <boost/lockfree/spsc_queue.hpp>
#include <cstddef>

namespace trade_bot {

/**
 * Single-Producer Single-Consumer lock-free queue.
 * Fixed capacity, zero allocation after construction.
 */
template <typename T, size_t Capacity>
using SpscQueue = boost::lockfree::spsc_queue<T, boost::lockfree::capacity<Capacity>>;

} // namespace trade_bot
