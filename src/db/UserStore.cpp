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
    std::atomic<bool> in_use{ false };
    std::atomic<bool> healthy{ false };
};
#endif

UserStore& UserStore::instance() {
    static UserStore store;
    return store;
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
    const std::string& email
) {
    if (username.empty() || password.empty()) {
        return { false, "username and password are required" };
    }

#ifdef MYWEBSERVER_WITH_MYSQL
    MysqlConnection* connection = acquire_connection();

    if (!connection) {
        return { false, status_message() };
    }

    MYSQL_STMT* stmt = connection->register_stmt;

    if (mysql_stmt_reset(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error };
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
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error };
    }

    if (mysql_stmt_execute(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);

        if (error_no == 1062) {
            return { false, "username already exists" };
        }

        return { false, error };
    }

    release_connection(connection);
    put_cached_login_user(username, password_hash, safe_display_name);
    return { true, "registered" };
#else
    return { false, status_message() };
#endif
}

LoginResult UserStore::login_user(
    const std::string& username,
    const std::string& password
) {
    if (username.empty() || password.empty()) {
        return { false, "username and password are required", "", "" };
    }

#ifdef MYWEBSERVER_WITH_MYSQL
    std::string expected_hash = password_hash_for(password);
    CachedLoginUser cached_user;

    if (get_cached_login_user(username, cached_user)) {
        if (!constant_time_equal(cached_user.password_hash, expected_hash)) {
            return { false, "invalid username or password", "", "" };
        }

        return { true, "logged in", username, cached_user.display_name };
    }

    MysqlConnection* connection = acquire_connection();

    if (!connection) {
        return { false, status_message(), "", "" };
    }

    MYSQL_STMT* stmt = connection->login_stmt;

    if (mysql_stmt_reset(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "" };
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
        return { false, error, "", "" };
    }

    if (mysql_stmt_execute(stmt)) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "" };
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
        return { false, error, "", "" };
    }

    int fetch_status = mysql_stmt_fetch(stmt);

    if (fetch_status == MYSQL_NO_DATA) {
        mysql_stmt_free_result(stmt);
        release_connection(connection);
        return { false, "invalid username or password", "", "" };
    }

    if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED) {
        unsigned int error_no = mysql_stmt_errno(stmt);
        std::string error = mysql_stmt_error(stmt);
        mysql_stmt_free_result(stmt);
        handle_connection_failure(connection, error_no, error);
        release_connection(connection);
        return { false, error, "", "" };
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
        return { false, "invalid username or password", "", "" };
    }

    return { true, "logged in", result_username, result_display_name };
#else
    return { false, status_message(), "", "" };
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
    pool_cv_.wait(lock, [this]() {
        return !pool_available_.load(std::memory_order_acquire) ||
            has_idle_connection();
    });

    if (!pool_available_.load(std::memory_order_acquire)) {
        return nullptr;
    }

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

    replacement->mysql = nullptr;
    replacement->register_stmt = nullptr;
    replacement->login_stmt = nullptr;
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
