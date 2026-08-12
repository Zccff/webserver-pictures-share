#pragma once

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SessionInfo {
    std::string username;
    std::string display_name;
};

class SessionManager {
public:
    static SessionManager& instance();

    bool init_from_env();

    bool available() const;

    std::string status_message() const;

    std::string create_session(
        const std::string& username,
        const std::string& display_name
    );

    bool get_session(const std::string& token, SessionInfo& session) const;

    void destroy_session(const std::string& token);

private:
    struct RedisConnection {
        int fd{ -1 };
        mutable std::mutex mutex;
    };

    struct CachedSession {
        SessionInfo session;
        std::chrono::steady_clock::time_point expires_at;
    };

private:
    SessionManager() = default;

    static std::string generate_token();

    bool command_with_retry(
        const std::initializer_list<std::string>& args,
        std::string& response
    ) const;

    RedisConnection* pick_connection() const;

    bool connect_locked(RedisConnection& connection) const;

    bool send_all_locked(RedisConnection& connection, const std::string& data) const;

    bool read_line_locked(RedisConnection& connection, std::string& line) const;

    bool read_exact_locked(
        RedisConnection& connection,
        size_t size,
        std::string& value
    ) const;

    bool command_locked(
        RedisConnection& connection,
        const std::initializer_list<std::string>& args,
        std::string& response
    ) const;

    void close_connection_locked(RedisConnection& connection) const;

    void set_status(bool available, const std::string& message) const;

    bool get_cached_session(const std::string& token, SessionInfo& session) const;

    void put_cached_session(const std::string& token, const SessionInfo& session) const;

    void erase_cached_session(const std::string& token) const;

    static std::string build_resp_command(const std::initializer_list<std::string>& args);

    static bool parse_session_payload(const std::string& payload, SessionInfo& session);

    static std::string session_payload(
        const std::string& username,
        const std::string& display_name
    );

private:
    mutable std::mutex state_mutex_;
    std::string host_{ "127.0.0.1" };
    int port_{ 6379 };
    std::string password_;
    int ttl_seconds_{ 86400 };
    int pool_size_{ 8 };
    mutable bool available_{ false };
    mutable std::string status_message_{ "redis session store is not initialized" };

    std::vector<std::unique_ptr<RedisConnection>> connections_;
    mutable std::atomic<size_t> next_connection_{ 0 };

    int cache_ttl_seconds_{ 3 };
    mutable std::shared_mutex cache_mutex_;
    mutable std::unordered_map<std::string, CachedSession> session_cache_;
};
