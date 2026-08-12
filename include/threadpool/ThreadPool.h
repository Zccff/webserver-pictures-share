#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(size_t thread_count = std::thread::hardware_concurrency()) {
        if (thread_count == 0) {
            thread_count = 4;
        }

        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(mutex_);

                        cv_.wait(lock, [this]() {
                            return stopped_ || !tasks_.empty();
                            });

                        if (stopped_ && tasks_.empty()) {
                            return;
                        }

                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }

                    task();
                }
                });
        }
    }

    ~ThreadPool() {
        stop();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_) {
                throw std::runtime_error("submit on stopped ThreadPool");
            }

            tasks_.push(std::move(task));
        }

        cv_.notify_one();
    }

    size_t queue_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (stopped_) {
                return;
            }

            stopped_ = true;
        }

        cv_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopped_{ false };
};