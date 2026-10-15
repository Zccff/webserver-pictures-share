#include "timer/HeapTimer.h"

void HeapTimer::add_or_adjust(int fd, int timeout_ms, TimeoutCallback cb) {
    auto expire = Clock::now() + std::chrono::milliseconds(timeout_ms);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = indices_.find(fd);

    if (it == indices_.end()) {
        size_t index = heap_.size();
        heap_.push_back(TimerNode{ fd, expire, std::move(cb) });
        indices_[fd] = index;
        sift_up(index);
        return;
    }

    size_t index = it->second;
    Clock::time_point old_expire = heap_[index].expire;
    heap_[index].expire = expire;
    heap_[index].cb = std::move(cb);

    if (expire < old_expire) {
        sift_up(index);
    }
    else {
        sift_down(index);
    }
}

void HeapTimer::remove(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = indices_.find(fd);

    if (it != indices_.end()) {
        erase_at(it->second);
    }
}

void HeapTimer::tick() {
    while (true) {
        TimeoutCallback cb;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (heap_.empty()) {
                return;
            }

            const TimerNode& node = heap_.front();

            if (node.expire > Clock::now()) {
                return;
            }

            cb = std::move(heap_.front().cb);
            erase_at(0);
        }

        if (cb) {
            cb();
        }
    }
}

int HeapTimer::get_next_timeout_ms() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!heap_.empty()) {
        const TimerNode& node = heap_.front();

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

size_t HeapTimer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heap_.size();
}

void HeapTimer::swap_nodes(size_t lhs, size_t rhs) {
    if (lhs == rhs) {
        return;
    }

    std::swap(heap_[lhs], heap_[rhs]);
    indices_[heap_[lhs].fd] = lhs;
    indices_[heap_[rhs].fd] = rhs;
}

void HeapTimer::sift_up(size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;

        if (heap_[parent].expire <= heap_[index].expire) {
            return;
        }

        swap_nodes(parent, index);
        index = parent;
    }
}

void HeapTimer::sift_down(size_t index) {
    while (true) {
        size_t left = index * 2 + 1;

        if (left >= heap_.size()) {
            return;
        }

        size_t right = left + 1;
        size_t smallest = left;

        if (right < heap_.size() &&
            heap_[right].expire < heap_[left].expire) {
            smallest = right;
        }

        if (heap_[index].expire <= heap_[smallest].expire) {
            return;
        }

        swap_nodes(index, smallest);
        index = smallest;
    }
}

void HeapTimer::erase_at(size_t index) {
    size_t last = heap_.size() - 1;

    if (index != last) {
        swap_nodes(index, last);
    }

    indices_.erase(heap_.back().fd);
    heap_.pop_back();

    if (index >= heap_.size()) {
        return;
    }

    if (index > 0 &&
        heap_[index].expire < heap_[(index - 1) / 2].expire) {
        sift_up(index);
    }
    else {
        sift_down(index);
    }
}
