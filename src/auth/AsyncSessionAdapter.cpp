#include "auth/AsyncSessionManager.h"

#include "redis/AsyncRedisClient.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <sys/random.h>
#include <unordered_map>
#include <utility>

namespace {

int positive_env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

size_t positive_env_size(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    size_t parsed = static_cast<size_t>(std::strtoull(value, nullptr, 10));
    return parsed > 0 ? parsed : fallback;
}

int non_negative_env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    int parsed = std::atoi(value);
    return parsed >= 0 ? parsed : fallback;
}

std::string generate_token() {
    std::array<unsigned char, 32> bytes{};
    ssize_t generated = getrandom(bytes.data(), bytes.size(), 0);
    if (generated != static_cast<ssize_t>(bytes.size())) {
        std::random_device random;
        for (unsigned char& byte : bytes) {
            byte = static_cast<unsigned char>(random() & 0xff);
        }
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string token(bytes.size() * 2, '\0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        token[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
        token[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    return token;
}

std::string encode_session(
    const std::string& username,
    const std::string& display_name
) {
    return std::to_string(username.size()) + ":" + username + display_name;
}

bool decode_session(const std::string& payload, SessionInfo& session) {
    size_t separator = payload.find(':');
    if (separator == std::string::npos || separator == 0) {
        return false;
    }

    size_t username_size = 0;
    const char* begin = payload.data();
    const char* end = payload.data() + separator;
    auto parsed = std::from_chars(begin, end, username_size);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        username_size == 0 || username_size > payload.size() - separator - 1) {
        return false;
    }

    size_t username_start = separator + 1;
    session.username = payload.substr(username_start, username_size);
    session.display_name = payload.substr(username_start + username_size);
    return true;
}

}

struct AsyncSessionManager::Impl {
    struct CachedSession {
        SessionInfo session;
        std::chrono::steady_clock::time_point expires_at;
    };

    int ttl_seconds{ 86400 };
    int cache_ttl_seconds{ 3 };
    size_t cache_capacity{ 20000 };
    mutable std::shared_mutex cache_mutex;
    mutable std::unordered_map<std::string, CachedSession> cache;
    mutable uint64_t cache_epoch{ 0 };
    std::atomic<bool> configured{ false };

    std::string redis_key(const std::string& token) const {
        return AsyncRedisClient::instance().key_prefix() + "session:" + token;
    }

    bool get_cached(const std::string& token, SessionInfo& session) const {
        if (cache_ttl_seconds <= 0 || token.empty()) {
            return false;
        }

        auto now = std::chrono::steady_clock::now();
        {
            std::shared_lock<std::shared_mutex> lock(cache_mutex);
            auto it = cache.find(token);
            if (it == cache.end()) {
                return false;
            }
            if (now < it->second.expires_at) {
                session = it->second.session;
                return true;
            }
        }

        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        auto it = cache.find(token);
        if (it != cache.end() && now >= it->second.expires_at) {
            cache.erase(it);
        }
        return false;
    }

    void put_cached(const std::string& token, const SessionInfo& session) const {
        if (cache_ttl_seconds <= 0 || token.empty()) {
            return;
        }

        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        if (cache.size() >= cache_capacity && cache.find(token) == cache.end()) {
            auto now = std::chrono::steady_clock::now();
            for (auto it = cache.begin(); it != cache.end();) {
                if (now >= it->second.expires_at) {
                    it = cache.erase(it);
                }
                else {
                    ++it;
                }
            }
            if (cache.size() >= cache_capacity) {
                cache.erase(cache.begin());
            }
        }

        cache[token] = {
            session,
            std::chrono::steady_clock::now() +
                std::chrono::seconds(cache_ttl_seconds)
        };
    }

    uint64_t current_cache_epoch() const {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        return cache_epoch;
    }

    void put_cached_if_epoch(
        const std::string& token,
        const SessionInfo& session,
        uint64_t expected_epoch
    ) const {
        if (cache_ttl_seconds <= 0 || token.empty()) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        if (cache_epoch != expected_epoch) {
            return;
        }
        if (cache.size() >= cache_capacity && cache.find(token) == cache.end()) {
            auto now = std::chrono::steady_clock::now();
            for (auto it = cache.begin(); it != cache.end();) {
                if (now >= it->second.expires_at) {
                    it = cache.erase(it);
                }
                else {
                    ++it;
                }
            }
            if (cache.size() >= cache_capacity) {
                cache.erase(cache.begin());
            }
        }
        cache[token] = { session, std::chrono::steady_clock::now() +
            std::chrono::seconds(cache_ttl_seconds) };
    }

    void erase_cached(const std::string& token) const {
        if (token.empty()) {
            return;
        }
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        cache.erase(token);
    }

    void revoke_cached(const std::string& token) const {
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        ++cache_epoch;
        cache.erase(token);
    }

    void clear_cache() {
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        ++cache_epoch;
        cache.clear();
    }
};

AsyncSessionManager& AsyncSessionManager::instance() {
    static AsyncSessionManager manager;
    return manager;
}

AsyncSessionManager::AsyncSessionManager() : impl_(std::make_unique<Impl>()) {
}

AsyncSessionManager::~AsyncSessionManager() = default;

bool AsyncSessionManager::init_from_env() {
    if (!impl_->configured.exchange(true)) {
        impl_->ttl_seconds = positive_env_int("SESSION_TTL_SECONDS", 86400);
        impl_->cache_ttl_seconds = non_negative_env_int(
            "SESSION_CACHE_TTL_SECONDS",
            3
        );
        impl_->cache_capacity = positive_env_size(
            "SESSION_CACHE_CAPACITY",
            20000
        );
    }
    return AsyncRedisClient::instance().init_from_env();
}

void AsyncSessionManager::shutdown() {
    impl_->clear_cache();
}

bool AsyncSessionManager::available() const {
    return AsyncRedisClient::instance().available();
}

std::string AsyncSessionManager::status_message() const {
    return AsyncRedisClient::instance().status_message();
}

void AsyncSessionManager::create_session(
    const std::string& username,
    const std::string& display_name,
    CreateCallback callback
) {
    std::string token = generate_token();
    SessionInfo session{ username, display_name };
    AsyncRedisClient::instance().command(
        {
            "SETEX",
            impl_->redis_key(token),
            std::to_string(impl_->ttl_seconds),
            encode_session(username, display_name)
        },
        [this, token, session, callback = std::move(callback)](
            AsyncRedisResult result
        ) mutable {
            if (result.type != RedisResultType::Status || result.value != "OK") {
                callback(
                    "",
                    result.error.empty()
                        ? "unexpected Redis SETEX response"
                        : std::move(result.error)
                );
                return;
            }
            impl_->put_cached(token, session);
            callback(token, "");
        }
    );
}

void AsyncSessionManager::get_session(
    const std::string& token,
    GetCallback callback
) const {
    SessionInfo cached;
    if (impl_->get_cached(token, cached)) {
        callback(true, std::move(cached), "");
        return;
    }

    const uint64_t expected_cache_epoch = impl_->current_cache_epoch();
    AsyncRedisClient::instance().command(
        { "GET", impl_->redis_key(token) },
        [this, token, expected_cache_epoch, callback = std::move(callback)](
            AsyncRedisResult result
        ) mutable {
            if (result.type == RedisResultType::Error) {
                callback(false, {}, std::move(result.error));
                return;
            }
            if (result.type == RedisResultType::Nil) {
                impl_->erase_cached(token);
                callback(false, {}, "");
                return;
            }
            if (result.type != RedisResultType::String) {
                callback(false, {}, "unexpected Redis GET response");
                return;
            }

            SessionInfo session;
            if (!decode_session(result.value, session)) {
                callback(false, {}, "invalid session payload");
                return;
            }
            // A logout that began after this GET invalidates the result even
            // if Redis replied with the pre-DEL value.
            impl_->put_cached_if_epoch(
                token, session, expected_cache_epoch
            );
            callback(true, std::move(session), "");
        }
    );
}

void AsyncSessionManager::destroy_session(
    const std::string& token,
    DestroyCallback callback
) {
    impl_->revoke_cached(token);
    if (token.empty()) {
        callback(true, "");
        return;
    }

    AsyncRedisClient::instance().command(
        { "DEL", impl_->redis_key(token) },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type != RedisResultType::Integer) {
                callback(
                    false,
                    result.error.empty()
                        ? "unexpected Redis DEL response"
                        : std::move(result.error)
                );
                return;
            }
            callback(true, "");
        }
    );
}
