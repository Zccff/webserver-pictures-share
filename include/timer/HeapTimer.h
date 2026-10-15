#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

class HeapTimer {
public:
    using TimeoutCallback = std::function<void()>;

    void add_or_adjust(int fd, int timeout_ms, TimeoutCallback cb);

    void remove(int fd);

    void tick();

    int get_next_timeout_ms();

    size_t size() const;

private:
    using Clock = std::chrono::steady_clock;

    struct TimerNode {
        int fd;
        Clock::time_point expire;
        TimeoutCallback cb;
    };

    void swap_nodes(size_t lhs, size_t rhs);

    void sift_up(size_t index);

    void sift_down(size_t index);

    void erase_at(size_t index);

private:
    mutable std::mutex mutex_;

    std::vector<TimerNode> heap_;
    std::unordered_map<int, size_t> indices_;
};
