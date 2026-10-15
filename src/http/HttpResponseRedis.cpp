#include "http/HttpResponse.h"

#include "cache/ImageRedisService.h"
#include "db/UserStore.h"
#include "http/HttpRedisPolicy.h"
#include "image/ImageActivationJournal.h"
#include "image/ImageDeleteGuard.h"
#include "log/AsyncLogger.h"
#include "utils/Metrics.h"
#include "utils/Sha256.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace {

void ignore_write_failure(const char* operation, RedisWriteResult result) {
    if (!result.ok) {
        LOG_WARN(std::string(operation) + " failed: " + result.error);
    }
}

}

class HttpResponse::UploadRedisFlow
    : public std::enable_shared_from_this<HttpResponse::UploadRedisFlow> {
public:
    UploadRedisFlow(
        HttpResponse* owner,
        HttpRequest request,
        SessionInfo session,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    )
        : owner_(owner),
          request_(std::move(request)),
          session_(std::move(session)),
          keep_alive_(keep_alive),
          callback_(std::move(callback)),
          executor_(std::move(executor)),
          idempotency_key_(request_.get_header("idempotency-key")) {
    }

    void start() {
        if (!is_valid_upload_idempotency_key(idempotency_key_)) {
            finish(owner_->build_json_response(
                400,
                "Bad Request",
                "{ \"ok\": false, \"message\": \"invalid Idempotency-Key\" }\n",
                keep_alive_
            ));
            return;
        }

        auto self = shared_from_this();
        if (!executor_([self]() { self->prepare_idempotency(); })) {
            fail_service("server is busy; retry later");
        }
    }

private:
    void prepare_idempotency() {
        if (owner_->request_cancelled()) {
            return;
        }
        std::string error;
        std::string filename;
        if (request_.has_streamed_upload()) {
            filename = owner_->url_decode(request_.upload_original_name());
            if (!Sha256::digest_file(
                    request_.upload_temp_path(), payload_hash_, error)) {
                finish(owner_->build_json_response(
                    500,
                    "Internal Server Error",
                    "{ \"ok\": false, \"message\": \"could not hash upload\" }\n",
                    keep_alive_
                ));
                return;
            }
        }
        else {
            std::string content;
            if (!owner_->parse_multipart_image(
                    request_, filename, content, error)) {
                finish(owner_->build_json_response(
                    400,
                    "Bad Request",
                    "{ \"ok\": false, \"message\": \"invalid upload body\" }\n",
                    keep_alive_
                ));
                return;
            }
            payload_hash_ = UserStore::sha256(content);
        }

        const std::string safe_name = owner_->safe_filename(filename);
        if (safe_name.empty() || safe_name.size() > 200 ||
            !owner_->is_allowed_image_name(safe_name)) {
            finish(owner_->build_json_response(
                400,
                "Bad Request",
                "{ \"ok\": false, \"message\": \"invalid image filename or extension\" }\n",
                keep_alive_
            ));
            return;
        }
        const size_t dot = safe_name.find_last_of('.');
        std::string extension = safe_name.substr(dot);
        std::transform(
            extension.begin(), extension.end(), extension.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        saved_name_ = UserStore::sha256(session_.username).substr(0, 12) +
            "_" + payload_hash_ + extension;

        if (idempotency_key_.empty()) {
            check_rate();
            return;
        }

        auto self = shared_from_this();
        ImageRedisService::instance().begin_upload(
            session_.username,
            idempotency_key_,
            payload_hash_,
            [self](RedisIdempotencyResult result) {
                self->on_idempotency(std::move(result));
            }
        );
    }

private:
    void on_idempotency(RedisIdempotencyResult result) {
        if (result.state == RedisIdempotencyState::Completed) {
            Metrics::instance().inc_idempotency_replay();
            finish(owner_->build_json_response(
                200,
                "OK",
                result.response_body,
                keep_alive_
            ));
            return;
        }
        if (result.state == RedisIdempotencyState::Processing) {
            finish(owner_->build_raw_response(
                409,
                "Conflict",
                "application/json; charset=utf-8",
                "{ \"ok\": false, \"message\": \"upload is already processing\" }\n",
                keep_alive_,
                "Retry-After: 1\r\n"
            ));
            return;
        }
        if (result.state == RedisIdempotencyState::Conflict) {
            finish(owner_->build_json_response(
                409,
                "Conflict",
                "{ \"ok\": false, \"message\": \"Idempotency-Key was reused for different content\" }\n",
                keep_alive_
            ));
            return;
        }
        if (result.state == RedisIdempotencyState::Error) {
            fail_service("upload idempotency store unavailable");
            return;
        }

        claim_acquired_ = true;
        check_rate();
    }

    void check_rate() {
        auto self = shared_from_this();
        ImageRedisService::instance().check_upload_rate(
            session_.username,
            [self](RedisLimitResult result) {
                if (!result.error.empty()) {
                    self->abort_claim();
                    self->fail_service("upload rate limiter unavailable");
                    return;
                }
                if (!result.allowed) {
                    Metrics::instance().inc_rate_limited_upload();
                    Metrics::instance().inc_upload_rate_rejection();
                    self->abort_claim();
                    self->finish(self->owner_->build_raw_response(
                        429,
                        "Too Many Requests",
                        "application/json; charset=utf-8",
                        "{ \"ok\": false, \"message\": \"upload rate limit exceeded\" }\n",
                        self->keep_alive_,
                        "Retry-After: 60\r\n"
                    ));
                    return;
                }
                self->acquire_permit();
            }
        );
    }

    void acquire_permit() {
        static std::atomic<uint64_t> sequence{ 1 };
        permit_token_ = UserStore::sha256(
            session_.username + ":" +
            std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()) + ":" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed))
        );
        auto self = shared_from_this();
        ImageRedisService::instance().acquire_upload_permit(
            session_.username,
            permit_token_,
            [self](RedisLimitResult result) {
                if (!result.error.empty()) {
                    self->abort_claim();
                    self->fail_service("upload concurrency limiter unavailable");
                    return;
                }
                if (!result.allowed) {
                    Metrics::instance().inc_rate_limited_upload();
                    Metrics::instance().inc_upload_permit_rejection();
                    self->abort_claim();
                    self->finish(self->owner_->build_raw_response(
                        429,
                        "Too Many Requests",
                        "application/json; charset=utf-8",
                        "{ \"ok\": false, \"message\": \"too many concurrent uploads for this user\" }\n",
                        self->keep_alive_,
                        "Retry-After: 1\r\n"
                    ));
                    return;
                }
                self->permit_acquired_ = true;
                self->begin_image_lifecycle();
            }
        );
    }

    void begin_image_lifecycle() {
        upload_token_ = UserStore::sha256(permit_token_ + ":image-generation");
        auto self = shared_from_this();
        ImageRedisService::instance().begin_image_upload(
            saved_name_,
            upload_token_,
            [self](RedisWriteResult result) {
                if (!result.ok) {
                    self->release_permit();
                    self->abort_claim();
                    self->fail_service("image lifecycle store unavailable");
                    return;
                }
                if (!result.applied) {
                    Metrics::instance().inc_duplicate_upload();
                    self->release_permit();
                    self->abort_claim();
                    self->finish(self->owner_->build_json_response(
                        409,
                        "Conflict",
                        "{ \"ok\": false, \"message\": \"image is already uploaded or being deleted\" }\n",
                        self->keep_alive_
                    ));
                    return;
                }
                self->lifecycle_acquired_ = true;
                std::string journal_error;
                if (!persist_image_activation(
                        std::filesystem::path(self->owner_->images_dir()),
                        self->saved_name_, self->upload_token_,
                        self->session_.username, journal_error)) {
                    LOG_ERROR("persist image activation journal failed for " +
                        self->saved_name_ + ": " + journal_error);
                    self->release_permit();
                    self->abort_image_lifecycle();
                    self->abort_claim();
                    self->fail_service(
                        "could not persist image activation recovery record");
                    return;
                }
                self->activation_journaled_ = true;
                self->schedule_upload();
            }
        );
    }

    void schedule_upload() {
        auto self = shared_from_this();
        bool scheduled = executor_([self]() {
            self->run_upload();
        });
        if (!scheduled) {
            release_permit();
            clear_activation_journal();
            abort_image_lifecycle();
            abort_claim();
            fail_service("server is busy; retry later");
        }
    }

    void run_upload() {
        if (owner_->request_cancelled()) {
            release_permit();
            clear_activation_journal();
            abort_image_lifecycle();
            abort_claim();
            return;
        }

        std::string response = owner_->build_upload_image_response(
            request_,
            session_,
            keep_alive_,
            payload_hash_
        );
        bool uploaded = response.rfind("HTTP/1.1 200 OK\r\n", 0) == 0;
        if (!uploaded) {
            release_permit();
            clear_activation_journal();
            abort_image_lifecycle();
            abort_claim();
            if (!response.empty()) {
                finish(std::move(response));
            }
            return;
        }
        // File and MySQL metadata are durable now. Activate the Redis
        // generation before the thumbnail worker can publish READY/FAILED;
        // otherwise a fast worker can finish while the lifecycle is still
        // UPLOADING and its generation-fenced status write is discarded.
        pending_response_ = std::move(response);
        activation_attempts_remaining_ = 3;
        activate_image();
    }

    void activate_image() {
        auto self = shared_from_this();
        ImageRedisService::instance().activate_image(
            saved_name_, "PROCESSING", upload_token_,
            [self](RedisWriteResult result) {
                self->on_image_activated(std::move(result));
            }
        );
    }

    void on_image_activated(RedisWriteResult result) {
        if (!result.ok) {
            --activation_attempts_remaining_;
            if (activation_attempts_remaining_ > 0) {
                activate_image();
                return;
            }
            release_permit();
            lifecycle_acquired_ = false;
            finish(owner_->build_raw_response(
                202,
                "Accepted",
                "application/json; charset=utf-8",
                "{ \"ok\": true, \"pending\": true, \"message\": "
                "\"image saved; activation will resume after recovery\" }\n",
                keep_alive_,
                "Retry-After: 2\r\n"
            ));
            return;
        }
        release_permit();
        if (!result.applied) {
            lifecycle_acquired_ = false;
            clear_activation_journal();
            abort_claim();
            finish(owner_->build_json_response(
                409,
                "Conflict",
                "{ \"ok\": false, \"message\": \"upload was superseded by image deletion\" }\n",
                keep_alive_
            ));
            return;
        }

        owner_->queue_thumbnail_processing(saved_name_, upload_token_);
        lifecycle_acquired_ = false;
        clear_activation_journal();
        complete_success();
    }

    void clear_activation_journal() {
        if (!activation_journaled_) return;
        std::string error;
        if (!clear_image_activation(
                std::filesystem::path(owner_->images_dir()), saved_name_, error)) {
            LOG_WARN("clear image activation journal failed for " +
                saved_name_ + ": " + error);
            return;
        }
        activation_journaled_ = false;
    }

    void complete_success() {
        if (claim_acquired_) {
            std::string body = http_response_body(pending_response_);
            ImageRedisService::instance().complete_upload(
                session_.username, idempotency_key_, payload_hash_, body,
                [self = shared_from_this()](RedisWriteResult result) {
                    if (!result.ok) {
                        LOG_ERROR("complete upload idempotency failed: " + result.error);
                        self->finish(self->owner_->build_json_response(
                            503, "Service Unavailable",
                            "{ \"ok\": false, \"message\": \"upload saved but idempotency record is unavailable\" }\n",
                            self->keep_alive_));
                        return;
                    }
                    self->claim_acquired_ = false;
                    self->finish(std::move(self->pending_response_));
                }
            );
            return;
        }
        finish(std::move(pending_response_));
    }

    void release_permit() {
        if (!permit_acquired_) {
            return;
        }
        permit_acquired_ = false;
        ImageRedisService::instance().release_upload_permit(
            session_.username,
            permit_token_,
            [](RedisWriteResult result) {
                ignore_write_failure("release upload permit", std::move(result));
            }
        );
    }

    void abort_image_lifecycle() {
        if (!lifecycle_acquired_) {
            return;
        }
        lifecycle_acquired_ = false;
        ImageRedisService::instance().abort_image_upload(
            saved_name_,
            upload_token_,
            [](RedisWriteResult result) {
                ignore_write_failure("abort image upload lifecycle", std::move(result));
            }
        );
    }

    void abort_claim() {
        if (!claim_acquired_) {
            return;
        }
        claim_acquired_ = false;
        ImageRedisService::instance().abort_upload(
            session_.username,
            idempotency_key_,
            payload_hash_,
            [](RedisWriteResult result) {
                ignore_write_failure("abort upload idempotency", std::move(result));
            }
        );
    }

    void fail_service(const std::string& message) {
        finish(owner_->build_json_response(
            503,
            "Service Unavailable",
            "{ \"ok\": false, \"message\": \"" + message + "\" }\n",
            keep_alive_
        ));
    }

    void finish(std::string response) {
        if (finished_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        callback_(std::move(response));
    }

    HttpResponse* owner_;
    HttpRequest request_;
    SessionInfo session_;
    bool keep_alive_;
    CompletionCallback callback_;
    Executor executor_;
    std::string idempotency_key_;
    std::string payload_hash_;
    std::string saved_name_;
    std::string permit_token_;
    std::string upload_token_;
    std::string pending_response_;
    bool claim_acquired_{ false };
    bool permit_acquired_{ false };
    bool lifecycle_acquired_{ false };
    bool activation_journaled_{ false };
    int activation_attempts_remaining_{ 0 };
    std::atomic<bool> finished_{ false };
};

