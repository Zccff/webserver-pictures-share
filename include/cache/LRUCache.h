#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>

class LRUCache {
public:
    using Value = std::shared_ptr<const std::string>;

    explicit LRUCache(size_t max_bytes = 10 * 1024 * 1024);

    bool get(const std::string& key, Value& value);

    void put(const std::string& key, Value value);

private:
    static constexpr size_t SHARD_COUNT = 8;
    static constexpr uint32_t RECENCY_UPDATE_INTERVAL = 16;

    struct Entry;
    using EntryPtr = std::shared_ptr<Entry>;
    using RecencyList = std::list<EntryPtr>;

    struct Entry {
        Entry(std::string entry_key, Value entry_value)
            : key(std::move(entry_key)), value(std::move(entry_value)) {
        }

        std::string key;
        Value value;
        std::atomic<uint32_t> access_count{ 0 };
        RecencyList::iterator position;
    };

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, EntryPtr> map;
        RecencyList recency;
        size_t current_bytes{ 0 };
        size_t max_bytes{ 0 };
    };

    Shard& shard_for(const std::string& key);

    static void promote_if_current(
        Shard& shard,
        const std::string& key,
        const EntryPtr& entry
    );

    static void evict_if_needed(Shard& shard);

private:
    std::array<Shard, SHARD_COUNT> shards_;
};
