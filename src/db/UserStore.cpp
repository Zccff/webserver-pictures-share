#include "db/UserStore.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

#ifdef MYWEBSERVER_WITH_MYSQL
#include <errmsg.h>
#include <mysql.h>
#endif

namespace {

std::string env_or_default(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value && value[0] != '\0' ? value : fallback;
}

unsigned int env_uint_or_default(const char* key, unsigned int fallback) {
    const char* value = std::getenv(key);

    if (!value || value[0] == '\0') {
        return fallback;
    }

    return static_cast<unsigned int>(std::stoul(value));
}

size_t env_size_or_default(const char* key, size_t fallback) {
    const char* value = std::getenv(key);

    if (!value || value[0] == '\0') {
        return fallback;
    }

    size_t parsed = static_cast<size_t>(std::stoul(value));
    return parsed == 0 ? fallback : parsed;
}

bool is_safe_identifier(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    for (char ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            return false;
        }
    }

    return true;
}

uint32_t rotr(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::string sha256_hex(const std::string& input) {
    static constexpr std::array<uint32_t, 64> k{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    std::array<uint32_t, 8> h{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    std::string data = input;
    uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8U;

    data.push_back(static_cast<char>(0x80));
    while ((data.size() % 64) != 56) {
        data.push_back('\0');
    }

    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<char>((bit_length >> shift) & 0xffU));
    }

    for (size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<uint32_t, 64> w{};

        for (size_t i = 0; i < 16; ++i) {
            size_t j = offset + i * 4;
            w[i] =
                (static_cast<uint32_t>(static_cast<unsigned char>(data[j])) << 24) |
                (static_cast<uint32_t>(static_cast<unsigned char>(data[j + 1])) << 16) |
                (static_cast<uint32_t>(static_cast<unsigned char>(data[j + 2])) << 8) |
                (static_cast<uint32_t>(static_cast<unsigned char>(data[j + 3])));
        }

        for (size_t i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];
        uint32_t f = h[5];
        uint32_t g = h[6];
        uint32_t hh = h[7];

        for (size_t i = 0; i < 64; ++i) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = s0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    for (uint32_t value : h) {
        oss << std::setw(8) << value;
    }

    return oss.str();
}

std::string password_hash_for(const std::string& password) {
    return sha256_hex("my_web_server:" + password);
}

std::string normalize_hash(std::string hash) {
    std::transform(
        hash.begin(),
        hash.end(),
        hash.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    return hash;
}

bool constant_time_equal(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    unsigned char diff = 0;

    for (size_t i = 0; i < lhs.size(); ++i) {
        diff |= static_cast<unsigned char>(lhs[i] ^ rhs[i]);
    }

    return diff == 0;
}

}

#ifdef MYWEBSERVER_WITH_MYSQL
struct MysqlConnection {
    MYSQL* mysql{ nullptr };
    MYSQL_STMT* register_stmt{ nullptr };
    MYSQL_STMT* login_stmt{ nullptr };
    MYSQL_STMT* image_lock_user_stmt{ nullptr };
    MYSQL_STMT* image_count_stmt{ nullptr };
    MYSQL_STMT* image_insert_stmt{ nullptr };
    MYSQL_STMT* image_owner_stmt{ nullptr };
    MYSQL_STMT* image_delete_stmt{ nullptr };
    std::atomic<bool> in_use{ false };
    std::atomic<bool> healthy{ false };
};
#endif

UserStore& UserStore::instance() {
    static UserStore store;
    return store;
}

std::string UserStore::sha256(const std::string& value) {
    return sha256_hex(value);
}

UserStore::~UserStore() {
#ifdef MYWEBSERVER_WITH_MYSQL
    std::lock_guard<std::mutex> lock(mutex_);

    for (MysqlConnection* connection : all_connections_) {
        close_connection(connection);
    }

    all_connections_.clear();
#endif
}

bool UserStore::init_from_env() {
#ifdef MYWEBSERVER_WITH_MYSQL
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!all_connections_.empty()) {
            size_t healthy = healthy_connections_.load(std::memory_order_acquire);
            available_ = healthy >= min_healthy_connections_;
            return available_;
        }

        host_ = env_or_default("MYSQL_HOST", "127.0.0.1");
        user_ = env_or_default("MYSQL_USER", "root");
        password_ = env_or_default("MYSQL_PASSWORD", "");
        database_ = env_or_default("MYSQL_DATABASE", "my_web_server");
        port_ = env_uint_or_default("MYSQL_PORT", 3306);
        pool_size_ = env_size_or_default("MYSQL_POOL_SIZE", 8);
        min_healthy_connections_ = (pool_size_ + 1) / 2;
        login_cache_ttl_seconds_ = static_cast<int>(
            env_size_or_default("LOGIN_CACHE_TTL_SECONDS", 3)
        );
        login_cache_capacity_ = env_size_or_default("LOGIN_CACHE_CAPACITY", 10000);
        pool_wait_timeout_ms_ = static_cast<int>(
            env_size_or_default("MYSQL_POOL_WAIT_TIMEOUT_MS", 100)
        );

        if (!is_safe_identifier(database_)) {
            available_ = false;
            pool_available_.store(false, std::memory_order_release);
            status_message_ = "MYSQL_DATABASE may only contain letters, digits, and underscore";
            return false;
        }

        available_ = false;
        pool_available_.store(false, std::memory_order_release);
        status_message_ = "mysql pool is connecting";
    }

    std::vector<MysqlConnection*> slots;
    slots.reserve(pool_size_);
    size_t healthy = 0;
    bool schema_ready = false;
    std::string last_error;

    for (size_t i = 0; i < pool_size_; ++i) {
        std::string error;
        MysqlConnection* connection = create_connection(error);

        if (!connection) {
            last_error = error;
            slots.push_back(new MysqlConnection());
            continue;
        }

        if (!schema_ready && !ensure_schema(connection)) {
            last_error = status_message();
            close_connection(connection);
            slots.push_back(new MysqlConnection());
            continue;
        }

        schema_ready = true;

        if (!prepare_statements(connection, error)) {
            last_error = error;
            close_connection(connection);
            slots.push_back(new MysqlConnection());
            continue;
        }

        connection->healthy.store(true, std::memory_order_release);
        slots.push_back(connection);
        ++healthy;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (MysqlConnection* connection : slots) {
            all_connections_.push_back(connection);
        }

        healthy_connections_.store(healthy, std::memory_order_release);
        available_ = healthy >= min_healthy_connections_;
        pool_available_.store(true, std::memory_order_release);

        std::ostringstream oss;
        oss << "mysql pool healthy=" << healthy << "/" << pool_size_
            << ", required=" << min_healthy_connections_;

        if (schema_ready) {
            oss << ", username unique index enabled";
        }
        else {
            oss << ", schema unavailable";
        }

        oss
            << ", login_cache_ttl=" << login_cache_ttl_seconds_ << "s";

        if (!last_error.empty()) {
            oss << ", last_error=" << last_error;
        }

        status_message_ = oss.str();
    }

    pool_cv_.notify_all();
    return healthy >= min_healthy_connections_;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    available_ = false;
    status_message_ = "server was built without MySQL support";
    return false;
#endif
}

