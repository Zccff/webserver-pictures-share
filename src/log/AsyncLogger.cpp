#include "log/AsyncLogger.h"

#include "utils/Metrics.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

AsyncLogger& AsyncLogger::instance() {
    static AsyncLogger logger;
    return logger;
}

AsyncLogger::~AsyncLogger() {
    shutdown();
}

void AsyncLogger::init(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        return;
    }

    const char* configured_level = std::getenv("MYWEBSERVER_LOG_LEVEL");

    if (configured_level && configured_level[0] != '\0') {
        min_level_.store(level_value(configured_level), std::memory_order_relaxed);
    }

    const char* configured_queue_size = std::getenv("LOG_QUEUE_CAPACITY");

    if (configured_queue_size && configured_queue_size[0] != '\0') {
        size_t parsed = static_cast<size_t>(std::strtoull(
            configured_queue_size,
            nullptr,
            10
        ));

        if (parsed > 0) {
            max_queue_size_ = parsed;
        }
    }

    file_.open(file_path, std::ios::app);

    if (!file_.is_open()) {
        std::cerr << "open log file failed: " << file_path << std::endl;
    }

    running_ = true;
    worker_ = std::thread(&AsyncLogger::worker_loop, this);
}

void AsyncLogger::log(const std::string& level, const std::string& message) {
    if (!should_log(level)) {
        return;
    }

    std::ostringstream oss;

    oss << "[" << now_time() << "] "
        << "[" << level << "] "
        << message << "\n";

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            std::cerr << oss.str();
            return;
        }

        if (queue_.size() >= max_queue_size_) {
            dropped_logs_.fetch_add(1, std::memory_order_relaxed);
            Metrics::instance().inc_dropped_log();
            return;
        }

        queue_.push(oss.str());
    }

    cv_.notify_one();
}

bool AsyncLogger::should_log(const std::string& level) const {
    return level_value(level) >= min_level_.load(std::memory_order_relaxed);
}

void AsyncLogger::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!running_) {
            return;
        }

        running_ = false;
    }

    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void AsyncLogger::worker_loop() {
    while (true) {
        std::queue<std::string> local_queue;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            cv_.wait(lock, [this]() {
                return !running_ || !queue_.empty();
                });

            if (!running_ && queue_.empty()) {
                break;
            }

            std::swap(local_queue, queue_);
        }

        while (!local_queue.empty()) {
            const std::string& line = local_queue.front();

            if (file_.is_open()) {
                file_ << line;
            }
            else {
                std::cerr << line;
            }

            local_queue.pop();
        }

        uint64_t dropped = dropped_logs_.exchange(0, std::memory_order_relaxed);

        if (dropped > 0) {
            std::string line = "[WARN] async log queue was full; dropped " +
                std::to_string(dropped) + " log records\n";

            if (file_.is_open()) {
                file_ << line;
            }
            else {
                std::cerr << line;
            }
        }

        if (file_.is_open()) {
            file_.flush();
        }
    }
}

std::string AsyncLogger::now_time() const {
    auto now = std::chrono::system_clock::now();

    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};

    localtime_r(&t, &tm);

    std::ostringstream oss;

    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

    return oss.str();
}

int AsyncLogger::level_value(const std::string& level) {
    std::string normalized = level;

    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        }
    );

    if (normalized == "INFO") {
        return 0;
    }

    if (normalized == "WARN" || normalized == "WARNING") {
        return 1;
    }

    if (normalized == "ERROR") {
        return 2;
    }

    if (normalized == "OFF" || normalized == "NONE") {
        return 3;
    }

    return 0;
}
