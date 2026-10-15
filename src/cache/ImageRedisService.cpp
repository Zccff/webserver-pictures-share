#include "cache/ImageRedisService.h"

#include "redis/AsyncRedisClient.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

int positive_env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    int parsed = std::atoi(value);
    return parsed > 0 ? parsed : fallback;
}

RedisWriteResult write_result(
    AsyncRedisResult result,
    RedisResultType expected,
    const char* unexpected
) {
    if (result.type == expected) {
        return { true, "" };
    }
    return {
        false,
        result.error.empty() ? unexpected : std::move(result.error)
    };
}

RedisWriteResult integer_mutation_result(
    AsyncRedisResult result,
    const char* unexpected
) {
    if (result.type == RedisResultType::Integer) {
        return { true, "", result.integer == 1 };
    }
    return {
        false,
        result.error.empty() ? unexpected : std::move(result.error),
        false
    };
}

void read_cache(
    std::vector<std::string> command,
    ImageRedisService::CacheCallback callback
) {
    AsyncRedisClient::instance().command(
        std::move(command),
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::String) {
                callback({ RedisCacheState::Hit, std::move(result.value), "" });
                return;
            }
            if (result.type == RedisResultType::Nil) {
                callback({ RedisCacheState::Miss, "", "" });
                return;
            }
            callback({
                RedisCacheState::Error,
                "",
                result.error.empty()
                    ? "unexpected Redis cache GET response"
                    : std::move(result.error)
            });
        }
    );
}

void write_status_command(
    std::vector<std::string> command,
    ImageRedisService::WriteCallback callback
) {
    AsyncRedisClient::instance().command(
        std::move(command),
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(write_result(
                std::move(result),
                RedisResultType::Status,
                "unexpected Redis status response"
            ));
        }
    );
}

void delete_command(
    std::vector<std::string> command,
    ImageRedisService::WriteCallback callback
) {
    AsyncRedisClient::instance().command(
        std::move(command),
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(write_result(
                std::move(result),
                RedisResultType::Integer,
                "unexpected Redis delete response"
            ));
        }
    );
}

const char* RATE_LIMIT_SCRIPT = R"lua(
local current = redis.call('INCR', KEYS[1])
if current == 1 then
    redis.call('EXPIRE', KEYS[1], ARGV[1])
end
if current > tonumber(ARGV[2]) then
    return 0
end
return 1
)lua";

const char* ACQUIRE_PERMIT_SCRIPT = R"lua(
local redis_time = redis.call('TIME')
local now_ms = tonumber(redis_time[1]) * 1000 +
    math.floor(tonumber(redis_time[2]) / 1000)
redis.call('ZREMRANGEBYSCORE', KEYS[1], '-inf', now_ms)
if redis.call('ZCARD', KEYS[1]) >= tonumber(ARGV[1]) then
    return 0
end
local expires_at = now_ms + tonumber(ARGV[2]) * 1000
local inserted = redis.call('ZADD', KEYS[1], 'NX', expires_at, ARGV[3])
redis.call('EXPIRE', KEYS[1], tonumber(ARGV[2]) + 1)
return inserted
)lua";

const char* RELEASE_PERMIT_SCRIPT = R"lua(
local removed = redis.call('ZREM', KEYS[1], ARGV[1])
if redis.call('ZCARD', KEYS[1]) == 0 then
    redis.call('DEL', KEYS[1])
end
return removed
)lua";

const char* BEGIN_IDEMPOTENCY_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
local processing = 'PROCESSING\n' .. ARGV[2]
if current then
    if current == processing then
        return 'PROCESSING'
    end
    local done_prefix = 'DONE\n' .. ARGV[2] .. '\n'
    if string.sub(current, 1, string.len(done_prefix)) == done_prefix then
        return current
    end
    return 'CONFLICT'
end
redis.call('SETEX', KEYS[1], ARGV[1], processing)
return 'CLAIMED'
)lua";

const char* ABORT_IDEMPOTENCY_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
if current == 'PROCESSING\n' .. ARGV[1] then
    return redis.call('DEL', KEYS[1])
end
return 0
)lua";

const char* COMPLETE_IDEMPOTENCY_SCRIPT = R"lua(
if redis.call('GET', KEYS[1]) ~= 'PROCESSING\n' .. ARGV[2] then
    return 0
end
redis.call('SETEX', KEYS[1], ARGV[1], 'DONE\n' .. ARGV[2] .. '\n' .. ARGV[3])
return 1
)lua";

const char* PUT_IMAGE_STATUS_SCRIPT = R"lua(
local lifecycle = redis.call('GET', KEYS[2])
if lifecycle and string.sub(lifecycle, 1, 7) ~= 'ACTIVE\n' then
    return 0
