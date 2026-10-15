#include "auth/AsyncSessionManager.h"
#include "cache/ImageRedisService.h"
#include "db/UserStore.h"
#include "image/ImageActivationJournal.h"
#include "image/ImageDeletionReconciler.h"
#include "image/ImageProcessor.h"
#include "log/AsyncLogger.h"
#include "redis/AsyncRedisClient.h"
#include "server/WebServer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

using RedisWriteStarter =
    std::function<void(ImageRedisService::WriteCallback)>;

RedisWriteResult run_redis_write_with_retry(
    const std::string& operation,
    const std::string& saved_name,
    const RedisWriteStarter& start
) {
    RedisWriteResult last{ false, "Redis recovery command timed out", false };
    for (int attempt = 1; attempt <= 3; ++attempt) {
        auto promise = std::make_shared<std::promise<RedisWriteResult>>();
        auto delivered = std::make_shared<std::atomic<bool>>(false);
        std::future<RedisWriteResult> future = promise->get_future();
        start([promise, delivered](RedisWriteResult result) mutable {
            if (!delivered->exchange(true, std::memory_order_acq_rel)) {
                promise->set_value(std::move(result));
            }
        });
        if (future.wait_for(std::chrono::milliseconds(3500)) ==
            std::future_status::ready) {
            last = future.get();
            if (last.ok) return last;
        }
    }
    LOG_WARN(operation + " failed for " + saved_name + ": " + last.error);
    return last;
}

void clear_activation_marker(
    const std::filesystem::path& image_directory,
    const std::string& saved_name
) {
    std::string error;
    if (!clear_image_activation(image_directory, saved_name, error)) {
        LOG_WARN("clear recovered activation journal failed for " +
            saved_name + ": " + error);
    }
}

void queue_recovered_thumbnail(
    const std::filesystem::path& image_directory,
    const PendingImageActivation& pending
) {
    const std::string thumbnail_name =
        std::filesystem::path(pending.saved_name).stem().string() + ".webp";
    const std::string source_path =
        (image_directory / pending.saved_name).string();
    const std::string thumbnail_path =
        (image_directory / "thumbnails" / thumbnail_name).string();
    const bool queued = ImageProcessor::instance().try_enqueue({
        source_path,
        thumbnail_path,
        [pending, source_path, thumbnail_path](ImageProcessResult result) {
            if (!std::filesystem::is_regular_file(source_path)) {
                std::error_code ignored;
                std::filesystem::remove(thumbnail_path, ignored);
                return;
            }
            if (!result.ok) {
                LOG_WARN("recovered thumbnail generation failed for " +
                    pending.saved_name + ": " + result.error);
            }
            ImageRedisService::instance().put_image_status_for_generation(
                pending.saved_name,
                result.ok ? "READY" : "FAILED",
                pending.upload_token,
                [](RedisWriteResult status_result) {
                    if (status_result.ok && status_result.applied) {
                        ImageRedisService::instance().invalidate_image_list(
                            [](RedisWriteResult) {});
                    }
                });
        }
    });
    if (!queued) {
        LOG_WARN("thumbnail recovery queue is full for " + pending.saved_name);
        ImageRedisService::instance().put_image_status_for_generation(
            pending.saved_name, "FAILED", pending.upload_token,
            [](RedisWriteResult status_result) {
                if (status_result.ok && status_result.applied) {
                    ImageRedisService::instance().invalidate_image_list(
                        [](RedisWriteResult) {});
                }
            });
    }
}

