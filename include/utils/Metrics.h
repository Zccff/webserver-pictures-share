#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class Metrics {
public:
    static Metrics& instance();

    void inc_active_connections();
    void dec_active_connections();

    void inc_total_connections();
    void inc_total_requests();

    void inc_cache_hit();
    void inc_cache_miss();

    void inc_rejected_connection();
    void inc_rejected_worker_task();
    void inc_dropped_log();
    void inc_rejected_redis_command();
    void inc_rate_limited_upload();
    void inc_duplicate_upload();
    void inc_redis_image_cache_hit();
    void inc_redis_image_cache_miss();
    void inc_upload_rate_rejection();
    void inc_upload_permit_rejection();
    void inc_idempotency_replay();
    void inc_rejected_image_processing_task();
    void inc_thumbnail_processed();
    void inc_thumbnail_failed();

    uint64_t active_connections() const;
    uint64_t total_connections() const;
    uint64_t total_requests() const;
    uint64_t cache_hits() const;
    uint64_t cache_misses() const;

    std::string to_json(size_t thread_pool_queue_size) const;

private:
    Metrics() = default;

private:
    std::atomic<uint64_t> active_connections_{ 0 };
    std::atomic<uint64_t> total_connections_{ 0 };
    std::atomic<uint64_t> total_requests_{ 0 };
    std::atomic<uint64_t> cache_hits_{ 0 };
    std::atomic<uint64_t> cache_misses_{ 0 };
    std::atomic<uint64_t> rejected_connections_{ 0 };
    std::atomic<uint64_t> rejected_worker_tasks_{ 0 };
    std::atomic<uint64_t> dropped_logs_{ 0 };
    std::atomic<uint64_t> rejected_redis_commands_{ 0 };
    std::atomic<uint64_t> rate_limited_uploads_{ 0 };
    std::atomic<uint64_t> duplicate_uploads_{ 0 };
    std::atomic<uint64_t> redis_image_cache_hits_{ 0 };
    std::atomic<uint64_t> redis_image_cache_misses_{ 0 };
    std::atomic<uint64_t> upload_rate_rejections_{ 0 };
    std::atomic<uint64_t> upload_permit_rejections_{ 0 };
    std::atomic<uint64_t> idempotency_replays_{ 0 };
    std::atomic<uint64_t> rejected_image_processing_tasks_{ 0 };
    std::atomic<uint64_t> thumbnails_processed_{ 0 };
    std::atomic<uint64_t> thumbnails_failed_{ 0 };
};
