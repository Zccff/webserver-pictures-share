#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class RedisResultType {
    Status,
    String,
    Integer,
    Nil,
    Error
};

struct AsyncRedisResult {
    RedisResultType type{ RedisResultType::Error };
    std::string value;
    int64_t integer{ 0 };
    std::string error;

    bool ok() const {
        return type != RedisResultType::Error;
    }
};

class AsyncRedisClient {
public:
    using Callback = std::function<void(AsyncRedisResult result)>;

    static AsyncRedisClient& instance();

    ~AsyncRedisClient();

    bool init_from_env();

    void shutdown();

    bool available() const;

    std::string status_message() const;

    std::string key_prefix() const;

    bool command(std::vector<std::string> args, Callback callback) const;

private:
    AsyncRedisClient();

    AsyncRedisClient(const AsyncRedisClient&) = delete;
    AsyncRedisClient& operator=(const AsyncRedisClient&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