bool UserStore::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_;
}

std::string UserStore::status_message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_message_;
}

bool UserStore::get_cached_login_user(
    const std::string& username,
    CachedLoginUser& user
) const {
    if (login_cache_ttl_seconds_ <= 0 || username.empty()) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();

    {
        std::shared_lock<std::shared_mutex> lock(login_cache_mutex_);
        auto it = login_cache_.find(username);

        if (it == login_cache_.end()) {
            return false;
        }

        if (now < it->second.expires_at) {
            user = it->second;
            return true;
        }
    }

    std::unique_lock<std::shared_mutex> lock(login_cache_mutex_);
    auto it = login_cache_.find(username);

    if (it == login_cache_.end()) {
        return false;
    }

    if (std::chrono::steady_clock::now() < it->second.expires_at) {
        user = it->second;
        return true;
    }

    login_cache_.erase(it);
    return false;
}

void UserStore::put_cached_login_user(
    const std::string& username,
    const std::string& password_hash,
    const std::string& display_name
) const {
    if (login_cache_ttl_seconds_ <= 0 || username.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(login_cache_mutex_);

    if (login_cache_.size() >= login_cache_capacity_ &&
        login_cache_.find(username) == login_cache_.end()) {
        auto now = std::chrono::steady_clock::now();

        for (auto it = login_cache_.begin(); it != login_cache_.end();) {
            if (now >= it->second.expires_at) {
                it = login_cache_.erase(it);
            }
            else {
                ++it;
            }
        }

        if (login_cache_.size() >= login_cache_capacity_) {
            login_cache_.erase(login_cache_.begin());
        }
    }

    login_cache_[username] = {
        normalize_hash(password_hash),
        display_name,
        std::chrono::steady_clock::now() +
            std::chrono::seconds(login_cache_ttl_seconds_)
    };
}

void UserStore::erase_cached_login_user(const std::string& username) const {
    if (username.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(login_cache_mutex_);
    login_cache_.erase(username);
}

RegisterResult UserStore::register_user(
    const std::string& username,
    const std::string& password,
    const std::string& display_name,
    const std::string& email,
    const std::shared_ptr<std::atomic<bool>>& cancelled
) {
    if (username.empty() || password.empty()) {
        return {
            false,
            "username and password are required",
            UserResultStatus::InvalidInput
        };
    }

#ifdef MYWEBSERVER_WITH_MYSQL
    auto is_cancelled = [&cancelled]() {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };

    if (is_cancelled()) {
        return { false, "request was cancelled", UserResultStatus::Cancelled };
    }

    MysqlConnection* connection = acquire_connection();

    if (!connection) {
        return { false, status_message(), UserResultStatus::Unavailable };
    }

    MYSQL_STMT* stmt = connection->register_stmt;

    auto fail = [&](unsigned int error_no, const std::string& error) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return RegisterResult{ false, error, UserResultStatus::Error };
    };

    if (mysql_stmt_reset(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        return fail(error_no, error);
    }

    std::string safe_display_name = display_name.empty() ? username : display_name;
    std::string password_hash = password_hash_for(password);

    MYSQL_BIND bind[4]{};
    unsigned long lengths[4]{
        static_cast<unsigned long>(username.size()),
        static_cast<unsigned long>(password_hash.size()),
        static_cast<unsigned long>(safe_display_name.size()),
        static_cast<unsigned long>(email.size())
    };

    std::string values[4]{
        username,
        password_hash,
        safe_display_name,
        email
    };

    for (int i = 0; i < 4; ++i) {
        bind[i].buffer_type = MYSQL_TYPE_STRING;
        bind[i].buffer = const_cast<char*>(values[i].c_str());
        bind[i].buffer_length = lengths[i];
        bind[i].length = &lengths[i];
    }

    if (mysql_stmt_bind_param(stmt, bind)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        return fail(error_no, error);
    }

    if (mysql_autocommit(connection->mysql, 0)) {
        unsigned int error_no = mysql_errno(connection->mysql);
        std::string error = mysql_error(connection->mysql);
        return fail(error_no, error);
    }

    if (is_cancelled()) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return { false, "request was cancelled", UserResultStatus::Cancelled };
    }

    if (mysql_stmt_execute(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);

        if (error_no == 1062) {
            return {
                false,
                "username already exists",
                UserResultStatus::Conflict
            };
        }

        return { false, error, UserResultStatus::Error };
    }

    if (is_cancelled()) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return { false, "request was cancelled", UserResultStatus::Cancelled };
    }

    if (mysql_commit(connection->mysql) || mysql_autocommit(connection->mysql, 1)) {
        unsigned int error_no = mysql_errno(connection->mysql);
        std::string error = mysql_error(connection->mysql);
        return fail(error_no, error);
    }

    release_connection(connection);
    put_cached_login_user(username, password_hash, safe_display_name);
    return { true, "registered", UserResultStatus::Ok };
#else
    (void)cancelled;
    return { false, status_message(), UserResultStatus::Unavailable };
#endif
}

