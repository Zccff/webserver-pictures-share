#include "auth/SessionManager.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

std::string env_or_default(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value && value[0] != '\0' ? value : fallback;
}

int env_int_or_default(const char* key, int fallback) {
    const char* value = std::getenv(key);

    if (!value || value[0] == '\0') {
        return fallback;
    }

    return std::atoi(value);
}

}

SessionManager& SessionManager::instance() {
    static SessionManager manager;
    return manager;
}

bool SessionManager::init_from_env() {
    int configured_pool_size = env_int_or_default("REDIS_POOL_SIZE", 8);
    int configured_cache_ttl = env_int_or_default("SESSION_CACHE_TTL_SECONDS", 3);
    int configured_cache_capacity = env_int_or_default("SESSION_CACHE_CAPACITY", 20000);
    int configured_io_timeout = env_int_or_default("REDIS_IO_TIMEOUT_MS", 3000);

    if (configured_pool_size <= 0) {
        configured_pool_size = 8;
    }

    if (configured_cache_ttl < 0) {
        configured_cache_ttl = 0;
    }

    if (configured_cache_capacity <= 0) {
        configured_cache_capacity = 20000;
    }

    if (configured_io_timeout <= 0) {
        configured_io_timeout = 3000;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        host_ = env_or_default("REDIS_HOST", "127.0.0.1");
        port_ = env_int_or_default("REDIS_PORT", 6379);
        password_ = env_or_default("REDIS_PASSWORD", "");
        ttl_seconds_ = env_int_or_default("SESSION_TTL_SECONDS", 86400);

        if (ttl_seconds_ <= 0) {
            ttl_seconds_ = 86400;
        }

        pool_size_ = configured_pool_size;
        cache_ttl_seconds_ = configured_cache_ttl;
        cache_capacity_ = static_cast<size_t>(configured_cache_capacity);
        io_timeout_ms_ = configured_io_timeout;
        available_ = false;
        status_message_ = "redis session store is connecting";
        connections_.clear();
        connections_.reserve(static_cast<size_t>(pool_size_));

        for (int i = 0; i < pool_size_; ++i) {
            connections_.push_back(std::make_unique<RedisConnection>());
        }
    }

    int connected = 0;

    for (const auto& connection : connections_) {
        std::lock_guard<std::mutex> lock(connection->mutex);

        if (connect_locked(*connection)) {
            ++connected;
        }
    }

    if (connected == 0) {
        set_status(false, "redis connect failed for all pool connections");
        return false;
    }

    std::ostringstream oss;
    oss << "redis connected, pool=" << connected << "/" << pool_size_
        << ", local_cache_ttl=" << cache_ttl_seconds_ << "s";

    set_status(true, oss.str());
    return true;
}

bool SessionManager::available() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return available_;
}

std::string SessionManager::status_message() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_message_;
}

std::string SessionManager::create_session(
    const std::string& username,
    const std::string& display_name
) {
    std::string token = generate_token();
    SessionInfo session{ username, display_name };
    std::string response;

    if (!command_with_retry(
        {
            "SETEX",
            "session:" + token,
            std::to_string(ttl_seconds_),
            session_payload(username, display_name)
        },
        response
    )) {
        return "";
    }

    if (response.rfind("+OK", 0) != 0) {
        set_status(false, "redis SETEX failed: " + response);
        return "";
    }

    put_cached_session(token, session);
    return token;
}

bool SessionManager::get_session(const std::string& token, SessionInfo& session) const {
    if (get_cached_session(token, session)) {
        return true;
    }

    std::string response;

    if (!command_with_retry({ "GET", "session:" + token }, response)) {
        return false;
    }

    if (response == "$-1\r\n") {
        erase_cached_session(token);
        return false;
    }

    if (response.empty() || response[0] != '$') {
        set_status(false, "unexpected redis GET response: " + response);
        return false;
    }

    size_t line_end = response.find("\r\n");

    if (line_end == std::string::npos) {
        return false;
    }

    std::string payload = response.substr(line_end + 2);

    if (payload.size() >= 2 && payload.compare(payload.size() - 2, 2, "\r\n") == 0) {
        payload.resize(payload.size() - 2);
    }

    if (!parse_session_payload(payload, session)) {
        return false;
    }

    put_cached_session(token, session);
    return true;
}

void SessionManager::destroy_session(const std::string& token) {
    erase_cached_session(token);

    std::string response;
    command_with_retry({ "DEL", "session:" + token }, response);
}

