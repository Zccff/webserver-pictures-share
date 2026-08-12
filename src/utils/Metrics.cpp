#include "utils/Metrics.h"

#include <iomanip>
#include <sstream>

Metrics& Metrics::instance() {
    static Metrics metrics;
    return metrics;
}

void Metrics::inc_active_connections() {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::dec_active_connections() {
    active_connections_.fetch_sub(1, std::memory_order_relaxed);
}

void Metrics::inc_total_connections() {
    total_connections_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::inc_total_requests() {
    total_requests_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::inc_cache_hit() {
    cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::inc_cache_miss() {
    cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Metrics::active_connections() const {
    return active_connections_.load(std::memory_order_relaxed);
}

uint64_t Metrics::total_connections() const {
    return total_connections_.load(std::memory_order_relaxed);
}

uint64_t Metrics::total_requests() const {
    return total_requests_.load(std::memory_order_relaxed);
}

uint64_t Metrics::cache_hits() const {
    return cache_hits_.load(std::memory_order_relaxed);
}

uint64_t Metrics::cache_misses() const {
    return cache_misses_.load(std::memory_order_relaxed);
}

std::string Metrics::to_json(size_t thread_pool_queue_size) const {
    uint64_t hits = cache_hits();
    uint64_t misses = cache_misses();
    uint64_t total_cache = hits + misses;

    double hit_rate = total_cache == 0
        ? 0.0
        : static_cast<double>(hits) / static_cast<double>(total_cache);

    std::ostringstream oss;

    oss << "{\n";
    oss << "  \"active_connections\": " << active_connections() << ",\n";
    oss << "  \"total_connections\": " << total_connections() << ",\n";
    oss << "  \"total_requests\": " << total_requests() << ",\n";
    oss << "  \"thread_pool_queue_size\": " << thread_pool_queue_size << ",\n";
    oss << "  \"cache_hits\": " << hits << ",\n";
    oss << "  \"cache_misses\": " << misses << ",\n";
    oss << "  \"cache_hit_rate\": " << std::fixed << std::setprecision(4) << hit_rate << "\n";
    oss << "}\n";

    return oss.str();
}