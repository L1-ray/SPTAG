// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <array>

#include "inc/Core/Common.h"

namespace SPTAG
{
namespace SPANN
{

// Page cache key: combines file identity and page ID
struct PageCacheKey
{
    uint32_t m_fileId;      // Which index file
    uint32_t m_pageId;      // Page number within file
    uint32_t m_pageSize;    // Page size (typically 4096)

    PageCacheKey() : m_fileId(0), m_pageId(0), m_pageSize(0)
    {
    }

    PageCacheKey(uint32_t fileId, uint32_t pageId, uint32_t pageSize)
        : m_fileId(fileId), m_pageId(pageId), m_pageSize(pageSize)
    {
    }

    bool operator==(const PageCacheKey& other) const
    {
        return m_fileId == other.m_fileId && m_pageId == other.m_pageId && m_pageSize == other.m_pageSize;
    }

    struct Hash
    {
        size_t operator()(const PageCacheKey& k) const
        {
            return static_cast<size_t>(k.m_fileId) ^ (static_cast<size_t>(k.m_pageId) << 16) ^
                   (static_cast<size_t>(k.m_pageSize) << 24);
        }
    };
};

// Cached page entry
struct PageCacheEntry
{
    std::vector<char> m_data;       // Page data
    std::chrono::steady_clock::time_point m_lastAccess;
    uint64_t m_hitCount;
    uint32_t m_pageSize;

    PageCacheEntry() : m_hitCount(0), m_pageSize(0)
    {
    }

    PageCacheEntry(uint32_t pageSize) : m_hitCount(1), m_pageSize(pageSize)
    {
        m_data.resize(pageSize);
        m_lastAccess = std::chrono::steady_clock::now();
    }

    size_t Size() const
    {
        return m_data.size();
    }
};

// Async insert request for background thread
struct AsyncInsertRequest
{
    PageCacheKey m_key;
    std::vector<char> m_data;

    AsyncInsertRequest() = default;
    AsyncInsertRequest(const PageCacheKey& key, const char* data, uint32_t size)
        : m_key(key)
    {
        m_data.resize(size);
        std::memcpy(m_data.data(), data, size);
    }
};

// In-flight request for coalescing
struct InFlightRequest
{
    PageCacheKey m_key;
    std::vector<std::function<void(bool, const char*)>> m_waiters; // Callbacks waiting for this page
    char* m_buffer;                 // Buffer for the read
    bool m_completed;
    bool m_success;

    InFlightRequest() : m_buffer(nullptr), m_completed(false), m_success(false)
    {
    }
};

// In-flight coalescing manager (lightweight, no cache storage)
class InFlightCoalescer
{
public:
    struct Stats
    {
        std::atomic<uint64_t> m_coalescedReads{0};
        std::atomic<uint64_t> m_totalRequests{0};
    };

    InFlightCoalescer() = default;
    ~InFlightCoalescer() = default;

    // Check if a request is in-flight and register a waiter
    // Returns true if coalesced (caller should not issue read)
    // Returns false if this is the first request (caller should issue read)
    bool TryCoalesce(const PageCacheKey& key, std::function<void(bool, const char*)> callback);

    // Mark an in-flight request as complete
    void CompleteInFlight(const PageCacheKey& key, bool success, const char* data);

    // Check if request is currently in-flight
    bool IsInFlight(const PageCacheKey& key) const;

