#include "cache/ImageRedisService.h"
#include "redis/AsyncRedisClient.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

template <typename Result, typename Start>
Result run(Start start) {
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    start([promise](Result result) mutable {
        promise->set_value(std::move(result));
    });
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        Result result;
        result.error = "operation timed out";
        return result;
    }
    return future.get();
}

bool expect(bool condition, const std::string& message, int& failures) {
    if (condition) {
        return true;
    }
    std::cerr << message << '\n';
    ++failures;
    return false;
}

}

int main() {
    const char* enabled = std::getenv("RUN_REDIS_INTEGRATION_TESTS");
    if (!enabled || std::string(enabled) != "1") {
        std::cout << "set RUN_REDIS_INTEGRATION_TESTS=1 to run\n";
        return 77;
    }

    setenv("IMAGE_LIST_CACHE_TTL_SECONDS", "30", 1);
    setenv("IMAGE_STATUS_CACHE_TTL_SECONDS", "30", 1);
    setenv("UPLOAD_RATE_LIMIT_PER_MINUTE", "2", 1);
    setenv("MAX_CONCURRENT_UPLOADS_PER_USER", "2", 1);
    setenv("UPLOAD_PERMIT_TTL_SECONDS", "30", 1);
    setenv("UPLOAD_IDEMPOTENCY_TTL_SECONDS", "30", 1);

    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    const std::string redis_key_prefix =
        "myweb:test:image-redis-service:" + suffix + ":";
    setenv("REDIS_KEY_PREFIX", redis_key_prefix.c_str(), 1);

    AsyncRedisClient& client = AsyncRedisClient::instance();
    if (!client.init_from_env()) {
        std::cerr << client.status_message() << '\n';
        return 1;
    }

    ImageRedisService& service = ImageRedisService::instance();
    int failures = 0;
    const std::string username = "redis_image_user_" + suffix;
    const std::string image_name = "image_" + suffix + ".webp";
    const std::string body = "{ \"ok\": true, \"images\": [{\"name\":\"one.webp\"}] }\n";

    auto invalidate = run<RedisWriteResult>([&](auto callback) {
        service.invalidate_image_list(std::move(callback));
    });
    expect(invalidate.ok, "initial image-list invalidation failed", failures);

    auto list_miss = run<RedisCacheResult>([&](auto callback) {
        service.get_image_list(std::move(callback));
    });
    expect(list_miss.state == RedisCacheState::Miss && !list_miss.version.empty(),
        "empty image-list cache was not a miss", failures);

    auto list_put = run<RedisWriteResult>([&](auto callback) {
        service.put_image_list_if_version(
            body, list_miss.version, std::move(callback));
    });
    expect(list_put.ok, "image-list cache write failed", failures);

    auto list_hit = run<RedisCacheResult>([&](auto callback) {
        service.get_image_list(std::move(callback));
    });
    expect(list_hit.state == RedisCacheState::Hit && list_hit.value == body,
        "image-list cache did not return the exact JSON body", failures);

    const std::string stale_version = list_hit.version;
    auto version_bump = run<RedisWriteResult>([&](auto callback) {
        service.invalidate_image_list(std::move(callback));
    });
    auto stale_fill = run<RedisWriteResult>([&](auto callback) {
        service.put_image_list_if_version(
            body, stale_version, std::move(callback));
    });
    auto after_stale_fill = run<RedisCacheResult>([&](auto callback) {
        service.get_image_list(std::move(callback));
    });
    expect(version_bump.ok && stale_fill.ok && !stale_fill.applied &&
        after_stale_fill.state == RedisCacheState::Miss,
        "stale database result repopulated the image-list cache", failures);

    auto fresh_fill = run<RedisWriteResult>([&](auto callback) {
        service.put_image_list_if_version(
            body, after_stale_fill.version, std::move(callback));
    });
    auto after_fresh_fill = run<RedisCacheResult>([&](auto callback) {
        service.get_image_list(std::move(callback));
    });
    expect(fresh_fill.ok &&
        after_fresh_fill.state == RedisCacheState::Hit &&
        after_fresh_fill.value == body,
        "current database result did not fill image-list cache", failures);

    auto status_put = run<RedisWriteResult>([&](auto callback) {
        service.put_image_status(image_name, "READY", std::move(callback));
    });
    expect(status_put.ok, "image status cache write failed", failures);

    auto status_hit = run<RedisCacheResult>([&](auto callback) {
        service.get_image_status(image_name, std::move(callback));
    });
    expect(status_hit.state == RedisCacheState::Hit && status_hit.value == "READY",
        "image status cache did not return READY", failures);

    auto status_delete = run<RedisWriteResult>([&](auto callback) {
        service.remove_image_status(image_name, std::move(callback));
    });
    expect(status_delete.ok, "image status invalidation failed", failures);

    auto rate1 = run<RedisLimitResult>([&](auto callback) {
        service.check_upload_rate(username, std::move(callback));
    });
    auto rate2 = run<RedisLimitResult>([&](auto callback) {
        service.check_upload_rate(username, std::move(callback));
    });
    auto rate3 = run<RedisLimitResult>([&](auto callback) {
        service.check_upload_rate(username, std::move(callback));
    });
    expect(rate1.allowed && rate2.allowed && !rate3.allowed && rate3.error.empty(),
        "per-minute upload rate boundary was not 2", failures);

    auto permit1 = run<RedisLimitResult>([&](auto callback) {
        service.acquire_upload_permit(username, "lease-a", std::move(callback));
    });
    auto permit2 = run<RedisLimitResult>([&](auto callback) {
        service.acquire_upload_permit(username, "lease-b", std::move(callback));
    });
    auto permit3 = run<RedisLimitResult>([&](auto callback) {
        service.acquire_upload_permit(username, "lease-c", std::move(callback));
    });
    expect(permit1.allowed && permit2.allowed && !permit3.allowed && permit3.error.empty(),
        "concurrent upload permit boundary was not 2", failures);

    auto release1 = run<RedisWriteResult>([&](auto callback) {
        service.release_upload_permit(username, "lease-a", std::move(callback));
    });
    auto permit4 = run<RedisLimitResult>([&](auto callback) {
        service.acquire_upload_permit(username, "lease-d", std::move(callback));
    });
    expect(release1.ok && permit4.allowed,
        "released upload permit was not reusable", failures);

    run<RedisWriteResult>([&](auto callback) {
        service.release_upload_permit(username, "lease-b", std::move(callback));
    });
    run<RedisWriteResult>([&](auto callback) {
        service.release_upload_permit(username, "lease-d", std::move(callback));
    });

    // Simulate expiration of old leases. A delayed release from the old
    // generation must not remove a newly acquired lease.
    run<RedisWriteResult>([&](auto callback) {
        client.command(
            { "DEL", client.key_prefix() + "upload:active:" + username },
            [callback = std::move(callback)](AsyncRedisResult result) mutable {
                callback({ result.type == RedisResultType::Integer, result.error });
            }
        );
    });
    auto new1 = run<RedisLimitResult>([&](auto callback) {
        service.acquire_upload_permit(username, "lease-new-a", std::move(callback));
    });
    auto new2 = run<RedisLimitResult>([&](auto callback) {
        service.acquire_upload_permit(username, "lease-new-b", std::move(callback));
    });
    auto stale_release = run<RedisWriteResult>([&](auto callback) {
        service.release_upload_permit(username, "lease-a", std::move(callback));
    });
    auto must_reject = run<RedisLimitResult>([&](auto callback) {
        service.acquire_upload_permit(username, "lease-new-c", std::move(callback));
    });
    expect(new1.allowed && new2.allowed && stale_release.ok &&
        !must_reject.allowed,
        "stale permit release removed a new lease", failures);
    run<RedisWriteResult>([&](auto callback) {
        client.command(
            { "DEL", client.key_prefix() + "upload:active:" + username },
            [callback = std::move(callback)](AsyncRedisResult result) mutable {
                callback({ result.type == RedisResultType::Integer, result.error });
            }
        );
    });

    const std::string idem_key = "idem-key-" + suffix;
    const std::string payload_hash = "payload-hash-a";
    auto idem1 = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(username, idem_key, payload_hash, std::move(callback));
    });
    auto idem2 = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(username, idem_key, payload_hash, std::move(callback));
    });
    expect(idem1.state == RedisIdempotencyState::Claimed &&
        idem2.state == RedisIdempotencyState::Processing,
        "idempotency claim did not distinguish first and active requests", failures);
    auto idem_conflict = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(
            username, idem_key, "payload-hash-b", std::move(callback));
    });
    expect(idem_conflict.state == RedisIdempotencyState::Conflict,
        "idempotency key accepted different in-flight content", failures);

    auto complete = run<RedisWriteResult>([&](auto callback) {
        service.complete_upload(
            username, idem_key, payload_hash, body, std::move(callback));
    });
    auto idem3 = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(username, idem_key, payload_hash, std::move(callback));
    });
    expect(complete.ok && idem3.state == RedisIdempotencyState::Completed &&
        idem3.response_body == body,
        "completed idempotency result was not replayed exactly", failures);

    auto abort_completed = run<RedisWriteResult>([&](auto callback) {
        service.abort_upload(
            username, idem_key, payload_hash, std::move(callback));
    });
    auto idem4 = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(username, idem_key, payload_hash, std::move(callback));
    });
    expect(abort_completed.ok && idem4.state == RedisIdempotencyState::Completed,
        "abort removed a completed idempotency result", failures);
    auto completed_conflict = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(
            username, idem_key, "payload-hash-b", std::move(callback));
    });
    expect(completed_conflict.state == RedisIdempotencyState::Conflict,
        "idempotency key replayed response for different content", failures);

    const std::string retry_key = "retry-key-" + suffix;
    auto retry1 = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(
            username, retry_key, payload_hash, std::move(callback));
    });
    auto abort_processing = run<RedisWriteResult>([&](auto callback) {
        service.abort_upload(
            username, retry_key, payload_hash, std::move(callback));
    });
    auto retry2 = run<RedisIdempotencyResult>([&](auto callback) {
        service.begin_upload(
            username, retry_key, payload_hash, std::move(callback));
    });
    expect(retry1.state == RedisIdempotencyState::Claimed && abort_processing.ok &&
        retry2.state == RedisIdempotencyState::Claimed,
        "aborted processing claim could not be retried", failures);
    run<RedisWriteResult>([&](auto callback) {
        service.abort_upload(
            username, retry_key, payload_hash, std::move(callback));
    });

    auto clear_ranking = run<RedisWriteResult>([&](auto callback) {
        service.remove_image_rankings(image_name, std::move(callback));
    });
    expect(clear_ranking.ok, "initial ranking cleanup failed", failures);

    auto empty_download_ranking = run<RedisRankingResult>([&](auto callback) {
        service.get_image_ranking(
            RedisRankingType::Downloads,
            100,
            std::move(callback)
        );
    });
    expect(empty_download_ranking.ok && empty_download_ranking.json == "[]",
        "empty ranking was not serialized as a JSON array", failures);

    auto view1 = run<RedisWriteResult>([&](auto callback) {
        service.increment_image_view(image_name, std::move(callback));
    });
    auto view2 = run<RedisWriteResult>([&](auto callback) {
        service.increment_image_view(image_name, std::move(callback));
    });
    auto download = run<RedisWriteResult>([&](auto callback) {
        service.increment_image_download(image_name, std::move(callback));
    });
    expect(view1.ok && view2.ok && download.ok,
        "ranking increments failed", failures);

    auto view_ranking = run<RedisRankingResult>([&](auto callback) {
        service.get_image_ranking(
            RedisRankingType::Views,
            100,
            std::move(callback)
        );
    });
    expect(view_ranking.ok &&
        view_ranking.json.find("\"name\":\"" + image_name + "\"") !=
            std::string::npos &&
        view_ranking.json.find("\"score\":2") != std::string::npos,
        "view ranking did not contain score 2", failures);

    auto status_again = run<RedisWriteResult>([&](auto callback) {
        service.put_image_status(image_name, "READY", std::move(callback));
    });
    expect(status_again.ok, "could not prepare status for derived cleanup", failures);

    const std::string upload_token = "upload-token-" + suffix;
    const std::string delete_token = "delete-token-" + suffix;
    auto begin_delete = run<RedisWriteResult>([&](auto callback) {
        service.begin_image_delete(
            image_name, delete_token, std::move(callback));
    });
    auto hidden_ranking = run<RedisRankingResult>([&](auto callback) {
        service.get_image_ranking(
            RedisRankingType::Views,
            100,
            std::move(callback)
        );
    });
    auto hidden_status = run<RedisCacheResult>([&](auto callback) {
        service.get_image_status(image_name, std::move(callback));
    });
    auto blocked_increment = run<RedisWriteResult>([&](auto callback) {
        service.increment_image_view(image_name, std::move(callback));
    });
    expect(begin_delete.ok && begin_delete.applied && hidden_ranking.ok &&
        hidden_ranking.json.find(image_name) == std::string::npos &&
        hidden_status.state == RedisCacheState::Miss &&
        blocked_increment.ok && !blocked_increment.applied,
        "deleting lifecycle did not hide or fence the image", failures);

    auto stale_activate = run<RedisWriteResult>([&](auto callback) {
        service.activate_image(
            image_name, "PROCESSING", upload_token, std::move(callback));
    });
    expect(stale_activate.ok && !stale_activate.applied,
        "late upload activation resurrected a deleting image", failures);

    auto cancel_delete = run<RedisWriteResult>([&](auto callback) {
        service.cancel_image_delete(
            image_name, delete_token, std::move(callback));
    });
    auto restored_ranking = run<RedisRankingResult>([&](auto callback) {
        service.get_image_ranking(
            RedisRankingType::Views, 100, std::move(callback));
    });
    auto restored_status = run<RedisCacheResult>([&](auto callback) {
        service.get_image_status(image_name, std::move(callback));
    });
    expect(cancel_delete.ok && cancel_delete.applied && restored_ranking.ok &&
        restored_ranking.json.find(image_name) != std::string::npos &&
        restored_status.state == RedisCacheState::Hit &&
        restored_status.value == "READY",
        "delete rollback did not preserve rankings and status", failures);

    const std::string reupload_name = "reupload_" + suffix + ".webp";
    const std::string upload1 = "upload-one-" + suffix;
    const std::string delete2 = "delete-two-" + suffix;
    auto begin_upload = run<RedisWriteResult>([&](auto callback) {
        service.begin_image_upload(
            reupload_name, upload1, std::move(callback));
    });
    auto begin_upload_retry = run<RedisWriteResult>([&](auto callback) {
        service.begin_image_upload(
            reupload_name, upload1, std::move(callback));
    });
    auto begin_delete2 = run<RedisWriteResult>([&](auto callback) {
        service.begin_image_delete(
            reupload_name, delete2, std::move(callback));
    });
    auto begin_delete_retry = run<RedisWriteResult>([&](auto callback) {
        service.begin_image_delete(
            reupload_name, delete2, std::move(callback));
    });
    auto delayed_activate = run<RedisWriteResult>([&](auto callback) {
        service.activate_image(
            reupload_name, "PROCESSING", upload1, std::move(callback));
    });
    expect(begin_upload.ok && begin_upload.applied &&
        begin_upload_retry.ok && begin_upload_retry.applied &&
        begin_delete2.ok && begin_delete2.applied &&
        begin_delete_retry.ok && begin_delete_retry.applied &&
        delayed_activate.ok && !delayed_activate.applied,
        "delete did not fence a delayed activation from another Redis connection",
        failures);

    auto cancel_delete2 = run<RedisWriteResult>([&](auto callback) {
        service.cancel_image_delete(
            reupload_name, delete2, std::move(callback));
    });
    auto activate_after_cancel = run<RedisWriteResult>([&](auto callback) {
        service.activate_image(
            reupload_name, "PROCESSING", upload1, std::move(callback));
    });
    auto activate_retry = run<RedisWriteResult>([&](auto callback) {
        service.activate_image(
            reupload_name, "PROCESSING", upload1, std::move(callback));
    });
    auto active_increment = run<RedisWriteResult>([&](auto callback) {
        service.increment_image_view(reupload_name, std::move(callback));
    });
    expect(cancel_delete2.ok && cancel_delete2.applied &&
        activate_after_cancel.ok && activate_after_cancel.applied &&
        activate_retry.ok && activate_retry.applied &&
        active_increment.ok && active_increment.applied,
        "rolled-back delete did not restore the in-flight upload generation",
        failures);

    const std::string delete3 = "delete-three-" + suffix;
    auto begin_delete3 = run<RedisWriteResult>([&](auto callback) {
        service.begin_image_delete(
            reupload_name, delete3, std::move(callback));
    });
    auto finalize_delete = run<RedisWriteResult>([&](auto callback) {
        service.finalize_image_delete(
            reupload_name, delete3, std::move(callback));
    });
    auto finalize_retry = run<RedisWriteResult>([&](auto callback) {
        service.finalize_image_delete(
            reupload_name, delete3, std::move(callback));
    });
    auto stale_after_finalize = run<RedisWriteResult>([&](auto callback) {
        service.activate_image(
            reupload_name, "READY", upload1, std::move(callback));
    });
    auto ranking_after_finalize = run<RedisRankingResult>([&](auto callback) {
        service.get_image_ranking(
            RedisRankingType::Views, 100, std::move(callback));
    });
    expect(begin_delete3.ok && begin_delete3.applied &&
        finalize_delete.ok && finalize_delete.applied &&
        finalize_retry.ok && finalize_retry.applied &&
        stale_after_finalize.ok && !stale_after_finalize.applied &&
        ranking_after_finalize.ok &&
        ranking_after_finalize.json.find(reupload_name) == std::string::npos,
        "finalized deletion accepted an old upload generation", failures);

    const std::string upload2 = "upload-two-" + suffix;
    auto reupload = run<RedisWriteResult>([&](auto callback) {
        service.begin_image_upload(
            reupload_name, upload2, std::move(callback));
    });
    auto reactivate = run<RedisWriteResult>([&](auto callback) {
        service.activate_image(
            reupload_name, "PROCESSING", upload2, std::move(callback));
    });
    expect(reupload.ok && reupload.applied && reactivate.ok && reactivate.applied,
        "a new upload generation could not reuse a finalized image name", failures);

    auto final_cleanup = run<RedisWriteResult>([&](auto callback) {
        service.force_finalize_image_delete(reupload_name, std::move(callback));
    });
    auto original_cleanup = run<RedisWriteResult>([&](auto callback) {
        service.force_finalize_image_delete(image_name, std::move(callback));
    });
    expect(final_cleanup.ok && original_cleanup.ok,
        "final lifecycle cleanup failed", failures);

    service.invalidate_image_list([](RedisWriteResult) {});
    client.shutdown();
    if (failures != 0) {
        return 2;
    }
    std::cout << "image Redis service test passed\n";
    return 0;
}
