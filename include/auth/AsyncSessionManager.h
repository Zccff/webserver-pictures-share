#pragma once

#include "auth/SessionInfo.h"

#include <functional>
#include <memory>
#include <string>

class AsyncSessionManager {
public:
    using CreateCallback = std::function<void(std::string token, std::string error)>;
    using GetCallback = std::function<void(
        bool found,
        SessionInfo session,
        std::string error
    )>;
    using DestroyCallback = std::function<void(bool ok, std::string error)>;

    static AsyncSessionManager& instance();

    ~AsyncSessionManager();

    bool init_from_env();

    void shutdown();

    bool available() const;

    std::string status_message() const;

    void create_session(
        const std::string& username,
        const std::string& display_name,
        CreateCallback callback
    );

    void get_session(const std::string& token, GetCallback callback) const;

    void destroy_session(const std::string& token, DestroyCallback callback);

private:
    AsyncSessionManager();

    AsyncSessionManager(const AsyncSessionManager&) = delete;
    AsyncSessionManager& operator=(const AsyncSessionManager&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
