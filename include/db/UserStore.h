#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum class UserResultStatus {
    Ok,
    InvalidInput,
    InvalidCredentials,
    Conflict,
    Cancelled,
    Unavailable,
    Error
};

struct RegisterResult {
    bool ok{ false };
    std::string message;
    UserResultStatus status{ UserResultStatus::Error };
};

struct LoginResult {
    bool ok{ false };
    std::string message;
    std::string username;
    std::string display_name;
    UserResultStatus status{ UserResultStatus::Error };
};

enum class ImageReserveStatus {
    Ok,
    Duplicate,
    LimitExceeded,
    Cancelled,
    Unavailable,
    Error
};

struct ImageReserveResult {
    ImageReserveStatus status{ ImageReserveStatus::Error };
    std::string message;
};

enum class ImageDeleteStatus {
    Ok,
    NotFound,
    Forbidden,
    Cancelled,
    CommitUnknown,
    Unavailable,
    Error
};

struct ImageDeleteResult {
    ImageDeleteStatus status{ ImageDeleteStatus::Error };
    std::string message;
};

#ifdef MYWEBSERVER_WITH_MYSQL
struct MysqlConnection;
#endif

class UserStore {
public:
    static UserStore& instance();

    ~UserStore();

    bool init_from_env();

    bool available() const;

    std::string status_message() const;

    RegisterResult register_user(
        const std::string& username,
        const std::string& password,
        const std::string& display_name,
        const std::string& email,
        const std::shared_ptr<std::atomic<bool>>& cancelled = nullptr
    );

    LoginResult login_user(
        const std::string& username,
        const std::string& password
    );

    ImageReserveResult reserve_image(
        const std::string& username,
        const std::string& stored_name,
        const std::string& original_name,
        const std::string& content_hash,
        uint64_t size_bytes,
        size_t max_images,
        const std::shared_ptr<std::atomic<bool>>& cancelled
    );

    ImageDeleteResult check_image_owner(
        const std::string& username,
        const std::string& stored_name,
        const std::shared_ptr<std::atomic<bool>>& cancelled = nullptr
    );

    ImageDeleteResult delete_owned_image(
        const std::string& username,
        const std::string& stored_name,
        const std::shared_ptr<std::atomic<bool>>& cancelled = nullptr
    );

    static std::string sha256(const std::string& value);

private:
    UserStore() = default;

#ifdef MYWEBSERVER_WITH_MYSQL
    MysqlConnection* acquire_connection();

    MysqlConnection* try_acquire_connection();

    bool has_idle_connection() const;

    void release_connection(MysqlConnection* connection);

    void handle_connection_failure(
        MysqlConnection* connection,
        unsigned int error_no,
        const std::string& error
    );

    MysqlConnection* create_connection(std::string& error) const;

    bool repair_connection(MysqlConnection* connection, std::string& error);

    void reset_connection(MysqlConnection* connection) const;

    void close_connection(MysqlConnection* connection) const;

    bool prepare_statements(MysqlConnection* connection, std::string& error) const;

    bool ensure_schema(MysqlConnection* connection);

    bool is_connection_error(unsigned int error_no) const;

    void update_pool_status(const std::string& detail = "");
#endif

    struct CachedLoginUser {
        std::string password_hash;
        std::string display_name;
        std::chrono::steady_clock::time_point expires_at;
    };

    bool get_cached_login_user(
        const std::string& username,
        CachedLoginUser& user
    ) const;

    void put_cached_login_user(
        const std::string& username,
        const std::string& password_hash,
        const std::string& display_name
    ) const;

    void erase_cached_login_user(const std::string& username) const;

private:
    mutable std::mutex mutex_;
    std::condition_variable pool_cv_;
    bool available_{ false };
    std::string status_message_{ "database is not initialized" };

#ifdef MYWEBSERVER_WITH_MYSQL
    std::string host_{ "127.0.0.1" };
    std::string user_{ "root" };
    std::string password_;
    std::string database_{ "my_web_server" };
    unsigned int port_{ 3306 };
    size_t pool_size_{ 8 };
    size_t min_healthy_connections_{ 4 };
    int pool_wait_timeout_ms_{ 100 };

    std::vector<MysqlConnection*> all_connections_;
    std::atomic<size_t> next_connection_{ 0 };
    std::atomic<size_t> healthy_connections_{ 0 };
    std::atomic<bool> pool_available_{ false };
#endif

    int login_cache_ttl_seconds_{ 3 };
    size_t login_cache_capacity_{ 10000 };
    mutable std::shared_mutex login_cache_mutex_;
    mutable std::unordered_map<std::string, CachedLoginUser> login_cache_;
};