LoginResult UserStore::login_user(
    const std::string& username,
    const std::string& password
) {
    if (username.empty() || password.empty()) {
        return {
            false,
            "username and password are required",
            "",
            "",
            UserResultStatus::InvalidInput
        };
    }

#ifdef MYWEBSERVER_WITH_MYSQL
    std::string expected_hash = password_hash_for(password);
    CachedLoginUser cached_user;

    if (get_cached_login_user(username, cached_user)) {
        if (!constant_time_equal(cached_user.password_hash, expected_hash)) {
            return {
                false,
                "invalid username or password",
                "",
                "",
                UserResultStatus::InvalidCredentials
            };
        }

        return {
            true,
            "logged in",
            username,
            cached_user.display_name,
            UserResultStatus::Ok
        };
    }

    MysqlConnection* connection = acquire_connection();

    if (!connection) {
        return {
            false,
            status_message(),
            "",
            "",
            UserResultStatus::Unavailable
        };
    }

    MYSQL_STMT* stmt = connection->login_stmt;

    if (mysql_stmt_reset(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "", UserResultStatus::Error };
    }

    MYSQL_BIND bind[1]{};
    unsigned long param_lengths[1]{
        static_cast<unsigned long>(username.size())
    };
    std::string params[1]{ username };

    for (int i = 0; i < 1; ++i) {
        bind[i].buffer_type = MYSQL_TYPE_STRING;
        bind[i].buffer = const_cast<char*>(params[i].c_str());
        bind[i].buffer_length = param_lengths[i];
        bind[i].length = &param_lengths[i];
    }

    if (mysql_stmt_bind_param(stmt, bind)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "", UserResultStatus::Error };
    }

    if (mysql_stmt_execute(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "", UserResultStatus::Error };
    }

    char username_buf[256]{};
    char password_hash_buf[128]{};
    char display_name_buf[256]{};
    unsigned long result_lengths[3]{};

    MYSQL_BIND result[3]{};
    result[0].buffer_type = MYSQL_TYPE_STRING;
    result[0].buffer = username_buf;
    result[0].buffer_length = sizeof(username_buf);
    result[0].length = &result_lengths[0];

    result[1].buffer_type = MYSQL_TYPE_STRING;
    result[1].buffer = password_hash_buf;
    result[1].buffer_length = sizeof(password_hash_buf);
    result[1].length = &result_lengths[1];

    result[2].buffer_type = MYSQL_TYPE_STRING;
    result[2].buffer = display_name_buf;
    result[2].buffer_length = sizeof(display_name_buf);
    result[2].length = &result_lengths[2];

    if (mysql_stmt_bind_result(stmt, result)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "", UserResultStatus::Error };
    }

    int fetch_status = mysql_stmt_fetch(stmt);

    if (fetch_status == MYSQL_NO_DATA) {
        mysql_stmt_free_result(stmt);
        release_connection(connection);
        return {
            false,
            "invalid username or password",
            "",
            "",
            UserResultStatus::InvalidCredentials
        };
    }

    if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        mysql_stmt_free_result(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "", UserResultStatus::Error };
    }

    std::string result_username(username_buf, result_lengths[0]);
    std::string stored_hash = normalize_hash(
        std::string(password_hash_buf, result_lengths[1])
    );
    std::string result_display_name(display_name_buf, result_lengths[2]);

    mysql_stmt_free_result(stmt);
    release_connection(connection);

    put_cached_login_user(result_username, stored_hash, result_display_name);

    if (!constant_time_equal(stored_hash, expected_hash)) {
        return {
            false,
            "invalid username or password",
            "",
            "",
            UserResultStatus::InvalidCredentials
        };
    }

    return {
        true,
        "logged in",
        result_username,
        result_display_name,
        UserResultStatus::Ok
    };