void reconcile_image_activations(
    const std::filesystem::path& image_directory
) {
    for (const PendingImageActivation& pending :
         load_pending_image_activations(image_directory)) {
        ImageDeleteResult metadata = UserStore::instance().check_image_owner(
            "", pending.saved_name);
        const bool metadata_present =
            metadata.status == ImageDeleteStatus::Ok ||
            metadata.status == ImageDeleteStatus::Forbidden;
        const bool source_present = std::filesystem::is_regular_file(
            image_directory / pending.saved_name);

        if (metadata.status == ImageDeleteStatus::NotFound) {
            RedisWriteResult aborted = run_redis_write_with_retry(
                "abort orphaned upload lifecycle", pending.saved_name,
                [&pending](ImageRedisService::WriteCallback callback) {
                    ImageRedisService::instance().abort_image_upload(
                        pending.saved_name, pending.upload_token,
                        std::move(callback));
                });
            if (aborted.ok) {
                clear_activation_marker(image_directory, pending.saved_name);
            }
            continue;
        }

        if (!metadata_present) {
            LOG_WARN("activation recovery could not read metadata for " +
                pending.saved_name + ": " + metadata.message);
            continue;
        }

        if (!source_present) {
            if (pending.username.empty()) {
                LOG_WARN("legacy activation journal has no owner for " +
                    pending.saved_name);
                continue;
            }
            ImageDeleteResult compensation =
                UserStore::instance().delete_owned_image(
                    pending.username, pending.saved_name);
            if (compensation.status == ImageDeleteStatus::CommitUnknown) {
                ImageDeleteResult observed =
                    UserStore::instance().check_image_owner(
                        "", pending.saved_name);
                if (observed.status == ImageDeleteStatus::NotFound) {
                    compensation.status = ImageDeleteStatus::Ok;
                }
            }
            if (compensation.status != ImageDeleteStatus::Ok &&
                compensation.status != ImageDeleteStatus::NotFound) {
                LOG_WARN("activation recovery could not remove metadata for " +
                    pending.saved_name + ": " + compensation.message);
                continue;
            }
            RedisWriteResult aborted = run_redis_write_with_retry(
                "abort upload without source file", pending.saved_name,
                [&pending](ImageRedisService::WriteCallback callback) {
                    ImageRedisService::instance().abort_image_upload(
                        pending.saved_name, pending.upload_token,
                        std::move(callback));
                });
            if (aborted.ok) {
                clear_activation_marker(image_directory, pending.saved_name);
            }
            continue;
        }

        const std::filesystem::path thumbnail =
            image_directory / "thumbnails" /
            (std::filesystem::path(pending.saved_name).stem().string() +
             ".webp");
        const bool thumbnail_ready =
            std::filesystem::is_regular_file(thumbnail);
        RedisWriteResult activated = run_redis_write_with_retry(
            "activate recovered upload", pending.saved_name,
            [&pending, thumbnail_ready](
                ImageRedisService::WriteCallback callback) {
                ImageRedisService::instance().activate_image(
                    pending.saved_name,
                    thumbnail_ready ? "READY" : "PROCESSING",
                    pending.upload_token,
                    std::move(callback));
            });
        if (!activated.ok) continue;

        clear_activation_marker(image_directory, pending.saved_name);
        if (activated.applied && !thumbnail_ready) {
            queue_recovered_thumbnail(image_directory, pending);
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    int port = 9006;

    std::string web_root = "../webroot";

    size_t thread_count = 6;

    if (argc >= 2) {
        port = std::stoi(argv[1]);
    }

    if (argc >= 3) {
        web_root = argv[2];
    }

    if (argc >= 4) {
        thread_count = static_cast<size_t>(std::stoul(argv[3]));
    }

    AsyncLogger::instance().init("server.log");

    if (UserStore::instance().init_from_env()) {
        LOG_INFO("mysql user store initialized");
    }
    else {
        LOG_WARN("mysql user store unavailable: " + UserStore::instance().status_message());
    }

    if (AsyncSessionManager::instance().init_from_env()) {
        LOG_INFO("async redis session store initialized");
    }
    else {
        LOG_WARN("async redis session store unavailable: " +
            AsyncSessionManager::instance().status_message());
    }

    const std::filesystem::path image_directory =
        std::filesystem::path(web_root) / "images";
    ImageDeletionRecoveryStats recovery =
        reconcile_image_deletion_quarantine(
            image_directory,
            [](const std::string& name) {
                ImageDeleteResult result =
                    UserStore::instance().check_image_owner("", name);
                if (result.status == ImageDeleteStatus::NotFound) {
                    return ImageMetadataState::Missing;
                }
                if (result.status == ImageDeleteStatus::Ok ||
                    result.status == ImageDeleteStatus::Forbidden) {
                    return ImageMetadataState::Present;
                }
                return ImageMetadataState::Unknown;
            },
            [](const std::string& name) {
                RedisWriteResult result = run_redis_write_with_retry(
                    "restore image lifecycle", name,
                    [&name](ImageRedisService::WriteCallback callback) {
                        ImageRedisService::instance().recover_image_after_delete(
                            name, std::move(callback));
                    });
                return result.ok;
            },
            [](const std::string& name) {
                RedisWriteResult result = run_redis_write_with_retry(
                    "finalize recovered deletion", name,
                    [&name](ImageRedisService::WriteCallback callback) {
                        ImageRedisService::instance().force_finalize_image_delete(
                            name, std::move(callback));
                    });
                return result.ok;
            });
    if (recovery.restored || recovery.discarded || recovery.unresolved) {
        LOG_INFO("image deletion recovery: restored=" +
            std::to_string(recovery.restored) + ", discarded=" +
            std::to_string(recovery.discarded) + ", unresolved=" +
            std::to_string(recovery.unresolved));
    }

    reconcile_image_activations(image_directory);

    WebServer server(
        port,
        web_root,
        thread_count
    );

    if (!server.init()) {
        std::cerr << "server init failed" << std::endl;
        return 1;
    }

    server.start();

    ImageProcessor::instance().shutdown();
    AsyncSessionManager::instance().shutdown();
    AsyncRedisClient::instance().shutdown();
    AsyncLogger::instance().shutdown();

    return 0;
}
