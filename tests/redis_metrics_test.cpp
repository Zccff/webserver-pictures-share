#include "utils/Metrics.h"

#include <iostream>
#include <string>

namespace {

bool contains_counter(
    const std::string& json,
    const std::string& name,
    unsigned long long value
) {
    return json.find(
        "\"" + name + "\": " + std::to_string(value)
    ) != std::string::npos;
}

}

int main() {
    Metrics& metrics = Metrics::instance();
    metrics.inc_redis_image_cache_hit();
    metrics.inc_redis_image_cache_miss();
    metrics.inc_upload_rate_rejection();
    metrics.inc_upload_permit_rejection();
    metrics.inc_idempotency_replay();
    metrics.inc_rejected_image_processing_task();
    metrics.inc_thumbnail_processed();
    metrics.inc_thumbnail_failed();

    const std::string json = metrics.to_json(7);
    if (!contains_counter(json, "redis_image_cache_hits", 1) ||
        !contains_counter(json, "redis_image_cache_misses", 1) ||
        !contains_counter(json, "upload_rate_rejections", 1) ||
        !contains_counter(json, "upload_permit_rejections", 1) ||
        !contains_counter(json, "idempotency_replays", 1) ||
        !contains_counter(json, "rejected_image_processing_tasks", 1) ||
        !contains_counter(json, "thumbnails_processed", 1) ||
        !contains_counter(json, "thumbnails_failed", 1)) {
        std::cerr << json;
        return 1;
    }

    return 0;
}