#else
    return {
        false,
        status_message(),
        "",
        "",
        UserResultStatus::Unavailable
    };
#endif
}

ImageReserveResult UserStore::reserve_image(
    const std::string& username,
    const std::string& stored_name,
    const std::string& original_name,
    const std::string& content_hash,
    uint64_t size_bytes,
    size_t max_images,
    const std::shared_ptr<std::atomic<bool>>& cancelled
) {
#ifdef MYWEBSERVER_WITH_MYSQL
    auto is_cancelled = [&cancelled]() {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };

    if (is_cancelled()) {
        return { ImageReserveStatus::Cancelled, "request was cancelled" };
    }

    MysqlConnection* connection = acquire_connection();

    if (!connection) {
        return { ImageReserveStatus::Unavailable, "database pool is busy or unavailable" };
    }

    auto fail = [&](MYSQL_STMT* stmt, const std::string& prefix) {
        unsigned int error_no = stmt ? mysql_stmt_errno(stmt) : mysql_errno(connection->mysql);
        std::string error = stmt ? mysql_stmt_error(stmt) : mysql_error(connection->mysql);
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return ImageReserveResult{
            ImageReserveStatus::Error,
            prefix + (error.empty() ? "mysql operation failed" : error)
        };
    };

    if (mysql_autocommit(connection->mysql, 0)) {
        return fail(nullptr, "start image transaction failed: ");
    }

    auto cancel_transaction = [&]() {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return ImageReserveResult{
            ImageReserveStatus::Cancelled,
            "request was cancelled"
        };
    };

    if (is_cancelled()) {
        return cancel_transaction();
    }

    MYSQL_STMT* lock_user = connection->image_lock_user_stmt;
    mysql_stmt_reset(lock_user);
    MYSQL_BIND user_param[1]{};
    unsigned long username_length = static_cast<unsigned long>(username.size());
    user_param[0].buffer_type = MYSQL_TYPE_STRING;
    user_param[0].buffer = const_cast<char*>(username.data());
    user_param[0].buffer_length = username_length;
    user_param[0].length = &username_length;

    if (mysql_stmt_bind_param(lock_user, user_param) || mysql_stmt_execute(lock_user)) {
        return fail(lock_user, "lock user for image upload failed: ");
    }

    unsigned long long user_id = 0;
    MYSQL_BIND user_result[1]{};
    user_result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    user_result[0].buffer = &user_id;
    user_result[0].is_unsigned = 1;

    if (mysql_stmt_bind_result(lock_user, user_result)) {
        return fail(lock_user, "read upload owner failed: ");
    }

    int user_fetch = mysql_stmt_fetch(lock_user);
    mysql_stmt_free_result(lock_user);

    if (user_fetch == MYSQL_NO_DATA) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return { ImageReserveStatus::Error, "upload owner does not exist" };
    }

    if (user_fetch != 0 && user_fetch != MYSQL_DATA_TRUNCATED) {
        return fail(lock_user, "read upload owner failed: ");
    }

    MYSQL_STMT* count_stmt = connection->image_count_stmt;
    mysql_stmt_reset(count_stmt);

    if (mysql_stmt_bind_param(count_stmt, user_param) || mysql_stmt_execute(count_stmt)) {
        return fail(count_stmt, "count user images failed: ");
    }

    unsigned long long image_count = 0;
    MYSQL_BIND count_result[1]{};
    count_result[0].buffer_type = MYSQL_TYPE_LONGLONG;
    count_result[0].buffer = &image_count;
    count_result[0].is_unsigned = 1;

    if (mysql_stmt_bind_result(count_stmt, count_result) ||
        mysql_stmt_fetch(count_stmt) != 0) {
        mysql_stmt_free_result(count_stmt);
        return fail(count_stmt, "read user image count failed: ");
    }

    mysql_stmt_free_result(count_stmt);

    if (is_cancelled()) {
        return cancel_transaction();
    }

    if (image_count >= max_images) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return { ImageReserveStatus::LimitExceeded, "image count limit reached" };
    }

    MYSQL_STMT* insert_stmt = connection->image_insert_stmt;
    mysql_stmt_reset(insert_stmt);
    std::string values[4]{ username, stored_name, original_name, content_hash };
    unsigned long lengths[4]{
        static_cast<unsigned long>(username.size()),
        static_cast<unsigned long>(stored_name.size()),
        static_cast<unsigned long>(original_name.size()),
        static_cast<unsigned long>(content_hash.size())
    };
    unsigned long long mysql_size = size_bytes;
    MYSQL_BIND insert_params[5]{};

    for (int i = 0; i < 4; ++i) {
        insert_params[i].buffer_type = MYSQL_TYPE_STRING;
        insert_params[i].buffer = const_cast<char*>(values[i].data());
        insert_params[i].buffer_length = lengths[i];
        insert_params[i].length = &lengths[i];
    }

    insert_params[4].buffer_type = MYSQL_TYPE_LONGLONG;
    insert_params[4].buffer = &mysql_size;
    insert_params[4].is_unsigned = 1;

    if (mysql_stmt_bind_param(insert_stmt, insert_params) ||
        mysql_stmt_execute(insert_stmt)) {
        unsigned int error_no = mysql_stmt_errno(insert_stmt);

        if (error_no == 1062) {
            mysql_rollback(connection->mysql);
            mysql_autocommit(connection->mysql, 1);
            release_connection(connection);
            return { ImageReserveStatus::Duplicate, "the same image was already uploaded by this user" };
        }

        return fail(insert_stmt, "save image metadata failed: ");
    }

    if (is_cancelled()) {
        return cancel_transaction();
    }

    if (mysql_commit(connection->mysql) || mysql_autocommit(connection->mysql, 1)) {
        return fail(nullptr, "commit image metadata failed: ");
    }

    release_connection(connection);
    return { ImageReserveStatus::Ok, "image reserved" };
