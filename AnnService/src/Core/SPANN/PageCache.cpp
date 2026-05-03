// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Core/SPANN/PageCache.h"

#include <algorithm>
#include <cstring>

namespace SPTAG
{
namespace SPANN
{

//=============================================================================
// InFlightCoalescer - Lightweight coalescing without cache storage
//=============================================================================

static InFlightCoalescer* g_coalescer = nullptr;
static std::mutex g_coalescerMutex;

InFlightCoalescer* GetGlobalCoalescer()
{
    return g_coalescer;
}

void InitGlobalCoalescer()
{
    std::lock_guard<std::mutex> lock(g_coalescerMutex);
    if (g_coalescer == nullptr)
    {
        g_coalescer = new InFlightCoalescer();
    }
}

void ShutdownGlobalCoalescer()
{
    std::lock_guard<std::mutex> lock(g_coalescerMutex);
    delete g_coalescer;
    g_coalescer = nullptr;
}

bool InFlightCoalescer::TryCoalesce(const PageCacheKey& key, std::function<void(bool, const char*)> callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats.m_totalRequests++;

    auto it = m_inFlight.find(key);
    if (it != m_inFlight.end())
    {
        // Already in-flight, add waiter
        it->second->m_waiters.push_back(callback);
        m_stats.m_coalescedReads++;
        return true;
    }

    // Create new in-flight request
    auto req = std::make_shared<InFlightRequest>();
    req->m_key = key;
    req->m_waiters.push_back(callback);
    m_inFlight[key] = req;
    return false;
}

void InFlightCoalescer::CompleteInFlight(const PageCacheKey& key, bool success, const char* data)
{
    std::shared_ptr<InFlightRequest> req;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_inFlight.find(key);
        if (it == m_inFlight.end())
        {
            return;
        }
        req = it->second;
        m_inFlight.erase(it);
    }

    // Notify all waiters
    req->m_completed = true;
    req->m_success = success;

    // Call all waiters (including the original one)
    for (auto& waiter : req->m_waiters)
    {
        waiter(success, data);
    }
}

bool InFlightCoalescer::IsInFlight(const PageCacheKey& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_inFlight.find(key) != m_inFlight.end();
}

//=============================================================================
// GlobalPageCache - Cache storage with integrated coalescing
//=============================================================================

static GlobalPageCache* g_pageCache = nullptr;
static std::mutex g_pageCacheMutex;

GlobalPageCache* GetGlobalPageCache()
{
    return g_pageCache;
}

void InitGlobalPageCache(size_t maxBytes)
{
    std::lock_guard<std::mutex> lock(g_pageCacheMutex);
    if (g_pageCache == nullptr)
    {
        g_pageCache = new GlobalPageCache(maxBytes);
    }
}

void ShutdownGlobalPageCache()
{
    std::lock_guard<std::mutex> lock(g_pageCacheMutex);
    delete g_pageCache;
    g_pageCache = nullptr;
}

GlobalPageCache::GlobalPageCache(size_t maxBytes)
    : m_maxBytes(maxBytes)
    , m_currentBytes(0)
    , m_shutdown(false)
    , m_enableCache(true)
    , m_enableCoalescing(true)
{
    // Start background thread for async inserts
    m_backgroundThread = std::thread(&GlobalPageCache::BackgroundThreadFunc, this);
}

GlobalPageCache::~GlobalPageCache()
{
    Shutdown();
    Clear();
}

void GlobalPageCache::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_asyncQueueMutex);
        m_shutdown = true;
    }
    m_asyncQueueCv.notify_all();
    if (m_backgroundThread.joinable())
    {
        m_backgroundThread.join();
    }
}

bool GlobalPageCache::AsyncInsert(const PageCacheKey& key, const char* data, uint32_t size)
{
    if (!m_enableCache || size == 0)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_asyncQueueMutex);
        if (m_asyncQueue.size() >= kMaxAsyncQueueSize)
        {
            m_stats.m_asyncInsertDropped++;
            return false;
        }

        m_asyncQueue.emplace_back(key, data, size);
        m_stats.m_asyncInsertQueued++;
    }

    // Notify background thread to process
    m_asyncQueueCv.notify_one();
    return true;
}