end
redis.call('SETEX', KEYS[1], ARGV[1], ARGV[2])
return 1
)lua";

const char* PUT_IMAGE_STATUS_FOR_GENERATION_SCRIPT = R"lua(
local lifecycle = redis.call('GET', KEYS[2])
if lifecycle ~= 'ACTIVE\n' .. ARGV[3] then
    return 0
end
redis.call('SETEX', KEYS[1], ARGV[1], ARGV[2])
return 1
)lua";

const char* GET_IMAGE_STATUS_SCRIPT = R"lua(
local lifecycle = redis.call('GET', KEYS[2])
if lifecycle and string.sub(lifecycle, 1, 7) ~= 'ACTIVE\n' then
    return nil
end
return redis.call('GET', KEYS[1])
)lua";

const char* GET_IMAGE_LIST_SCRIPT = R"lua(
local version = redis.call('GET', KEYS[2]) or '0'
local body = redis.call('GET', KEYS[1])
if body then
    return 'HIT\n' .. version .. '\n' .. body
end
return 'MISS\n' .. version
)lua";

const char* PUT_IMAGE_LIST_IF_VERSION_SCRIPT = R"lua(
local version = redis.call('GET', KEYS[2]) or '0'
if version ~= ARGV[1] then
    return 0
end
redis.call('SETEX', KEYS[1], ARGV[2], ARGV[3])
return 1
)lua";

const char* INVALIDATE_IMAGE_LIST_SCRIPT = R"lua(
redis.call('INCR', KEYS[2])
redis.call('DEL', KEYS[1])
return 1
)lua";

const char* BEGIN_IMAGE_UPLOAD_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
local own_prefix = 'UPLOADING\n' .. ARGV[2] .. '\n'
if current and string.sub(current, 1, string.len(own_prefix)) == own_prefix then
    return 1
end
if current and string.sub(current, 1, 8) ~= 'DELETED\n' then
    return 0
end
local previous = current or ''
redis.call('SETEX', KEYS[1], ARGV[1],
    'UPLOADING\n' .. ARGV[2] .. '\n' .. previous)
return 1
)lua";

const char* ABORT_IMAGE_UPLOAD_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
local prefix = 'UPLOADING\n' .. ARGV[1] .. '\n'
if not current or string.sub(current, 1, string.len(prefix)) ~= prefix then
    return 0
end
local previous = string.sub(current, string.len(prefix) + 1)
if previous == '' then
    redis.call('DEL', KEYS[1])
else
    redis.call('SETEX', KEYS[1], ARGV[2], previous)
end
return 1
)lua";

const char* ACTIVATE_IMAGE_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
if current == 'ACTIVE\n' .. ARGV[3] then
    return 1
end
local prefix = 'UPLOADING\n' .. ARGV[3] .. '\n'
if not current or string.sub(current, 1, string.len(prefix)) ~= prefix then
    return 0
end
redis.call('SET', KEYS[1], 'ACTIVE\n' .. ARGV[3])
redis.call('SETEX', KEYS[2], ARGV[1], ARGV[2])
redis.call('DEL', KEYS[3])
redis.call('INCR', KEYS[4])
return 1
)lua";

const char* BEGIN_IMAGE_DELETE_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
local own_prefix = 'DELETING\n' .. ARGV[2] .. '\n'
if current and string.sub(current, 1, string.len(own_prefix)) == own_prefix then
    return 1
end
if current and (
    string.sub(current, 1, 9) == 'DELETING\n' or
    string.sub(current, 1, 8) == 'DELETED\n'
) then
    return 0
end
local previous = current or ''
redis.call('SET', KEYS[1],
    'DELETING\n' .. ARGV[2] .. '\n' .. previous)
redis.call('DEL', KEYS[2])
redis.call('INCR', KEYS[3])
return 1
)lua";

const char* CANCEL_IMAGE_DELETE_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
local prefix = 'DELETING\n' .. ARGV[1] .. '\n'
if not current or string.sub(current, 1, 9) ~= 'DELETING\n' then
    return 1
end
if string.sub(current, 1, string.len(prefix)) ~= prefix then
    return 0
end
local previous = string.sub(current, string.len(prefix) + 1)
if previous == '' then
    redis.call('DEL', KEYS[1])
elseif string.sub(previous, 1, 10) == 'UPLOADING\n' then
    redis.call('SETEX', KEYS[1], ARGV[2], previous)
else
    redis.call('SET', KEYS[1], previous)