#else
    (void)username;
    (void)stored_name;
    (void)original_name;
    (void)content_hash;
    (void)size_bytes;
    (void)max_images;
    (void)cancelled;
    return { ImageReserveStatus::Unavailable, status_message() };
#endif
}

ImageDeleteResult UserStore::check_image_owner(
    const std::string& username,
    const std::string& stored_name,
    const std::shared_ptr<std::atomic<bool>>& cancelled
) {
#ifdef MYWEBSERVER_WITH_MYSQL
    auto is_cancelled = [&cancelled]() {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };
    if (is_cancelled()) {
        return { ImageDeleteStatus::Cancelled, "request was cancelled" };
    }

    MysqlConnection* connection = acquire_connection();
    if (!connection) {
        return {
            ImageDeleteStatus::Unavailable,
            "database pool is busy or unavailable"
        };
    }

    auto fail = [&](MYSQL_STMT* stmt, const std::string& prefix) {
        unsigned int error_no = stmt
            ? mysql_stmt_errno(stmt)
            : mysql_errno(connection->mysql);
        std::string error = stmt
            ? mysql_stmt_error(stmt)
            : mysql_error(connection->mysql);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return ImageDeleteResult{ ImageDeleteStatus::Error, prefix + error };
    };

    MYSQL_STMT* owner_stmt = connection->image_owner_stmt;
    mysql_stmt_reset(owner_stmt);
    unsigned long stored_length = static_cast<unsigned long>(stored_name.size());
    MYSQL_BIND owner_param[1]{};
    owner_param[0].buffer_type = MYSQL_TYPE_STRING;
    owner_param[0].buffer = const_cast<char*>(stored_name.data());
    owner_param[0].buffer_length = stored_length;
    owner_param[0].length = &stored_length;
    if (mysql_stmt_bind_param(owner_stmt, owner_param) ||
        mysql_stmt_execute(owner_stmt)) {
        return fail(owner_stmt, "read image owner failed: ");
    }

    char owner_buffer[65]{};
    unsigned long owner_length = 0;
    MYSQL_BIND owner_result[1]{};
    owner_result[0].buffer_type = MYSQL_TYPE_STRING;
    owner_result[0].buffer = owner_buffer;
    owner_result[0].buffer_length = sizeof(owner_buffer);
    owner_result[0].length = &owner_length;
    if (mysql_stmt_bind_result(owner_stmt, owner_result)) {
        return fail(owner_stmt, "read image owner failed: ");
    }

    int fetch_status = mysql_stmt_fetch(owner_stmt);
    mysql_stmt_free_result(owner_stmt);
    if (fetch_status == MYSQL_NO_DATA) {
        release_connection(connection);
        return { ImageDeleteStatus::NotFound, "image metadata not found" };
    }
    if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
        return fail(owner_stmt, "read image owner failed: ");
    }

    const bool owner_matches =
        std::string(owner_buffer, owner_length) == username;
    release_connection(connection);
    if (is_cancelled()) {
        return { ImageDeleteStatus::Cancelled, "request was cancelled" };
    }
    if (!owner_matches) {
        return {
            ImageDeleteStatus::Forbidden,
            "only the uploader may delete this image"
        };
    }
    return { ImageDeleteStatus::Ok, "image ownership confirmed" };
#else
    (void)username;
    (void)stored_name;
    (void)cancelled;
    return { ImageDeleteStatus::Unavailable, status_message() };
#endif
}