void GlobalPageCache::BackgroundThreadFunc()
{
    while (true)
    {
        std::vector<AsyncInsertRequest> batch;
        {
            std::unique_lock<std::mutex> lock(m_asyncQueueMutex);
            m_asyncQueueCv.wait(lock, [this] { return m_shutdown || !m_asyncQueue.empty(); });

            if (m_shutdown && m_asyncQueue.empty())
            {
                return;
            }

            // Swap out the queue to process without holding lock
            batch.swap(m_asyncQueue);
        }

        // Process batch outside lock
        for (const auto& req : batch)
        {
            Insert(req.m_key, req.m_data.data(), static_cast<uint32_t>(req.m_data.size()));
        }
    }
}

bool GlobalPageCache::TryGet(const PageCacheKey& key, char* buffer, uint32_t* bytesCopied, uint64_t* lockWaitNs)
{
    if (!m_enableCache)
    {
        return false;
    }

    auto startTime = std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> lock(m_cacheMutex);

    auto duration = std::chrono::steady_clock::now() - startTime;
    uint64_t waitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    m_stats.m_cacheLockWaitNs += waitNs;

    if (lockWaitNs)
    {
        *lockWaitNs = waitNs;
    }

    auto it = m_cache.find(key);
    if (it == m_cache.end())
    {
        m_stats.m_misses++;
        return false;
    }

    // Hit - copy data
    const auto& entry = it->second;
    uint32_t copySize = std::min(static_cast<uint32_t>(entry->m_data.size()), key.m_pageSize);
    std::memcpy(buffer, entry->m_data.data(), copySize);
    if (bytesCopied)
    {
        *bytesCopied = copySize;
    }

    // Update stats
    m_stats.m_hits++;
    m_stats.m_bytesServed += copySize;
    m_stats.m_savedIoPages++;  // B4: Each hit saves one I/O page

    // M1: Skip LRU update on hit to avoid lock upgrade overhead
    // LRU update only happens on eviction which is rare for async insert
    // lock.unlock();
    // {
    //     std::unique_lock<std::shared_mutex> writeLock(m_cacheMutex);
    //     UpdateLRU(key);
    // }

    return true;
}

bool GlobalPageCache::ShouldCache(const PageCacheKey& key)
{
    if (!m_enableCache)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_accessCountMutex);
    auto it = m_accessCount.find(key);
    if (it == m_accessCount.end())
    {
        // First access - record it
        m_accessCount[key] = 1;
        m_stats.m_accessCountFiltered++;
        return false;
    }

    it->second++;
    if (it->second >= kMinAccessCountToCache)
    {
        return true;  // Hot posting, should cache
    }

    m_stats.m_accessCountFiltered++;
    return false;
}

void GlobalPageCache::Insert(const PageCacheKey& key, const char* data, uint32_t size)
{
    if (!m_enableCache || size == 0)
    {
        return;
    }

    auto startTime = std::chrono::steady_clock::now();

    std::unique_lock<std::shared_mutex> lock(m_cacheMutex);

    auto duration = std::chrono::steady_clock::now() - startTime;
    m_stats.m_cacheLockWaitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    // Check if already exists
    if (m_cache.find(key) != m_cache.end())
    {
        return;
    }

    // Evict if needed
    EvictIfNeeded(size);

    // Create new entry
    auto entry = std::make_shared<PageCacheEntry>(size);
    std::memcpy(entry->m_data.data(), data, size);

    m_cache[key] = entry;
    m_currentBytes += size;

    // Add to LRU
    m_lruList.push_front(key);
    m_lruMap[key] = m_lruList.begin();
}

bool GlobalPageCache::TryCoalesce(const PageCacheKey& key, std::function<void(bool, const char*)> callback)
{
    if (!m_enableCoalescing)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_inFlightMutex);

    auto it = m_inFlight.find(key);
    if (it != m_inFlight.end())
    {
        // Already in-flight, add waiter
        it->second->m_waiters.push_back(callback);
        m_stats.m_coalescedReads++;
        return true;
    }

    // Create new in-flight request
    auto req = std::make_shared<InFlightRequest>();
    req->m_key = key;
    req->m_waiters.push_back(callback);
    m_inFlight[key] = req;
    return false;
}

