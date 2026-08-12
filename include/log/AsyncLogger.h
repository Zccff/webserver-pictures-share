#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class AsyncLogger {
public:
    static AsyncLogger& instance();

    void init(const std::string& file_path = "server.log");

    void log(const std::string& level, const std::string& message);

    bool should_log(const std::string& level) const;

    void shutdown();

private:
    AsyncLogger() = default;

    ~AsyncLogger();

    void worker_loop();

    std::string now_time() const;

    static int level_value(const std::string& level);

private:
    std::atomic<int> min_level_{ 0 };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    std::ofstream file_;
    std::thread worker_;
    bool running_{ false };
};

#define LOG_INFO(msg) do { if (AsyncLogger::instance().should_log("INFO")) AsyncLogger::instance().log("INFO", msg); } while (0)
#define LOG_WARN(msg) do { if (AsyncLogger::instance().should_log("WARN")) AsyncLogger::instance().log("WARN", msg); } while (0)
#define LOG_ERROR(msg) do { if (AsyncLogger::instance().should_log("ERROR")) AsyncLogger::instance().log("ERROR", msg); } while (0)
