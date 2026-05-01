#pragma once

#include <moodycamel/concurrentqueue.h>

namespace trade_bot {

/**
 * Multi-Producer Multi-Consumer lock-free queue.
 * Wrapper for moodycamel::ConcurrentQueue.
 */
template <typename T>
class MpmcQueue {
public:
    void push(const T& item) {
        queue_.enqueue(item);
    }

    bool try_pop(T& item) {
        return queue_.try_dequeue(item);
    }

private:
    moodycamel::ConcurrentQueue<T> queue_;
};

} // namespace trade_bot