void GlobalPageCache::CompleteInFlight(const PageCacheKey& key, bool success, const char* data)
{
    std::shared_ptr<InFlightRequest> req;

    {
        std::lock_guard<std::mutex> lock(m_inFlightMutex);
        auto it = m_inFlight.find(key);
        if (it == m_inFlight.end())
        {
            return;
        }
        req = it->second;
        m_inFlight.erase(it);
    }

    // Notify all waiters
    req->m_completed = true;
    req->m_success = success;

    // Insert into cache if successful
    if (success && data && m_enableCache)
    {
        Insert(key, data, key.m_pageSize);
    }

    // Call all waiters (including the original one)
    for (auto& waiter : req->m_waiters)
    {
        waiter(success, data);
    }
}

bool GlobalPageCache::IsInFlight(const PageCacheKey& key) const
{
    std::lock_guard<std::mutex> lock(m_inFlightMutex);
    return m_inFlight.find(key) != m_inFlight.end();
}

void GlobalPageCache::Clear()
{
    std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
    m_cache.clear();
    m_lruList.clear();
    m_lruMap.clear();
    m_currentBytes = 0;
}

void GlobalPageCache::SetMaxSize(size_t maxBytes)
{
    std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
    m_maxBytes = maxBytes;
    EvictIfNeeded(0);
}

void GlobalPageCache::EvictIfNeeded(size_t neededBytes)
{
    // Must be called with exclusive lock held
    while (!m_lruList.empty() && m_currentBytes + neededBytes > m_maxBytes)
    {
        // Evict least recently used (back of list)
        PageCacheKey key = m_lruList.back();
        auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            m_currentBytes -= it->second->Size();
            m_cache.erase(it);
            m_stats.m_evictions++;
        }
        m_lruMap.erase(key);
        m_lruList.pop_back();
    }
}

void GlobalPageCache::UpdateLRU(const PageCacheKey& key)
{
    // Must be called with exclusive lock held
    auto it = m_lruMap.find(key);
    if (it != m_lruMap.end())
    {
        // Move to front
        m_lruList.erase(it->second);
        m_lruList.push_front(key);
        m_lruMap[key] = m_lruList.begin();
    }
}

//=============================================================================
// ShardedPageCache - Reduces lock contention by sharding
//=============================================================================

static ShardedPageCache* g_shardedPageCache = nullptr;
static std::mutex g_shardedPageCacheMutex;

ShardedPageCache* GetGlobalShardedPageCache()
{
    return g_shardedPageCache;
}

void InitGlobalShardedPageCache(size_t maxBytes)
{
    std::lock_guard<std::mutex> lock(g_shardedPageCacheMutex);
    if (g_shardedPageCache == nullptr)
    {
        g_shardedPageCache = new ShardedPageCache(maxBytes);
    }
}

void ShutdownGlobalShardedPageCache()
{
    std::lock_guard<std::mutex> lock(g_shardedPageCacheMutex);
    delete g_shardedPageCache;
    g_shardedPageCache = nullptr;
}

ShardedPageCache::ShardedPageCache(size_t maxBytes)
    : m_maxBytes(maxBytes)
{
    // Start background thread for async inserts
    m_backgroundThread = std::thread(&ShardedPageCache::BackgroundThreadFunc, this);
}

ShardedPageCache::~ShardedPageCache()
{
    Shutdown();
    Clear();
}

void ShardedPageCache::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_asyncQueueMutex);
        m_shutdown = true;
    }
    m_asyncQueueCv.notify_all();
    if (m_backgroundThread.joinable())
    {
        m_backgroundThread.join();
    }
}

size_t ShardedPageCache::GetShard(const PageCacheKey& key) const
{
    // Simple hash: XOR file ID and page ID, then mod shard count
    return static_cast<size_t>(key.m_fileId ^ key.m_pageId) % kShardCount;
}