    // Get statistics
    const Stats& GetStats() const { return m_stats; }

private:
    mutable std::mutex m_mutex;
    std::unordered_map<PageCacheKey, std::shared_ptr<InFlightRequest>, PageCacheKey::Hash> m_inFlight;
    Stats m_stats;
};

// Global singleton accessor for coalescer (independent of cache)
InFlightCoalescer* GetGlobalCoalescer();
void InitGlobalCoalescer();
void ShutdownGlobalCoalescer();

// Global page cache with in-flight coalescing (cache storage + coalescing)
class GlobalPageCache
{
public:
    struct Stats
    {
        std::atomic<uint64_t> m_hits{0};
        std::atomic<uint64_t> m_misses{0};
        std::atomic<uint64_t> m_evictions{0};
        std::atomic<uint64_t> m_coalescedReads{0};
        std::atomic<uint64_t> m_bytesRead{0};
        std::atomic<uint64_t> m_bytesServed{0};
        std::atomic<uint64_t> m_inFlightWaits{0};
        std::atomic<uint64_t> m_cacheLockWaitNs{0};
        std::atomic<uint64_t> m_asyncInsertQueued{0};
        std::atomic<uint64_t> m_asyncInsertDropped{0};
        std::atomic<uint64_t> m_accessCountFiltered{0};  // Filtered by access count
        std::atomic<uint64_t> m_savedIoPages{0};         // B4: Pages saved by cache hit

        double HitRate() const
        {
            uint64_t total = m_hits + m_misses;
            return total > 0 ? static_cast<double>(m_hits) / total : 0.0;
        }
    };

    GlobalPageCache(size_t maxBytes = 256 * 1024 * 1024); // Default 256MB
    ~GlobalPageCache();

    // Try to get a page from cache
    // Returns true if hit, false if miss
    // If hit, copies data to buffer and increments hit count
    // lockWaitNs (optional): returns lock wait time in nanoseconds
    bool TryGet(const PageCacheKey& key, char* buffer, uint32_t* bytesCopied, uint64_t* lockWaitNs = nullptr);

    // Record access for admission control (called before I/O)
    // Returns true if this posting should be cached after I/O
    bool ShouldCache(const PageCacheKey& key);

    // Insert a page into cache (synchronous, for internal use)
    void Insert(const PageCacheKey& key, const char* data, uint32_t size);

    // Async insert: enqueue for background thread (non-blocking)
    // Returns true if queued, false if queue full (dropped)
    bool AsyncInsert(const PageCacheKey& key, const char* data, uint32_t size);

    // Check if a request is in-flight and register a waiter
    // Returns true if coalesced (caller should not issue read)
    // Returns false if this is the first request (caller should issue read)
    bool TryCoalesce(const PageCacheKey& key, std::function<void(bool, const char*)> callback);

    // Mark an in-flight request as complete
    void CompleteInFlight(const PageCacheKey& key, bool success, const char* data);

    // Check if request is currently in-flight
    bool IsInFlight(const PageCacheKey& key) const;

    // Get statistics
    const Stats& GetStats() const
    {
        return m_stats;
    }

    // Clear cache
    void Clear();

    // Resize cache
    void SetMaxSize(size_t maxBytes);

    // Get current size
    size_t CurrentSize() const
    {
        return m_currentBytes;
    }

    // Shutdown background thread
    void Shutdown();

private:
    void EvictIfNeeded(size_t neededBytes);
    void UpdateLRU(const PageCacheKey& key);
    void BackgroundThreadFunc();

    mutable std::shared_mutex m_cacheMutex;
    std::unordered_map<PageCacheKey, std::shared_ptr<PageCacheEntry>, PageCacheKey::Hash> m_cache;
    std::list<PageCacheKey> m_lruList; // Most recently used at front
    std::unordered_map<PageCacheKey, std::list<PageCacheKey>::iterator, PageCacheKey::Hash> m_lruMap;

    mutable std::mutex m_inFlightMutex;
    std::unordered_map<PageCacheKey, std::shared_ptr<InFlightRequest>, PageCacheKey::Hash> m_inFlight;

    // Access count for admission control
    mutable std::mutex m_accessCountMutex;
    std::unordered_map<PageCacheKey, uint32_t, PageCacheKey::Hash> m_accessCount;
    static constexpr uint32_t kMinAccessCountToCache = 2;  // Only cache after 2 accesses