ImageDeleteResult UserStore::delete_owned_image(
    const std::string& username,
    const std::string& stored_name,
    const std::shared_ptr<std::atomic<bool>>& cancelled
) {
#ifdef MYWEBSERVER_WITH_MYSQL
    auto is_cancelled = [&cancelled]() {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };

    if (is_cancelled()) {
        return { ImageDeleteStatus::Cancelled, "request was cancelled" };
    }

    MysqlConnection* connection = acquire_connection();

    if (!connection) {
        return { ImageDeleteStatus::Unavailable, "database pool is busy or unavailable" };
    }

    auto statement_error = [&](MYSQL_STMT* stmt, const std::string& prefix) {
        unsigned int error_no = stmt
            ? mysql_stmt_errno(stmt)
            : mysql_errno(connection->mysql);
        std::string error = stmt
            ? mysql_stmt_error(stmt)
            : mysql_error(connection->mysql);
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return ImageDeleteResult{ ImageDeleteStatus::Error, prefix + error };
    };

    auto cancel_transaction = [&]() {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return ImageDeleteResult{ ImageDeleteStatus::Cancelled, "request was cancelled" };
    };

    if (mysql_autocommit(connection->mysql, 0)) {
        return statement_error(nullptr, "start image delete transaction failed: ");
    }

    if (is_cancelled()) {
        return cancel_transaction();
    }

    MYSQL_STMT* owner_stmt = connection->image_owner_stmt;
    mysql_stmt_reset(owner_stmt);
    unsigned long stored_length = static_cast<unsigned long>(stored_name.size());
    MYSQL_BIND owner_param[1]{};
    owner_param[0].buffer_type = MYSQL_TYPE_STRING;
    owner_param[0].buffer = const_cast<char*>(stored_name.data());
    owner_param[0].buffer_length = stored_length;
    owner_param[0].length = &stored_length;

    if (mysql_stmt_bind_param(owner_stmt, owner_param) ||
        mysql_stmt_execute(owner_stmt)) {
        return statement_error(owner_stmt, "read image owner failed: ");
    }

    char owner_buffer[65]{};
    unsigned long owner_length = 0;
    MYSQL_BIND owner_result[1]{};
    owner_result[0].buffer_type = MYSQL_TYPE_STRING;
    owner_result[0].buffer = owner_buffer;
    owner_result[0].buffer_length = sizeof(owner_buffer);
    owner_result[0].length = &owner_length;

    if (mysql_stmt_bind_result(owner_stmt, owner_result)) {
        return statement_error(owner_stmt, "read image owner failed: ");
    }

    int fetch_status = mysql_stmt_fetch(owner_stmt);
    mysql_stmt_free_result(owner_stmt);

    if (fetch_status == MYSQL_NO_DATA) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return { ImageDeleteStatus::NotFound, "image metadata not found" };
    }

    if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
        return statement_error(owner_stmt, "read image owner failed: ");
    }

    if (std::string(owner_buffer, owner_length) != username) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return { ImageDeleteStatus::Forbidden, "only the uploader may delete this image" };
    }

    if (is_cancelled()) {
        return cancel_transaction();
    }

    MYSQL_STMT* delete_stmt = connection->image_delete_stmt;
    mysql_stmt_reset(delete_stmt);
    std::string values[2]{ username, stored_name };
    unsigned long lengths[2]{
        static_cast<unsigned long>(username.size()),
        static_cast<unsigned long>(stored_name.size())
    };
    MYSQL_BIND delete_params[2]{};

    for (int i = 0; i < 2; ++i) {
        delete_params[i].buffer_type = MYSQL_TYPE_STRING;
        delete_params[i].buffer = const_cast<char*>(values[i].data());
        delete_params[i].buffer_length = lengths[i];
        delete_params[i].length = &lengths[i];
    }

    if (mysql_stmt_bind_param(delete_stmt, delete_params) ||
        mysql_stmt_execute(delete_stmt)) {
        return statement_error(delete_stmt, "delete image metadata failed: ");
    }

    if (mysql_stmt_affected_rows(delete_stmt) == 0) {
        mysql_rollback(connection->mysql);
        mysql_autocommit(connection->mysql, 1);
        release_connection(connection);
        return { ImageDeleteStatus::NotFound, "image metadata not found" };
    }

    if (is_cancelled()) {
        return cancel_transaction();
    }

    if (mysql_commit(connection->mysql)) {
        const unsigned int error_no = mysql_errno(connection->mysql);
        const std::string error = mysql_error(connection->mysql);
        // The server may have committed immediately before the connection was
        // lost. Do not report an ordinary failure: the caller must resolve the
        // outcome with a fresh ownership read before restoring the file.
        mysql_autocommit(connection->mysql, 1);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return {
            ImageDeleteStatus::CommitUnknown,
            "image delete commit outcome is unknown: " + error
        };
    }

    if (mysql_autocommit(connection->mysql, 1)) {
        const std::string error = mysql_error(connection->mysql);
        // COMMIT already succeeded. Retire/repair this slot, but never make the
        // upper layer restore a file whose database row is gone.
        handle_connection_failure(connection, 2006, error);
        release_connection(connection);
        return {
            ImageDeleteStatus::Ok,
            "image metadata deleted; database connection was repaired"
        };
    }

    release_connection(connection);
    return { ImageDeleteStatus::Ok, "image metadata deleted" };
#else
    (void)username;
    (void)stored_name;
    (void)cancelled;
    return { ImageDeleteStatus::Unavailable, status_message() };
#endif
}

