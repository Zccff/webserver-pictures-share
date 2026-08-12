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
};