end
redis.call('DEL', KEYS[2])
redis.call('INCR', KEYS[3])
return 1
)lua";

const char* RECOVER_IMAGE_AFTER_DELETE_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[1])
if not current or string.sub(current, 1, 9) ~= 'DELETING\n' then
    return 0
end
local token_end = string.find(current, '\n', 10, true)
if not token_end then
    return 0
end
local previous = string.sub(current, token_end + 1)
if previous == '' then
    redis.call('DEL', KEYS[1])
elseif string.sub(previous, 1, 10) == 'UPLOADING\n' then
    redis.call('SETEX', KEYS[1], ARGV[1], previous)
else
    redis.call('SET', KEYS[1], previous)
end
redis.call('DEL', KEYS[2])
redis.call('INCR', KEYS[3])
return 1
)lua";

}

ImageRedisService& ImageRedisService::instance() {
    static ImageRedisService service;
    return service;
}

ImageRedisService::ImageRedisService()
    : image_list_ttl_seconds_(positive_env_int(
        "IMAGE_LIST_CACHE_TTL_SECONDS",
        3
    )),
      image_status_ttl_seconds_(positive_env_int(
        "IMAGE_STATUS_CACHE_TTL_SECONDS",
        30
    )),
      upload_rate_limit_per_minute_(positive_env_int(
        "UPLOAD_RATE_LIMIT_PER_MINUTE",
        20
    )),
      max_concurrent_uploads_per_user_(positive_env_int(
        "MAX_CONCURRENT_UPLOADS_PER_USER",
        2
    )),
      upload_permit_ttl_seconds_(positive_env_int(
        "UPLOAD_PERMIT_TTL_SECONDS",
        30
    )),
      upload_idempotency_ttl_seconds_(positive_env_int(
        "UPLOAD_IDEMPOTENCY_TTL_SECONDS",
        86400
    )),
      image_lifecycle_pending_ttl_seconds_(positive_env_int(
        "IMAGE_LIFECYCLE_PENDING_TTL_SECONDS",
        120
    )) {
    const int business_timeout_ms = positive_env_int(
        "BUSINESS_TIMEOUT_MS", 15000);
    const int minimum_lease_seconds = business_timeout_ms / 1000 + 5;
    if (upload_permit_ttl_seconds_ < minimum_lease_seconds) {
        upload_permit_ttl_seconds_ = minimum_lease_seconds;
    }
}

std::string ImageRedisService::list_key() const {
    return AsyncRedisClient::instance().key_prefix() + "image:list:all";
}

std::string ImageRedisService::list_version_key() const {
    return AsyncRedisClient::instance().key_prefix() + "image:list:version";
}

std::string ImageRedisService::status_key(const std::string& saved_name) const {
    return AsyncRedisClient::instance().key_prefix() +
        "image:status:" + saved_name;
}

std::string ImageRedisService::rate_key(const std::string& username) const {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return AsyncRedisClient::instance().key_prefix() +
        "upload:rate:" + username + ":" + std::to_string(seconds / 60);
}

std::string ImageRedisService::permit_key(const std::string& username) const {
    return AsyncRedisClient::instance().key_prefix() +
        "upload:active:" + username;
}

std::string ImageRedisService::idempotency_key(
    const std::string& username,
    const std::string& key
) const {
    return AsyncRedisClient::instance().key_prefix() +
        "upload:idem:" + username + ":" + key;
}

std::string ImageRedisService::lifecycle_prefix() const {
    return AsyncRedisClient::instance().key_prefix() + "image:lifecycle:";
}

std::string ImageRedisService::lifecycle_key(
    const std::string& saved_name
) const {
    return lifecycle_prefix() + saved_name;
}

void ImageRedisService::get_image_list(CacheCallback callback) const {
    AsyncRedisClient::instance().command(
        { "EVAL", GET_IMAGE_LIST_SCRIPT, "2", list_key(), list_version_key() },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type != RedisResultType::String) {
                callback({
                    RedisCacheState::Error,
                    "",
                    result.error.empty()
                        ? "unexpected Redis image-list response"
                        : std::move(result.error),
                    ""
                });
                return;
            }
            if (result.value.rfind("MISS\n", 0) == 0) {
                callback({
                    RedisCacheState::Miss,
                    "",
                    "",
                    result.value.substr(5)
                });
                return;
            }
            if (result.value.rfind("HIT\n", 0) == 0) {
                const size_t separator = result.value.find('\n', 4);
                if (separator != std::string::npos) {
                    callback({
                        RedisCacheState::Hit,
                        result.value.substr(separator + 1),
                        "",
                        result.value.substr(4, separator - 4)
                    });
                    return;
                }
            }
            callback({
                RedisCacheState::Error,
                "",
                "invalid Redis image-list envelope",
                ""
            });
        }
    );
}