#ifdef MYWEBSERVER_WITH_MYSQL
MysqlConnection* UserStore::acquire_connection() {
    if (!pool_available_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    if (MysqlConnection* connection = try_acquire_connection()) {
        return connection;
    }

    if (healthy_connections_.load(std::memory_order_acquire) == 0) {
        return nullptr;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    bool ready = pool_cv_.wait_for(
        lock,
        std::chrono::milliseconds(pool_wait_timeout_ms_),
        [this]() {
        return !pool_available_.load(std::memory_order_acquire) ||
            has_idle_connection();
        }
    );

    if (!ready || !pool_available_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // try_acquire_connection may repair a broken slot. Repair updates pool
    // status and locks mutex_, so never call it while holding mutex_.
    lock.unlock();
    return try_acquire_connection();
}

MysqlConnection* UserStore::try_acquire_connection() {
    size_t count = all_connections_.size();

    if (count == 0) {
        return nullptr;
    }

    size_t start = next_connection_.fetch_add(1, std::memory_order_relaxed);

    // Healthy slots stay on the fast path. A broken slot must not add reconnect
    // latency while another healthy connection is immediately available.
    for (size_t i = 0; i < count; ++i) {
        MysqlConnection* connection = all_connections_[(start + i) % count];

        if (!connection->healthy.load(std::memory_order_acquire)) {
            continue;
        }

        bool expected = false;

        if (connection->in_use.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed
        )) {
            return connection;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        MysqlConnection* connection = all_connections_[(start + i) % count];

        if (connection->healthy.load(std::memory_order_acquire)) {
            continue;
        }

        bool expected = false;

        if (connection->in_use.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed
        )) {

            std::string error;

            if (repair_connection(connection, error)) {
                return connection;
            }

            release_connection(connection);
        }
    }

    return nullptr;
}

bool UserStore::has_idle_connection() const {
    for (MysqlConnection* connection : all_connections_) {
        if (connection->healthy.load(std::memory_order_acquire) &&
            !connection->in_use.load(std::memory_order_acquire)) {
            return true;
        }
    }

    return false;
}

void UserStore::release_connection(MysqlConnection* connection) {
    if (!connection) {
        return;
    }

    connection->in_use.store(false, std::memory_order_release);
    pool_cv_.notify_one();
}

void UserStore::handle_connection_failure(
    MysqlConnection* connection,
    unsigned int error_no,
    const std::string& error
) {
    if (!connection || !is_connection_error(error_no)) {
        return;
    }

    if (connection->healthy.exchange(false, std::memory_order_acq_rel)) {
        healthy_connections_.fetch_sub(1, std::memory_order_acq_rel);
    }

    reset_connection(connection);
    update_pool_status("repairing failed connection: " + error);

    std::string repair_error;

    if (!repair_connection(connection, repair_error)) {
        update_pool_status(
            "connection error=" + error +
            ", reconnect failed=" + repair_error
        );
    }
}

MysqlConnection* UserStore::create_connection(std::string& error) const {
    MYSQL* mysql = mysql_init(nullptr);

    if (!mysql) {
        error = "mysql_init failed";
        return nullptr;
    }

    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    unsigned int connect_timeout_seconds = 2;
    unsigned int io_timeout_seconds = 3;
    mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_seconds);
    mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &io_timeout_seconds);
    mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &io_timeout_seconds);

    if (!mysql_real_connect(
        mysql,
        host_.c_str(),
        user_.c_str(),
        password_.c_str(),
        nullptr,
        port_,
        nullptr,
        0
    )) {
        error = "mysql connect failed: " + std::string(mysql_error(mysql));
        mysql_close(mysql);
        return nullptr;
    }

    std::string create_database_sql =
        "CREATE DATABASE IF NOT EXISTS `" + database_ + "` "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci";

    if (mysql_query(mysql, create_database_sql.c_str())) {
        error = "create database failed: " + std::string(mysql_error(mysql));
        mysql_close(mysql);
        return nullptr;
    }

    if (mysql_select_db(mysql, database_.c_str())) {
        error = "select database failed: " + std::string(mysql_error(mysql));
        mysql_close(mysql);
        return nullptr;
    }

    MysqlConnection* connection = new MysqlConnection();
    connection->mysql = mysql;
    return connection;
}

bool UserStore::repair_connection(
    MysqlConnection* connection,
    std::string& error
) {
    if (!connection) {
        error = "mysql connection slot is null";
        return false;
    }

    MysqlConnection* replacement = create_connection(error);

    if (!replacement) {
        return false;
    }

    if (!ensure_schema(replacement) ||
        !prepare_statements(replacement, error)) {
        close_connection(replacement);
        return false;
    }

    reset_connection(connection);
    connection->mysql = replacement->mysql;
    connection->register_stmt = replacement->register_stmt;
    connection->login_stmt = replacement->login_stmt;
    connection->image_lock_user_stmt = replacement->image_lock_user_stmt;
    connection->image_count_stmt = replacement->image_count_stmt;
    connection->image_insert_stmt = replacement->image_insert_stmt;
    connection->image_owner_stmt = replacement->image_owner_stmt;
    connection->image_delete_stmt = replacement->image_delete_stmt;

    replacement->mysql = nullptr;
    replacement->register_stmt = nullptr;
    replacement->login_stmt = nullptr;
    replacement->image_lock_user_stmt = nullptr;
    replacement->image_count_stmt = nullptr;
    replacement->image_insert_stmt = nullptr;
    replacement->image_owner_stmt = nullptr;
    replacement->image_delete_stmt = nullptr;
    delete replacement;

    if (!connection->healthy.exchange(true, std::memory_order_acq_rel)) {
        healthy_connections_.fetch_add(1, std::memory_order_acq_rel);
    }

    update_pool_status();
    pool_cv_.notify_all();
    return true;
}

void UserStore::reset_connection(MysqlConnection* connection) const {
    if (!connection) {
        return;
    }

    if (connection->register_stmt) {
        mysql_stmt_close(connection->register_stmt);
        connection->register_stmt = nullptr;
    }

    if (connection->login_stmt) {
        mysql_stmt_close(connection->login_stmt);
        connection->login_stmt = nullptr;
    }

    MYSQL_STMT** image_statements[] = {
        &connection->image_lock_user_stmt,
        &connection->image_count_stmt,
        &connection->image_insert_stmt,
        &connection->image_owner_stmt,
        &connection->image_delete_stmt
    };

    for (MYSQL_STMT** statement : image_statements) {
        if (*statement) {
            mysql_stmt_close(*statement);
            *statement = nullptr;
        }
    }

    if (connection->mysql) {
        mysql_close(connection->mysql);
        connection->mysql = nullptr;
    }
}

