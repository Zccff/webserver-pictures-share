#include "timer/HeapTimer.h"

void HeapTimer::add_or_adjust(int fd, int timeout_ms, TimeoutCallback cb) {
    auto expire = Clock::now() + std::chrono::milliseconds(timeout_ms);

    std::lock_guard<std::mutex> lock(mutex_);

    expires_[fd] = expire;

    heap_.push(TimerNode{
        fd,
        expire,
        std::move(cb)
        });
}

void HeapTimer::remove(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);

    expires_.erase(fd);
}

void HeapTimer::tick() {
    while (true) {
        TimeoutCallback cb;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (heap_.empty()) {
                return;
            }

            const TimerNode& node = heap_.top();

            auto it = expires_.find(node.fd);

            if (it == expires_.end() || it->second != node.expire) {
                heap_.pop();
                continue;
            }

            if (node.expire > Clock::now()) {
                return;
            }

            cb = node.cb;

            expires_.erase(node.fd);

            heap_.pop();
        }

        if (cb) {
            cb();
        }
    }
}

int HeapTimer::get_next_timeout_ms() {
    std::lock_guard<std::mutex> lock(mutex_);

    while (!heap_.empty()) {
        const TimerNode& node = heap_.top();

        auto it = expires_.find(node.fd);

        if (it == expires_.end() || it->second != node.expire) {
            heap_.pop();
            continue;
        }

        auto now = Clock::now();

        if (node.expire <= now) {
            return 0;
        }

        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                node.expire - now
            ).count()
            );
    }

    return -1;
}