class HttpResponse::DeleteRedisFlow
    : public std::enable_shared_from_this<HttpResponse::DeleteRedisFlow> {
public:
    DeleteRedisFlow(
        HttpResponse* owner,
        HttpRequest request,
        SessionInfo session,
        bool keep_alive,
        CompletionCallback callback,
        Executor executor
    )
        : owner_(owner),
          request_(std::move(request)),
          session_(std::move(session)),
          keep_alive_(keep_alive),
          callback_(std::move(callback)),
          executor_(std::move(executor)) {
    }

    void start() {
        auto self = shared_from_this();
        if (!executor_([self]() { self->stage_delete(); })) {
            fail_service("server is busy; retry later");
        }
    }

private:
    void stage_delete() {
        if (owner_->request_cancelled()) {
            return;
        }

        const std::string requested = owner_->url_decode(
            owner_->query_value(request_.path(), "name"));
        name_ = owner_->safe_filename(requested);
        if (name_.empty() || name_ != requested || name_.size() > 255 ||
            !owner_->is_allowed_image_name(name_)) {
            finish(owner_->build_json_response(
                400,
                "Bad Request",
                "{ \"ok\": false, \"message\": \"invalid image name\" }\n",
                keep_alive_
            ));
            return;
        }

        ImageDeleteResult ownership = UserStore::instance().check_image_owner(
            session_.username, name_, owner_->cancelled_);
        if (ownership.status != ImageDeleteStatus::Ok) {
            finish_database_error(std::move(ownership));
            return;
        }

        guard_ = std::make_unique<ImageDeleteGuard>(
            std::filesystem::path(owner_->images_dir()), name_);
        std::string error;
        const ImageDeleteStageStatus stage_status = guard_->stage(error);
        if (stage_status == ImageDeleteStageStatus::InProgress) {
            finish(owner_->build_json_response(
                409,
                "Conflict",
                "{ \"ok\": false, \"message\": \"image deletion is already in progress\" }\n",
                keep_alive_
            ));
            return;
        }
        if (stage_status == ImageDeleteStageStatus::Error) {
            LOG_ERROR("stage image deletion failed for " + name_ + ": " + error);
            finish(owner_->build_json_response(
                500,
                "Internal Server Error",
                "{ \"ok\": false, \"message\": \"could not prepare image deletion\" }\n",
                keep_alive_
            ));
            return;
        }

        if (owner_->request_cancelled()) {
            if (!rollback_guard()) {
                finish_pending_recovery("delete preparation rollback is pending");
            }
            return;
        }
        static std::atomic<uint64_t> sequence{ 1 };
        delete_token_ = UserStore::sha256(
            session_.username + ":delete:" + name_ + ":" +
            std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()) + ":" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        begin_attempts_remaining_ = 3;
        begin_delete();
    }

    void begin_delete() {
        auto self = shared_from_this();
        ImageRedisService::instance().begin_image_delete(
            name_, delete_token_,
            [self](RedisWriteResult result) {
                self->on_delete_started(std::move(result));
            });
    }

    void on_delete_started(RedisWriteResult result) {
        if (!result.ok) {
            --begin_attempts_remaining_;
            if (begin_attempts_remaining_ > 0) {
                begin_delete();
                return;
            }
            LOG_ERROR("begin image deletion failed for " + name_ +
                ": " + result.error);
            // The command may have reached Redis even when its reply was lost.
            // Keep both the quarantine and any possible lifecycle fence so the
            // startup reconciler can decide from MySQL state.
            finish_pending_recovery("image deletion preparation is pending recovery");
            return;
        }
        if (!result.applied) {
            if (!rollback_guard()) {
                finish_pending_recovery("image delete conflict needs file recovery");
                return;
            }
            finish(owner_->build_json_response(
                409,
                "Conflict",
                "{ \"ok\": false, \"message\": \"image deletion is already in progress\" }\n",
                keep_alive_
            ));
            return;
        }
        marker_created_ = true;

        if (owner_->request_cancelled()) {
            auto self = shared_from_this();
            rollback_and_cancel_marker([self](bool restored) {
                if (!restored) {
                    self->finish_pending_recovery(
                        "cancelled delete is pending recovery");
                }
            });
            return;
        }

        auto self = shared_from_this();
        if (!executor_([self]() { self->commit_delete(); })) {
            rollback_and_cancel_marker([self](bool restored) {
                if (restored) self->fail_service("server is busy; retry later");
                else self->finish_pending_recovery(
                    "delete rollback is pending recovery");
            });
        }
    }

    void commit_delete() {
        if (owner_->request_cancelled()) {
            auto self = shared_from_this();
            rollback_and_cancel_marker([self](bool restored) {
                if (!restored) self->finish_pending_recovery(
                    "cancelled delete is pending recovery");
            });
            return;
        }

        ImageDeleteResult result = UserStore::instance().delete_owned_image(
            session_.username, name_, owner_->cancelled_);
        if (result.status == ImageDeleteStatus::CommitUnknown) {
            resolve_unknown_commit();
            return;
        }
        if (result.status != ImageDeleteStatus::Ok) {
            auto self = shared_from_this();
            rollback_and_cancel_marker(
                [self, result = std::move(result)](bool restored) mutable {
                    if (restored) {
                        self->finish_database_error(std::move(result));
                    }
                    else {
                        self->finish_pending_recovery(
                            "delete failed and file recovery is pending");
                    }
                });
            return;
        }
        finalize_attempts_remaining_ = 3;
        finalize_delete();
    }

    void resolve_unknown_commit() {
        ImageDeleteResult observed = UserStore::instance().check_image_owner(
            session_.username, name_);
        if (observed.status == ImageDeleteStatus::NotFound) {
            finalize_attempts_remaining_ = 3;
            finalize_delete();
            return;
        }
        if (observed.status == ImageDeleteStatus::Ok) {
            auto self = shared_from_this();
            rollback_and_cancel_marker([self](bool restored) {
                if (restored) {
                    self->fail_service(
                        "database commit was not confirmed; image was restored");
                }
                else self->finish_pending_recovery(
                    "database outcome needs recovery");
            });
            return;
        }
        LOG_ERROR("could not resolve image delete commit outcome for " + name_ +
            ": " + observed.message);
        finish_pending_recovery("delete accepted; commit outcome is being recovered");
    }

    void finalize_delete() {
        auto self = shared_from_this();
        ImageRedisService::instance().finalize_image_delete(
            name_, delete_token_,
            [self](RedisWriteResult result) {
                self->on_delete_finalized(std::move(result));
            }
        );
    }

    void on_delete_finalized(RedisWriteResult result) {
        if (!result.ok) {
            --finalize_attempts_remaining_;
            if (finalize_attempts_remaining_ > 0) {
                finalize_delete();
                return;
            }
            LOG_ERROR("finalize image deletion failed for " + name_ +
                ": " + result.error);
            finish_pending_recovery("image deleted; cache cleanup is pending");
            return;
        }
        if (!result.applied) {
            LOG_ERROR("image delete lifecycle token changed before finalize: " + name_);
            finish_pending_recovery("image deleted; lifecycle recovery is pending");
            return;
        }
        marker_created_ = false;
        std::filesystem::path thumbnail =
            std::filesystem::path(owner_->images_dir()) / "thumbnails" /
            (std::filesystem::path(name_).stem().string() + ".webp");
        std::error_code thumbnail_error;
        std::filesystem::remove(thumbnail, thumbnail_error);
        if (thumbnail_error) {
            LOG_ERROR("remove image thumbnail failed for " + name_ +
                ": " + thumbnail_error.message());
            finish_pending_recovery("image deleted; thumbnail cleanup is pending");
            return;
        }

        std::string discard_error;
        if (guard_ && !guard_->discard(discard_error)) {
            LOG_ERROR("discard quarantined image failed for " + name_ +
                ": " + discard_error);
            finish_pending_recovery("image deleted; file cleanup is pending");
            return;
        }
        finish(owner_->build_json_response(
            200,
            "OK",
            "{ \"ok\": true, \"message\": \"image deleted\" }\n",
            keep_alive_
        ));
    }

    bool rollback_guard() {
        if (guard_) {
            std::string rollback_error;
            if (!guard_->rollback(rollback_error)) {
                LOG_ERROR("rollback image deletion failed for " + name_ +
                    ": " + rollback_error);
                return false;
            }
        }
        return true;
    }

    void rollback_and_cancel_marker(std::function<void(bool)> completion) {
        if (!marker_created_) {
            completion(rollback_guard());
            return;
        }
        cancel_delete_marker(3, std::move(completion));
    }

    void cancel_delete_marker(
        int attempts_remaining,
        std::function<void(bool)> completion
    ) {
        auto self = shared_from_this();
        ImageRedisService::instance().cancel_image_delete(
            name_,
            delete_token_,
            [self, attempts_remaining,
             completion = std::move(completion)](
                RedisWriteResult result) mutable {
                if (!result.ok && attempts_remaining > 1) {
                    self->cancel_delete_marker(
                        attempts_remaining - 1, std::move(completion));
                    return;
                }
                if (!result.ok || !result.applied) {
                    LOG_ERROR("cancel image deletion marker failed for " +
                        self->name_ + ": " + (result.error.empty()
                            ? "lifecycle token changed" : result.error));
                    completion(false);
                    return;
                }
                self->marker_created_ = false;
                completion(self->rollback_guard());
            });
    }

    void finish_pending_recovery(const std::string& message) {
        if (guard_) {
            guard_->defer_to_recovery();
        }
        finish(owner_->build_raw_response(
            202,
            "Accepted",
            "application/json; charset=utf-8",
            "{ \"ok\": true, \"pending\": true, \"message\": \"" +
                owner_->json_escape(message) + "\" }\n",
            keep_alive_,
            "Retry-After: 2\r\n"
        ));
    }

    void finish_database_error(ImageDeleteResult result) {
        if (result.status == ImageDeleteStatus::Cancelled) {
            return;
        }
        int status = 500;
        std::string text = "Internal Server Error";
        std::string message = result.message;
        if (result.status == ImageDeleteStatus::NotFound) {
            status = 404;
            text = "Not Found";
        }
        else if (result.status == ImageDeleteStatus::Forbidden) {
            status = 403;
            text = "Forbidden";
        }
        else if (result.status == ImageDeleteStatus::Unavailable) {
            status = 503;
            text = "Service Unavailable";
            message = "image database is busy or unavailable";
        }
        else {
            LOG_ERROR("image delete database operation failed: " +
                result.message);
            message = "could not update image metadata";
        }
        finish(owner_->build_json_response(
            status,
            text,
            "{ \"ok\": false, \"message\": \"" +
                owner_->json_escape(message) + "\" }\n",
            keep_alive_
        ));
    }

    void fail_service(const std::string& message) {
        finish(owner_->build_json_response(
            503,
            "Service Unavailable",
            "{ \"ok\": false, \"message\": \"" +
                owner_->json_escape(message) + "\" }\n",
            keep_alive_
        ));
    }

    void finish(std::string response) {
        if (finished_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        callback_(std::move(response));
    }

    HttpResponse* owner_;
    HttpRequest request_;
    SessionInfo session_;
    bool keep_alive_;
    CompletionCallback callback_;
    Executor executor_;
    std::string name_;
    std::string delete_token_;
    std::unique_ptr<ImageDeleteGuard> guard_;
    int begin_attempts_remaining_{ 0 };
    int finalize_attempts_remaining_{ 0 };
    bool marker_created_{ false };
    std::atomic<bool> finished_{ false };
};

void HttpResponse::build_authenticated_response_async(
    const HttpRequest& request,
    const SessionInfo& session,
    bool keep_alive,
    CompletionCallback callback,
    Executor executor
) {
    std::string clean_path = path_without_query(request.path());
    if (request.method() == "GET" && clean_path == "/api/images") {
        build_images_response_async(
            keep_alive,
            std::move(callback),
            std::move(executor)
        );
        return;
    }
    if (request.method() == "GET" && clean_path == "/api/rankings") {
        build_rankings_response_async(
            request,
            keep_alive,
            std::move(callback)
        );
        return;
    }
    if (request.method() == "POST" && clean_path == "/api/image-view") {
        build_record_image_view_response_async(
            request,
            keep_alive,
            std::move(callback)
        );
        return;
    }
    if (request.method() == "GET" && clean_path == "/api/image-status") {
        build_image_status_response_async(
            request,
            keep_alive,
            std::move(callback)
        );
        return;
    }
    if (request.method() == "POST" && clean_path == "/api/upload-image") {
        build_upload_image_response_async(
            request,
            session,
            keep_alive,
            std::move(callback),
            std::move(executor)
        );
        return;
    }
    if (request.method() == "DELETE" && clean_path == "/api/images") {
        build_delete_image_response_async(
            request,
            session,
            keep_alive,
            std::move(callback),
            std::move(executor)
        );
        return;
    }

    CompletionCallback fallback = callback;
    bool scheduled = executor([
        this,
        request,
        session,
        keep_alive,
        callback = std::move(callback)
    ]() mutable {
        if (!request_cancelled()) {
            callback(build_authenticated_response(request, session, keep_alive));
        }
    });
    if (!scheduled) {
        fallback(build_json_response(
            503,
            "Service Unavailable",
            "{ \"ok\": false, \"message\": \"server is busy; retry later\" }\n",
            keep_alive
        ));
    }
}

void HttpResponse::build_images_response_async(
    bool keep_alive,
    CompletionCallback callback,
    Executor executor
) {
    ImageRedisService::instance().get_image_list([
        this,
        keep_alive,
        callback = std::move(callback),
        executor = std::move(executor)
    ](RedisCacheResult result) mutable {
        if (request_cancelled()) {
            return;
        }
        if (result.state == RedisCacheState::Hit) {
            Metrics::instance().inc_cache_hit();
            Metrics::instance().inc_redis_image_cache_hit();
            callback(build_json_response(200, "OK", result.value, keep_alive));
            return;
        }

        Metrics::instance().inc_cache_miss();
        Metrics::instance().inc_redis_image_cache_miss();
        if (result.state == RedisCacheState::Error) {
            LOG_WARN("image-list Redis cache read failed: " + result.error);
        }

        const std::string list_version = result.version;
        CompletionCallback fallback = callback;
        bool scheduled = executor([
            this,
            list_version,
            keep_alive,
            callback = std::move(callback)
        ]() mutable {
            if (request_cancelled()) {
                return;
            }
            std::string response = build_images_response(keep_alive);
            std::string body = http_response_body(response);
            if (!body.empty() && response.rfind("HTTP/1.1 200 OK\r\n", 0) == 0) {
                ImageRedisService::instance().put_image_list_if_version(
                    body,
                    list_version,
                    [](RedisWriteResult result) {
                        ignore_write_failure("fill image-list cache", std::move(result));
                    }
                );
            }
            callback(std::move(response));
        });
        if (!scheduled) {
            fallback(build_json_response(
                503,
                "Service Unavailable",
                "{ \"ok\": false, \"message\": \"server is busy; retry later\" }\n",
                keep_alive
            ));
        }
    });
}

void HttpResponse::build_upload_image_response_async(
    const HttpRequest& request,
    const SessionInfo& session,
    bool keep_alive,
    CompletionCallback callback,
    Executor executor
) {
    auto flow = std::make_shared<UploadRedisFlow>(
        this,
        request,
        session,
        keep_alive,
        std::move(callback),
        std::move(executor)
    );
    flow->start();
}

void HttpResponse::build_delete_image_response_async(
    const HttpRequest& request,
    const SessionInfo& session,
    bool keep_alive,
    CompletionCallback callback,
    Executor executor
) {
    auto flow = std::make_shared<DeleteRedisFlow>(
        this,
        request,
        session,
        keep_alive,
        std::move(callback),
        std::move(executor)
    );
    flow->start();
}
