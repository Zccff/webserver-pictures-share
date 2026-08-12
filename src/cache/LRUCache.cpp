#include "cache/LRUCache.h"

#include <functional>
#include <mutex>
#include <utility>

LRUCache::LRUCache(size_t max_bytes) {
    const size_t base = max_bytes / SHARD_COUNT;
    const size_t remainder = max_bytes % SHARD_COUNT;

    for (size_t i = 0; i < SHARD_COUNT; ++i) {
        shards_[i].max_bytes = base + (i < remainder ? 1 : 0);
    }
}

bool LRUCache::get(const std::string& key, Value& value) {
    Shard& shard = shard_for(key);
    EntryPtr entry;

    {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        auto it = shard.map.find(key);

        if (it == shard.map.end()) {
            return false;
        }

        entry = it->second;
        value = entry->value;
    }

    uint32_t accesses = entry->access_count.fetch_add(
        1,
        std::memory_order_relaxed
    ) + 1;

    if (accesses % RECENCY_UPDATE_INTERVAL == 0) {
        promote_if_current(shard, key, entry);
    }

    return true;
}

void LRUCache::put(const std::string& key, Value value) {
    if (!value) {
        return;
    }

    Shard& shard = shard_for(key);

    if (value->size() > shard.max_bytes) {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.map.find(key);

    if (it != shard.map.end()) {
        shard.current_bytes -= it->second->value->size();
        it->second->value = std::move(value);
        it->second->access_count.store(0, std::memory_order_relaxed);
        shard.current_bytes += it->second->value->size();
        shard.recency.splice(
            shard.recency.begin(),
            shard.recency,
            it->second->position
        );
    }
    else {
        auto entry = std::make_shared<Entry>(key, std::move(value));
        shard.recency.push_front(entry);
        entry->position = shard.recency.begin();

        shard.current_bytes += entry->value->size();
        shard.map.emplace(key, std::move(entry));
    }

    evict_if_needed(shard);
}

LRUCache::Shard& LRUCache::shard_for(const std::string& key) {
    size_t index = std::hash<std::string>{}(key) % SHARD_COUNT;
    return shards_[index];
}

void LRUCache::promote_if_current(
    Shard& shard,
    const std::string& key,
    const EntryPtr& entry
) {
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.map.find(key);

    if (it == shard.map.end() || it->second != entry) {
        return;
    }

    shard.recency.splice(
        shard.recency.begin(),
        shard.recency,
        entry->position
    );
}

void LRUCache::evict_if_needed(Shard& shard) {
    while (shard.current_bytes > shard.max_bytes && !shard.recency.empty()) {
        const auto& entry = shard.recency.back();
        shard.current_bytes -= entry->value->size();
        shard.map.erase(entry->key);
        shard.recency.pop_back();
    }
}
