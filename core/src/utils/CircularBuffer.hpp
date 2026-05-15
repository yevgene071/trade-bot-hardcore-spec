#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>

namespace trade_bot {

/**
 * A simple fixed-size circular buffer.
 */
template <typename T, std::size_t N>
class CircularBuffer {
public:
    void push_back(const T& value) {
        data_[head_] = value;
        head_ = (head_ + 1) % N;
        if (size_ < N) {
            size_++;
        } else {
            tail_ = (tail_ + 1) % N;
        }
    }

    void pop_front() {
        if (size_ == 0) return;
        tail_ = (tail_ + 1) % N;
        size_--;
    }

    const T& front() const {
        return data_[tail_];
    }

    const T& back() const {
        return data_[(head_ + N - 1) % N];
    }

    const T& operator[](std::size_t i) const {
        return data_[(tail_ + i) % N];
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return N; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ == N; }

    void clear() {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

private:
    std::array<T, N> data_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
};

} // namespace trade_bot