std::string SessionManager::generate_token() {
    std::array<unsigned char, 32> bytes{};
    ssize_t generated = getrandom(bytes.data(), bytes.size(), 0);

    if (generated != static_cast<ssize_t>(bytes.size())) {
        std::random_device random;

        for (unsigned char& byte : bytes) {
            byte = static_cast<unsigned char>(random() & 0xff);
        }
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.resize(bytes.size() * 2);

    for (size_t i = 0; i < bytes.size(); ++i) {
        token[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
        token[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }

    return token;
}

bool SessionManager::command_with_retry(
    const std::initializer_list<std::string>& args,
    std::string& response
) const {
    if (connections_.empty()) {
        set_status(false, "redis connection pool is empty");
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        size_t start = next_connection_.fetch_add(1, std::memory_order_relaxed);
        bool tried_connection = false;

        for (size_t i = 0; i < connections_.size(); ++i) {
            RedisConnection& connection =
                *connections_[(start + i) % connections_.size()];
            std::unique_lock<std::mutex> lock(connection.mutex, std::try_to_lock);

            if (!lock.owns_lock()) {
                continue;
            }

            tried_connection = true;

            if (connection.fd < 0 && !connect_locked(connection)) {
                continue;
            }

            if (command_locked(connection, args, response)) {
                set_status(true, "redis connected");
                return true;
            }

            close_connection_locked(connection);
        }

        if (tried_connection) {
            continue;
        }

        // Every slot was busy. Wait on one rotating slot instead of queuing
        // behind the originally selected connection while another slot frees.
        RedisConnection& connection = *connections_[start % connections_.size()];
        std::unique_lock<std::mutex> lock(connection.mutex);

        if (connection.fd < 0 && !connect_locked(connection)) {
            continue;
        }

        if (command_locked(connection, args, response)) {
            set_status(true, "redis connected");
            return true;
        }

        close_connection_locked(connection);
    }

    return false;
}

bool SessionManager::connect_locked(RedisConnection& connection) const {
    close_connection_locked(connection);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    std::string port = std::to_string(port_);

    int gai_status = getaddrinfo(host_.c_str(), port.c_str(), &hints, &result);

    if (gai_status != 0) {
        set_status(false, "redis resolve failed: " + std::string(gai_strerror(gai_status)));
        return false;
    }

    int connected_fd = -1;

    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        connected_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (connected_fd < 0) {
            continue;
        }

        int old_flags = fcntl(connected_fd, F_GETFL, 0);

        if (old_flags >= 0) {
            fcntl(connected_fd, F_SETFL, old_flags | O_NONBLOCK);
        }

        int connect_result = connect(connected_fd, rp->ai_addr, rp->ai_addrlen);
        bool connected = connect_result == 0;

        if (!connected && errno == EINPROGRESS) {
            pollfd descriptor{};
            descriptor.fd = connected_fd;
            descriptor.events = POLLOUT;
            int poll_result;

            do {
                poll_result = poll(&descriptor, 1, io_timeout_ms_);
            } while (poll_result < 0 && errno == EINTR);

            if (poll_result > 0) {
                int socket_error = 0;
                socklen_t error_length = sizeof(socket_error);
                connected = getsockopt(
                    connected_fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &socket_error,
                    &error_length
                ) == 0 && socket_error == 0;
            }
        }

        if (old_flags >= 0) {
            fcntl(connected_fd, F_SETFL, old_flags);
        }

        if (connected) {
            break;
        }

        close(connected_fd);
        connected_fd = -1;
    }

    freeaddrinfo(result);

    if (connected_fd < 0) {
        set_status(false, "redis connect failed: " + std::string(std::strerror(errno)));
        return false;
    }

    connection.fd = connected_fd;

    timeval timeout{};
    timeout.tv_sec = io_timeout_ms_ / 1000;
    timeout.tv_usec = (io_timeout_ms_ % 1000) * 1000;
    setsockopt(connection.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (!password_.empty()) {
        std::string response;

        if (!command_locked(connection, { "AUTH", password_ }, response) ||
            response.rfind("+OK", 0) != 0) {
            set_status(false, "redis auth failed: " + response);
            close_connection_locked(connection);
            return false;
        }
    }

    std::string response;

    if (!command_locked(connection, { "PING" }, response) ||
        response.rfind("+PONG", 0) != 0) {
        set_status(false, "redis ping failed: " + response);
        close_connection_locked(connection);
        return false;
    }

    return true;
}

bool SessionManager::send_all_locked(
    RedisConnection& connection,
    const std::string& data
) const {
    size_t sent = 0;

    while (sent < data.size()) {
        ssize_t n = send(
            connection.fd,
            data.data() + sent,
            data.size() - sent,
            0
        );

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            set_status(false, "redis send failed: " + std::string(std::strerror(errno)));
            return false;
        }

        if (n == 0) {
            set_status(false, "redis send returned zero");
            return false;
        }

        sent += static_cast<size_t>(n);
    }

    return true;
}

bool SessionManager::read_line_locked(
    RedisConnection& connection,
    std::string& line
) const {
    line.clear();
    char ch = '\0';

    while (true) {
        ssize_t n = recv(connection.fd, &ch, 1, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            set_status(false, "redis read failed: " + std::string(std::strerror(errno)));
            return false;
        }

        if (n == 0) {
            set_status(false, "redis connection closed");
            return false;
        }

        line.push_back(ch);

        if (line.size() >= 2 &&
            line[line.size() - 2] == '\r' &&
            line[line.size() - 1] == '\n') {
            return true;
        }
    }
}

bool SessionManager::read_exact_locked(
    RedisConnection& connection,
    size_t size,
    std::string& value
) const {
    value.clear();
    value.resize(size);
    size_t read_size = 0;

    while (read_size < size) {
        ssize_t n = recv(connection.fd, &value[read_size], size - read_size, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            set_status(false, "redis bulk read failed: " + std::string(std::strerror(errno)));
            return false;
        }

        if (n == 0) {
            set_status(false, "redis bulk read connection closed");
            return false;
        }

        read_size += static_cast<size_t>(n);
    }

    return true;
}

bool SessionManager::command_locked(
    RedisConnection& connection,
    const std::initializer_list<std::string>& args,
    std::string& response
) const {
    response.clear();

    if (connection.fd < 0) {
        set_status(false, "redis socket is not connected");
        return false;
    }

    if (!send_all_locked(connection, build_resp_command(args))) {
        return false;
    }

    std::string line;

    if (!read_line_locked(connection, line)) {
        return false;
    }

    response += line;

    if (!line.empty() && line[0] == '$') {
        long bulk_size = std::strtol(line.c_str() + 1, nullptr, 10);

        if (bulk_size < 0) {
            return true;
        }

        std::string bulk;

        if (!read_exact_locked(
            connection,
            static_cast<size_t>(bulk_size) + 2,
            bulk
        )) {
            return false;
        }

        response += bulk;
    }

    return true;
}

void SessionManager::close_connection_locked(RedisConnection& connection) const {
    if (connection.fd >= 0) {
        close(connection.fd);
        connection.fd = -1;
    }
}

void SessionManager::set_status(bool available, const std::string& message) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    available_ = available;
    status_message_ = message;
}

bool SessionManager::get_cached_session(
    const std::string& token,
    SessionInfo& session
) const {
    if (cache_ttl_seconds_ <= 0) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();

    {
        std::shared_lock<std::shared_mutex> lock(cache_mutex_);
        auto it = session_cache_.find(token);

        if (it == session_cache_.end()) {
            return false;
        }

        if (now < it->second.expires_at) {
            session = it->second.session;
            return true;
        }
    }

    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    auto it = session_cache_.find(token);

    if (it == session_cache_.end()) {
        return false;
    }

    if (std::chrono::steady_clock::now() < it->second.expires_at) {
        session = it->second.session;
        return true;
    }

    session_cache_.erase(it);
    return false;
}

void SessionManager::put_cached_session(
    const std::string& token,
    const SessionInfo& session
) const {
    if (cache_ttl_seconds_ <= 0 || token.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(cache_mutex_);

    if (session_cache_.size() >= cache_capacity_ &&
        session_cache_.find(token) == session_cache_.end()) {
        auto now = std::chrono::steady_clock::now();

        for (auto it = session_cache_.begin(); it != session_cache_.end();) {
            if (now >= it->second.expires_at) {
                it = session_cache_.erase(it);
            }
            else {
                ++it;
            }
        }

        if (session_cache_.size() >= cache_capacity_) {
            session_cache_.erase(session_cache_.begin());
        }
    }

    session_cache_[token] = {
        session,
        std::chrono::steady_clock::now() +
            std::chrono::seconds(cache_ttl_seconds_)
    };
}

void SessionManager::erase_cached_session(const std::string& token) const {
    if (token.empty()) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(cache_mutex_);
    session_cache_.erase(token);
}

std::string SessionManager::build_resp_command(
    const std::initializer_list<std::string>& args
) {
    std::ostringstream oss;
    oss << "*" << args.size() << "\r\n";

    for (const std::string& arg : args) {
        oss << "$" << arg.size() << "\r\n" << arg << "\r\n";
    }

    return oss.str();
}

bool SessionManager::parse_session_payload(
    const std::string& payload,
    SessionInfo& session
) {
    size_t sep = payload.find('\n');

    if (sep == std::string::npos) {
        return false;
    }

    session.username = payload.substr(0, sep);
    session.display_name = payload.substr(sep + 1);
    return !session.username.empty();
}

std::string SessionManager::session_payload(
    const std::string& username,
    const std::string& display_name
) {
    return username + "\n" + display_name;
}
