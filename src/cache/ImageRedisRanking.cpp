#include "cache/ImageRedisService.h"

#include "redis/AsyncRedisClient.h"

#include <algorithm>
#include <string>
#include <utility>

namespace {

const char* RANKING_JSON_SCRIPT = R"lua(
local requested = tonumber(ARGV[1])
local values = redis.call(
    'ZREVRANGE', KEYS[1], 0, requested * 4 - 1, 'WITHSCORES'
)
local entries = {}
for i = 1, #values, 2 do
    local lifecycle = redis.call('GET', ARGV[2] .. values[i])
    if not lifecycle or string.sub(lifecycle, 1, 7) == 'ACTIVE\n' then
        entries[#entries + 1] = {
            name = values[i],
            score = tonumber(values[i + 1])
        }
        if #entries >= requested then
            break
        end
    end
end
if #entries == 0 then
    return '[]'
end
return cjson.encode(entries)
)lua";

const char* REMOVE_RANKINGS_SCRIPT = R"lua(
local removed = redis.call('ZREM', KEYS[1], ARGV[1])
removed = removed + redis.call('ZREM', KEYS[2], ARGV[1])
return removed
)lua";

const char* FINALIZE_IMAGE_DELETE_SCRIPT = R"lua(
local current = redis.call('GET', KEYS[6])
if current == 'DELETED\n' .. ARGV[2] then
    return 1
end
local expected = 'DELETING\n' .. ARGV[2] .. '\n'
if not current or string.sub(current, 1, string.len(expected)) ~= expected then
    return 0
end
redis.call('ZREM', KEYS[1], ARGV[1])
redis.call('ZREM', KEYS[2], ARGV[1])
redis.call('DEL', KEYS[3])
redis.call('DEL', KEYS[4])
redis.call('INCR', KEYS[5])
redis.call('SETEX', KEYS[6], ARGV[3], 'DELETED\n' .. ARGV[2])
return 1
)lua";

const char* FORCE_FINALIZE_IMAGE_DELETE_SCRIPT = R"lua(
redis.call('ZREM', KEYS[1], ARGV[1])
redis.call('ZREM', KEYS[2], ARGV[1])
redis.call('DEL', KEYS[3])
redis.call('DEL', KEYS[4])
redis.call('INCR', KEYS[5])
redis.call('SETEX', KEYS[6], ARGV[2], 'DELETED\nRECOVERY')
return 1
)lua";

const char* INCREMENT_RANKING_SCRIPT = R"lua(
local lifecycle = redis.call('GET', KEYS[2])
if lifecycle and string.sub(lifecycle, 1, 7) ~= 'ACTIVE\n' then
    return 0
end
redis.call('ZINCRBY', KEYS[1], 1, ARGV[1])
return 1
)lua";

void increment_ranking(
    const std::string& key,
    const std::string& lifecycle_key,
    const std::string& saved_name,
    ImageRedisService::WriteCallback callback
) {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            INCREMENT_RANKING_SCRIPT,
            "2",
            key,
            lifecycle_key,
            saved_name
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer)
                callback({ true, "", result.integer == 1 });
            else callback({ false, result.error.empty()
                ? "unexpected Redis ranking increment response"
                : std::move(result.error), false });
        }
    );
}

}  // namespace

std::string ImageRedisService::ranking_key(RedisRankingType type) const {
    return AsyncRedisClient::instance().key_prefix() +
        (type == RedisRankingType::Views
            ? "image:rank:views"
            : "image:rank:downloads");
}

void ImageRedisService::increment_image_view(
    const std::string& saved_name,
    WriteCallback callback
) const {
    increment_ranking(
        ranking_key(RedisRankingType::Views),
        lifecycle_key(saved_name),
        saved_name,
        std::move(callback)
    );
}

void ImageRedisService::increment_image_download(
    const std::string& saved_name,
    WriteCallback callback
) const {
    increment_ranking(
        ranking_key(RedisRankingType::Downloads),
        lifecycle_key(saved_name),
        saved_name,
        std::move(callback)
    );
}

void ImageRedisService::get_image_ranking(
    RedisRankingType type,
    size_t limit,
    RankingCallback callback
) const {
    limit = std::max<size_t>(1, std::min<size_t>(limit, 100));
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            RANKING_JSON_SCRIPT,
            "1",
            ranking_key(type),
            std::to_string(limit),
            lifecycle_prefix()
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::String) {
                callback({ true, std::move(result.value), "" });
                return;
            }
            callback({
                false,
                "[]",
                result.error.empty()
                    ? "unexpected Redis ranking response"
                    : std::move(result.error)
            });
        }
    );
}

void ImageRedisService::remove_image_rankings(
    const std::string& saved_name,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            REMOVE_RANKINGS_SCRIPT,
            "2",
            ranking_key(RedisRankingType::Views),
            ranking_key(RedisRankingType::Downloads),
            saved_name
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer) {
                callback({ true, "" });
                return;
            }
            callback({
                false,
                result.error.empty()
                    ? "unexpected Redis ranking removal response"
                    : std::move(result.error)
            });
        }
    );
}

void ImageRedisService::finalize_image_delete(
    const std::string& saved_name,
    const std::string& delete_token,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            FINALIZE_IMAGE_DELETE_SCRIPT,
            "6",
            ranking_key(RedisRankingType::Views),
            ranking_key(RedisRankingType::Downloads),
            status_key(saved_name),
            list_key(),
            list_version_key(),
            lifecycle_key(saved_name),
            saved_name,
            delete_token,
            "604800"
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer) {
                callback({ true, "", result.integer == 1 });
                return;
            }
            callback({
                false,
                result.error.empty()
                    ? "unexpected Redis derived-data cleanup response"
                    : std::move(result.error),
                false
            });
        }
    );
}

void ImageRedisService::force_finalize_image_delete(
    const std::string& saved_name,
    WriteCallback callback
) const {
    AsyncRedisClient::instance().command(
        {
            "EVAL",
            FORCE_FINALIZE_IMAGE_DELETE_SCRIPT,
            "6",
            ranking_key(RedisRankingType::Views),
            ranking_key(RedisRankingType::Downloads),
            status_key(saved_name),
            list_key(),
            list_version_key(),
            lifecycle_key(saved_name),
            saved_name,
            "604800"
        },
        [callback = std::move(callback)](AsyncRedisResult result) mutable {
            if (result.type == RedisResultType::Integer && result.integer == 1) {
                callback({ true, "", true });
                return;
            }
            callback({
                false,
                result.error.empty()
                    ? "unexpected Redis forced deletion cleanup response"
                    : std::move(result.error),
                false
            });
        }
    );
}
