#pragma once

#include <cstddef>
#include <functional>
#include <string>

enum class RedisCacheState {
    Hit,
    Miss,
    Error
};

struct RedisCacheResult {
    RedisCacheState state{ RedisCacheState::Error };
    std::string value;
    std::string error;
    std::string version;
};

struct RedisWriteResult {
    bool ok{ false };
    std::string error;
    bool applied{ true };
};

struct RedisLimitResult {
    bool allowed{ false };
    std::string error;
};

enum class RedisIdempotencyState {
    Claimed,
    Processing,
    Completed,
    Conflict,
    Error
};

struct RedisIdempotencyResult {
    RedisIdempotencyState state{ RedisIdempotencyState::Error };
    std::string response_body;
    std::string error;
};

enum class RedisRankingType {
    Views,
    Downloads
};

struct RedisRankingResult {
    bool ok{ false };
    std::string json;
    std::string error;
};

class ImageRedisService {
public:
    using CacheCallback = std::function<void(RedisCacheResult)>;
    using WriteCallback = std::function<void(RedisWriteResult)>;
    using LimitCallback = std::function<void(RedisLimitResult)>;
    using IdempotencyCallback = std::function<void(RedisIdempotencyResult)>;
    using RankingCallback = std::function<void(RedisRankingResult)>;

    static ImageRedisService& instance();

    void get_image_list(CacheCallback callback) const;

    void put_image_list(
        const std::string& response_body,
        WriteCallback callback
    ) const;

    void put_image_list_if_version(
        const std::string& response_body,
        const std::string& expected_version,
        WriteCallback callback
    ) const;

    void invalidate_image_list(WriteCallback callback) const;

    void get_image_status(
        const std::string& saved_name,
        CacheCallback callback
    ) const;

    void put_image_status(
        const std::string& saved_name,
        const std::string& status,
        WriteCallback callback
    ) const;

    void put_image_status_for_generation(
        const std::string& saved_name,
        const std::string& status,
        const std::string& upload_token,
        WriteCallback callback
    ) const;

    void remove_image_status(
        const std::string& saved_name,
        WriteCallback callback
    ) const;

    void check_upload_rate(
        const std::string& username,
        LimitCallback callback
    ) const;

    void acquire_upload_permit(
        const std::string& username,
        const std::string& lease_token,
        LimitCallback callback
    ) const;

    void release_upload_permit(
        const std::string& username,
        const std::string& lease_token,
        WriteCallback callback
    ) const;

    void begin_upload(
        const std::string& username,
        const std::string& idempotency_key,
        const std::string& payload_hash,
        IdempotencyCallback callback
    ) const;

    void complete_upload(
        const std::string& username,
        const std::string& idempotency_key,
        const std::string& payload_hash,
        const std::string& response_body,
        WriteCallback callback
    ) const;

    void abort_upload(
        const std::string& username,
        const std::string& idempotency_key,
        const std::string& payload_hash,
        WriteCallback callback
    ) const;

    void increment_image_view(
        const std::string& saved_name,
        WriteCallback callback
    ) const;

    void increment_image_download(
        const std::string& saved_name,
        WriteCallback callback
    ) const;

    void get_image_ranking(
        RedisRankingType type,
        size_t limit,
        RankingCallback callback
    ) const;

    void remove_image_rankings(
        const std::string& saved_name,
        WriteCallback callback
    ) const;

    void begin_image_upload(
        const std::string& saved_name,
        const std::string& upload_token,
        WriteCallback callback
    ) const;

    void abort_image_upload(
        const std::string& saved_name,
        const std::string& upload_token,
        WriteCallback callback
    ) const;

    void activate_image(
        const std::string& saved_name,
        const std::string& status,
        const std::string& upload_token,
        WriteCallback callback
    ) const;

    void begin_image_delete(
        const std::string& saved_name,
        const std::string& delete_token,
        WriteCallback callback
    ) const;

    void cancel_image_delete(
        const std::string& saved_name,
        const std::string& delete_token,
        WriteCallback callback
    ) const;

    void finalize_image_delete(
        const std::string& saved_name,
        const std::string& delete_token,
        WriteCallback callback
    ) const;

    void recover_image_after_delete(
        const std::string& saved_name,
        WriteCallback callback
    ) const;

    void force_finalize_image_delete(
        const std::string& saved_name,
        WriteCallback callback
    ) const;

private:
    ImageRedisService();

    std::string list_key() const;
    std::string list_version_key() const;
    std::string status_key(const std::string& saved_name) const;
    std::string rate_key(const std::string& username) const;
    std::string permit_key(const std::string& username) const;
    std::string idempotency_key(
        const std::string& username,
        const std::string& key
    ) const;
    std::string ranking_key(RedisRankingType type) const;
    std::string lifecycle_prefix() const;
    std::string lifecycle_key(const std::string& saved_name) const;

    int image_list_ttl_seconds_{ 3 };
    int image_status_ttl_seconds_{ 30 };
    int upload_rate_limit_per_minute_{ 20 };
    int max_concurrent_uploads_per_user_{ 2 };
    int upload_permit_ttl_seconds_{ 30 };
    int upload_idempotency_ttl_seconds_{ 86400 };
    int image_lifecycle_pending_ttl_seconds_{ 120 };
};