void UserStore::close_connection(MysqlConnection* connection) const {
    if (!connection) {
        return;
    }

    reset_connection(connection);
    delete connection;
}

bool UserStore::prepare_statements(
    MysqlConnection* connection,
    std::string& error
) const {
    connection->register_stmt = mysql_stmt_init(connection->mysql);

    if (!connection->register_stmt) {
        error = "mysql_stmt_init register failed";
        return false;
    }

    const char* register_sql =
        "INSERT INTO users(username, password_hash, display_name, email) "
        "VALUES (?, ?, ?, ?)";

    if (mysql_stmt_prepare(
        connection->register_stmt,
        register_sql,
        static_cast<unsigned long>(std::strlen(register_sql))
    )) {
        error = mysql_stmt_error(connection->register_stmt);
        return false;
    }

    connection->login_stmt = mysql_stmt_init(connection->mysql);

    if (!connection->login_stmt) {
        error = "mysql_stmt_init login failed";
        return false;
    }

    const char* login_sql =
        "SELECT username, password_hash, display_name FROM users "
        "WHERE username = ? "
        "LIMIT 1";

    if (mysql_stmt_prepare(
        connection->login_stmt,
        login_sql,
        static_cast<unsigned long>(std::strlen(login_sql))
    )) {
        error = mysql_stmt_error(connection->login_stmt);
        return false;
    }

    struct StatementSpec {
        MYSQL_STMT** target;
        const char* sql;
        const char* name;
    };

    StatementSpec image_statements[] = {
        {
            &connection->image_lock_user_stmt,
            "SELECT id FROM users WHERE username = ? FOR UPDATE",
            "image lock user"
        },
        {
            &connection->image_count_stmt,
            "SELECT COUNT(*) FROM images WHERE owner_username = ?",
            "image count"
        },
        {
            &connection->image_insert_stmt,
            "INSERT INTO images(owner_username, stored_name, original_name, content_hash, size_bytes) VALUES (?, ?, ?, ?, ?)",
            "image insert"
        },
        {
            &connection->image_owner_stmt,
            "SELECT owner_username FROM images WHERE stored_name = ? LIMIT 1 FOR UPDATE",
            "image owner"
        },
        {
            &connection->image_delete_stmt,
            "DELETE FROM images WHERE owner_username = ? AND stored_name = ?",
            "image delete"
        }
    };

    for (const StatementSpec& spec : image_statements) {
        *spec.target = mysql_stmt_init(connection->mysql);

        if (!*spec.target) {
            error = std::string("mysql_stmt_init ") + spec.name + " failed";
            return false;
        }

        if (mysql_stmt_prepare(
            *spec.target,
            spec.sql,
            static_cast<unsigned long>(std::strlen(spec.sql))
        )) {
            error = mysql_stmt_error(*spec.target);
            return false;
        }
    }

    return true;
}

bool UserStore::ensure_schema(MysqlConnection* connection) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "username VARCHAR(64) NOT NULL UNIQUE,"
        "password_hash VARCHAR(64) NOT NULL,"
        "display_name VARCHAR(64) NOT NULL,"
        "email VARCHAR(128) NOT NULL DEFAULT '',"
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (mysql_query(connection->mysql, sql)) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "create users table failed: " +
            std::string(mysql_error(connection->mysql));
        return false;
    }

    const char* image_sql =
        "CREATE TABLE IF NOT EXISTS images ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "owner_username VARCHAR(64) NOT NULL,"
        "stored_name VARCHAR(255) NOT NULL UNIQUE,"
        "original_name VARCHAR(255) NOT NULL,"
        "content_hash CHAR(64) NOT NULL,"
        "size_bytes BIGINT UNSIGNED NOT NULL,"
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "UNIQUE KEY uq_images_owner_hash(owner_username, content_hash),"
        "KEY idx_images_owner(owner_username),"
        "CONSTRAINT fk_images_owner FOREIGN KEY(owner_username) "
        "REFERENCES users(username) ON DELETE CASCADE"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

    if (mysql_query(connection->mysql, image_sql)) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "create images table failed: " +
            std::string(mysql_error(connection->mysql));
        return false;
    }

    return true;
}

bool UserStore::is_connection_error(unsigned int error_no) const {
    return error_no == CR_CONNECTION_ERROR ||
        error_no == CR_CONN_HOST_ERROR ||
        error_no == CR_SERVER_GONE_ERROR ||
        error_no == CR_SERVER_LOST ||
        error_no == CR_SERVER_LOST_EXTENDED;
}

void UserStore::update_pool_status(const std::string& detail) {
    size_t healthy = healthy_connections_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(mutex_);
    available_ = healthy >= min_healthy_connections_;

    std::ostringstream oss;
    oss << "mysql pool healthy=" << healthy << "/" << pool_size_
        << ", required=" << min_healthy_connections_;

    if (!detail.empty()) {
        oss << ", " << detail;
    }

    status_message_ = oss.str();
}
#endif