void ImageRedisService::put_image_list(
    const std::string& response_body,
    WriteCallback callback
) const {
    get_image_list([
        this,
        response_body,
        callback = std::move(callback)
    ](RedisCacheResult result) mutable {
        if (result.state == RedisCacheState::Error) {
            callback({ false, std::move(result.error) });
            return;
        }
        put_image_list_if_version(
            response_body, result.version, std::move(callback));
    });
}

void ImageRedisService::put_image_list_if_version(
    const std::string& response_body,
    const std::string& expected_version,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            PUT_IMAGE_LIST_IF_VERSION_SCRIPT,
            "2",
            list_key(),
            list_version_key(),
            expected_version,
            std::to_string(image_list_ttl_seconds_),
            response_body
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer) {
                callback({ true, "", result.integer == 1 });
                return;
            }
            callback({
                false,
                result.error.empty()
                    ? "unexpected Redis image-list write response"
                    : std::move(result.error)
            });
        }
    );
}

void ImageRedisService::invalidate_image_list(WriteCallback callback) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            INVALIDATE_IMAGE_LIST_SCRIPT,
            "2",
            list_key(),
            list_version_key()
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(write_result(
                std::move(result),
                RedisResultType::Integer,
                "unexpected Redis image-list invalidation response"
            ));
        }
    );
}

void ImageRedisService::get_image_status(
    const std::string& saved_name,
    CacheCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL", GET_IMAGE_STATUS_SCRIPT, "2",
            status_key(saved_name), lifecycle_key(saved_name)
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::String) {
                callback({ RedisCacheState::Hit, std::move(result.value), "" });
            } else if (result.type == RedisResultType::Nil) {
                callback({ RedisCacheState::Miss, "", "" });
            } else {
                callback({ RedisCacheState::Error, "", result.error.empty()
                    ? "unexpected Redis image-status response"
                    : std::move(result.error) });
            }
        }
    );
}

void ImageRedisService::put_image_status(
    const std::string& saved_name,
    const std::string& status,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            PUT_IMAGE_STATUS_SCRIPT,
            "2",
            status_key(saved_name),
            lifecycle_key(saved_name),
            std::to_string(image_status_ttl_seconds_),
            status
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result), "unexpected Redis image-status response"));
        }
    );
}

void ImageRedisService::put_image_status_for_generation(
    const std::string& saved_name,
    const std::string& status,
    const std::string& upload_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL", PUT_IMAGE_STATUS_FOR_GENERATION_SCRIPT, "2",
            status_key(saved_name), lifecycle_key(saved_name),
            std::to_string(image_status_ttl_seconds_), status, upload_token
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result), "unexpected Redis generation status response"));
        }
    );
}

void ImageRedisService::activate_image(
    const std::string& saved_name,
    const std::string& status,
    const std::string& upload_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            ACTIVATE_IMAGE_SCRIPT,
            "4",
            lifecycle_key(saved_name),
            status_key(saved_name),
            list_key(),
            list_version_key(),
            std::to_string(image_status_ttl_seconds_),
            status,
            upload_token
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result),
                "unexpected Redis image activation response"
            ));
        }
    );
}

void ImageRedisService::begin_image_upload(
    const std::string& saved_name,
    const std::string& upload_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL", BEGIN_IMAGE_UPLOAD_SCRIPT, "1", lifecycle_key(saved_name),
            std::to_string(image_lifecycle_pending_ttl_seconds_), upload_token
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result), "unexpected Redis upload lifecycle response"));
        }
    );
}

void ImageRedisService::abort_image_upload(
    const std::string& saved_name,
    const std::string& upload_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL", ABORT_IMAGE_UPLOAD_SCRIPT, "1", lifecycle_key(saved_name),
            upload_token, "604800"
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result), "unexpected Redis upload abort response"));
        }
    );
}

void ImageRedisService::begin_image_delete(
    const std::string& saved_name,
    const std::string& delete_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL", BEGIN_IMAGE_DELETE_SCRIPT, "3",
            lifecycle_key(saved_name), list_key(), list_version_key(),
            "604800", delete_token
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result), "unexpected Redis delete lifecycle response"));
        }
    );
}

void ImageRedisService::cancel_image_delete(
    const std::string& saved_name,
    const std::string& delete_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            CANCEL_IMAGE_DELETE_SCRIPT,
            "3",
            lifecycle_key(saved_name),
            list_key(),
            list_version_key(),
            delete_token,
            std::to_string(image_lifecycle_pending_ttl_seconds_)
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result),
                "unexpected Redis delete cancellation response"
            ));
        }
    );
}