bool ShardedPageCache::TryGet(const PageCacheKey& key, char* buffer, uint32_t* bytesCopied, uint64_t* lockWaitNs)
{
    size_t shardIdx = GetShard(key);
    Shard& shard = m_shards[shardIdx];

    auto startTime = std::chrono::steady_clock::now();

    std::shared_lock<std::shared_mutex> lock(shard.mutex);

    auto duration = std::chrono::steady_clock::now() - startTime;
    uint64_t waitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    m_stats.m_cacheLockWaitNs += waitNs;

    if (lockWaitNs)
    {
        *lockWaitNs = waitNs;
    }

    auto it = shard.cache.find(key);
    if (it == shard.cache.end())
    {
        m_stats.m_misses++;
        return false;
    }

    // Hit - copy data
    const auto& entry = it->second;
    uint32_t copySize = std::min(static_cast<uint32_t>(entry->m_data.size()), key.m_pageSize);
    std::memcpy(buffer, entry->m_data.data(), copySize);
    if (bytesCopied)
    {
        *bytesCopied = copySize;
    }

    m_stats.m_hits++;
    m_stats.m_bytesServed += copySize;
    m_stats.m_savedIoPages++;

    return true;
}

void ShardedPageCache::Insert(const PageCacheKey& key, const char* data, uint32_t size)
{
    size_t shardIdx = GetShard(key);
    Shard& shard = m_shards[shardIdx];

    auto startTime = std::chrono::steady_clock::now();

    std::unique_lock<std::shared_mutex> lock(shard.mutex);

    auto duration = std::chrono::steady_clock::now() - startTime;
    m_stats.m_cacheLockWaitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    // Check if already exists
    if (shard.cache.find(key) != shard.cache.end())
    {
        return;
    }

    // Evict if needed
    EvictIfNeeded(shard, size);

    // Create new entry
    auto entry = std::make_shared<PageCacheEntry>(size);
    std::memcpy(entry->m_data.data(), data, size);

    shard.cache[key] = entry;
    shard.currentBytes += size;
    m_currentBytes += size;

    // Add to LRU
    shard.lruList.push_front(key);
    shard.lruMap[key] = shard.lruList.begin();
}

bool ShardedPageCache::AsyncInsert(const PageCacheKey& key, const char* data, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_asyncQueueMutex);
        if (m_asyncQueue.size() >= kMaxAsyncQueueSize)
        {
            m_stats.m_asyncInsertDropped++;
            return false;
        }

        m_asyncQueue.emplace_back(key, data, size);
        m_stats.m_asyncInsertQueued++;
    }

    m_asyncQueueCv.notify_one();
    return true;
}

void ShardedPageCache::BackgroundThreadFunc()
{
    while (true)
    {
        std::vector<AsyncInsertRequest> batch;
        {
            std::unique_lock<std::mutex> lock(m_asyncQueueMutex);
            m_asyncQueueCv.wait(lock, [this] { return m_shutdown || !m_asyncQueue.empty(); });

            if (m_shutdown && m_asyncQueue.empty())
            {
                return;
            }

            batch.swap(m_asyncQueue);
        }

        for (const auto& req : batch)
        {
            Insert(req.m_key, req.m_data.data(), static_cast<uint32_t>(req.m_data.size()));
        }
    }
}

void ShardedPageCache::Clear()
{
    for (size_t i = 0; i < kShardCount; i++)
    {
        std::unique_lock<std::shared_mutex> lock(m_shards[i].mutex);
        m_shards[i].cache.clear();
        m_shards[i].lruList.clear();
        m_shards[i].lruMap.clear();
        m_shards[i].currentBytes = 0;
    }
    m_currentBytes = 0;
}

void ShardedPageCache::EvictIfNeeded(Shard& shard, size_t neededBytes)
{
    // Evict from this shard if total cache would exceed limit
    // Use approximate check to avoid locking all shards
    while (!shard.lruList.empty() && m_currentBytes.load() + neededBytes > m_maxBytes)
    {
        PageCacheKey key = shard.lruList.back();
        auto it = shard.cache.find(key);
        if (it != shard.cache.end())
        {
            shard.currentBytes -= it->second->Size();
            m_currentBytes -= it->second->Size();
            shard.cache.erase(it);
            m_stats.m_evictions++;
        }
        shard.lruMap.erase(key);
        shard.lruList.pop_back();
    }
}

} // namespace SPANN
} // namespace SPTAG
