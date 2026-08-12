#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

class HeapTimer {
public:
    using TimeoutCallback = std::function<void()>;

    void add_or_adjust(int fd, int timeout_ms, TimeoutCallback cb);

    void remove(int fd);

    void tick();

    int get_next_timeout_ms();

private:
    using Clock = std::chrono::steady_clock;

    struct TimerNode {
        int fd;
        Clock::time_point expire;
        TimeoutCallback cb;

        bool operator>(const TimerNode& other) const {
            return expire > other.expire;
        }
    };

private:
    std::mutex mutex_;

    std::priority_queue<
        TimerNode,
        std::vector<TimerNode>,
        std::greater<TimerNode>
    > heap_;

    std::unordered_map<int, Clock::time_point> expires_;
};