void ImageRedisService::recover_image_after_delete(
    const std::string& saved_name,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL", RECOVER_IMAGE_AFTER_DELETE_SCRIPT, "3",
            lifecycle_key(saved_name), list_key(), list_version_key(),
            std::to_string(image_lifecycle_pending_ttl_seconds_)
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(integer_mutation_result(
                std::move(result), "unexpected Redis delete recovery response"));
        }
    );
}

void ImageRedisService::remove_image_status(
    const std::string& saved_name,
    WriteCallback callback
) const {
    delete_command({ "DEL", status_key(saved_name) }, std::move(callback));
}

void ImageRedisService::check_upload_rate(
    const std::string& username,
    LimitCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            RATE_LIMIT_SCRIPT,
            "1",
            rate_key(username),
            "60",
            std::to_string(upload_rate_limit_per_minute_)
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer) {
                callback({ result.integer == 1, "" });
                return;
            }
            callback({
                false,
                result.error.empty()
                    ? "unexpected Redis rate-limit response"
                    : std::move(result.error)
            });
        }
    );
}

void ImageRedisService::acquire_upload_permit(
    const std::string& username,
    const std::string& lease_token,
    LimitCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            ACQUIRE_PERMIT_SCRIPT,
            "1",
            permit_key(username),
            std::to_string(max_concurrent_uploads_per_user_),
            std::to_string(upload_permit_ttl_seconds_),
            lease_token
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer) {
                callback({ result.integer == 1, "" });
                return;
            }
            callback({
                false,
                result.error.empty()
                    ? "unexpected Redis upload-permit response"
                    : std::move(result.error)
            });
        }
    );
}

void ImageRedisService::release_upload_permit(
    const std::string& username,
    const std::string& lease_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            RELEASE_PERMIT_SCRIPT,
            "1",
            permit_key(username),
            lease_token
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(write_result(
                std::move(result),
                RedisResultType::Integer,
                "unexpected Redis permit-release response"
            ));
        }
    );
}

void ImageRedisService::begin_upload(
    const std::string& username,
    const std::string& key,
    const std::string& payload_hash,
    IdempotencyCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            BEGIN_IDEMPOTENCY_SCRIPT,
            "1",
            idempotency_key(username, key),
            std::to_string(upload_idempotency_ttl_seconds_),
            payload_hash
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type != RedisResultType::String) {
                callback({
                    RedisIdempotencyState::Error,
                    "",
                    result.error.empty()
                        ? "unexpected Redis idempotency response"
                        : std::move(result.error)
                });
                return;
            }
            if (result.value == "CLAIMED") {
                callback({ RedisIdempotencyState::Claimed, "", "" });
                return;
            }
            if (result.value == "PROCESSING") {
                callback({ RedisIdempotencyState::Processing, "", "" });
                return;
            }
            if (result.value == "CONFLICT") {
                callback({ RedisIdempotencyState::Conflict, "", "" });
                return;
            }
            const size_t hash_end = result.value.find('\n', 5);
            const std::string done_prefix = hash_end == std::string::npos
                ? ""
                : result.value.substr(0, hash_end + 1);
            if (!done_prefix.empty() &&
                done_prefix.rfind("DONE\n", 0) == 0) {
                callback({
                    RedisIdempotencyState::Completed,
                    result.value.substr(done_prefix.size()),
                    ""
                });
                return;
            }
            callback({
                RedisIdempotencyState::Error,
                "",
                "invalid Redis idempotency payload"
            });
        }
    );
}

void ImageRedisService::complete_upload(
    const std::string& username,
    const std::string& key,
    const std::string& payload_hash,
    const std::string& response_body,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            COMPLETE_IDEMPOTENCY_SCRIPT,
            "1",
            idempotency_key(username, key),
            std::to_string(upload_idempotency_ttl_seconds_),
            payload_hash,
            response_body
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer && result.integer == 1) {
                callback({ true, "" });
                return;
            }
            callback({
                false,
                result.error.empty()
                    ? "idempotency claim expired or changed before completion"
                    : std::move(result.error)
            });
        }
    );
}

void ImageRedisService::abort_upload(
    const std::string& username,
    const std::string& key,
    const std::string& payload_hash,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            ABORT_IDEMPOTENCY_SCRIPT,
            "1",
            idempotency_key(username, key),
            payload_hash
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            callback(write_result(
                std::move(result),
                RedisResultType::Integer,
                "unexpected Redis idempotency-abort response"
            ));
        }
    );
}