    // Async insert queue
    std::mutex m_asyncQueueMutex;
    std::condition_variable m_asyncQueueCv;
    std::vector<AsyncInsertRequest> m_asyncQueue;
    std::thread m_backgroundThread;
    bool m_shutdown;
    static constexpr size_t kMaxAsyncQueueSize = 100000;  // Increased from 10000

    size_t m_maxBytes;
    size_t m_currentBytes;
    Stats m_stats;

    // Configuration
    bool m_enableCache;
    bool m_enableCoalescing;
};

// ShardedPageCache - Reduces lock contention by sharding the cache
// Each shard has its own lock, reducing contention from N threads to ~N/kShardCount
class ShardedPageCache
{
public:
    struct Stats
    {
        std::atomic<uint64_t> m_hits{0};
        std::atomic<uint64_t> m_misses{0};
        std::atomic<uint64_t> m_evictions{0};
        std::atomic<uint64_t> m_bytesServed{0};
        std::atomic<uint64_t> m_cacheLockWaitNs{0};
        std::atomic<uint64_t> m_asyncInsertQueued{0};
        std::atomic<uint64_t> m_asyncInsertDropped{0};
        std::atomic<uint64_t> m_savedIoPages{0};

        double HitRate() const
        {
            uint64_t total = m_hits + m_misses;
            return total > 0 ? static_cast<double>(m_hits) / total : 0.0;
        }
    };

    static constexpr size_t kShardCount = 16;  // 16 shards for 8-16 threads

    ShardedPageCache(size_t maxBytes = 256 * 1024 * 1024);
    ~ShardedPageCache();

    // Try to get a page from cache
    // Returns true if hit, false if miss
    bool TryGet(const PageCacheKey& key, char* buffer, uint32_t* bytesCopied, uint64_t* lockWaitNs = nullptr);

    // Insert a page into cache (synchronous)
    void Insert(const PageCacheKey& key, const char* data, uint32_t size);

    // Async insert: enqueue for background thread
    bool AsyncInsert(const PageCacheKey& key, const char* data, uint32_t size);

    // Get statistics
    const Stats& GetStats() const { return m_stats; }

    // Clear all shards
    void Clear();

    // Shutdown background thread
    void Shutdown();

    // Get current total size
    size_t CurrentSize() const { return m_currentBytes.load(); }

private:
    struct Shard
    {
        mutable std::shared_mutex mutex;
        std::unordered_map<PageCacheKey, std::shared_ptr<PageCacheEntry>, PageCacheKey::Hash> cache;
        std::list<PageCacheKey> lruList;
        std::unordered_map<PageCacheKey, std::list<PageCacheKey>::iterator, PageCacheKey::Hash> lruMap;
        size_t currentBytes = 0;
    };

    size_t GetShard(const PageCacheKey& key) const;
    void EvictIfNeeded(Shard& shard, size_t neededBytes);
    void BackgroundThreadFunc();

    std::array<Shard, kShardCount> m_shards;
    std::atomic<size_t> m_currentBytes{0};
    size_t m_maxBytes;

    // Async insert queue
    std::mutex m_asyncQueueMutex;
    std::condition_variable m_asyncQueueCv;
    std::vector<AsyncInsertRequest> m_asyncQueue;
    std::thread m_backgroundThread;
    std::atomic<bool> m_shutdown{false};
    static constexpr size_t kMaxAsyncQueueSize = 100000;

    Stats m_stats;
};

// Global singleton accessor for sharded cache
ShardedPageCache* GetGlobalShardedPageCache();
void InitGlobalShardedPageCache(size_t maxBytes = 256 * 1024 * 1024);
void ShutdownGlobalShardedPageCache();

// Global singleton accessor for cache (with integrated coalescing)
GlobalPageCache* GetGlobalPageCache();
void InitGlobalPageCache(size_t maxBytes = 256 * 1024 * 1024);
void ShutdownGlobalPageCache();

} // namespace SPANN
} // namespace SPTAG
