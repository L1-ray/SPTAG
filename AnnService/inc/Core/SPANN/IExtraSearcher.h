// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _SPTAG_SPANN_IEXTRASEARCHER_H_
#define _SPTAG_SPANN_IEXTRASEARCHER_H_

#include "Options.h"

#include "inc/Core/Common/VersionLabel.h"
#include "inc/Core/VectorIndex.h"
#include "inc/Helper/AsyncFileReader.h"
#include "inc/Helper/ConcurrentSet.h"
#include "inc/Helper/VectorSetReader.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace SPTAG
{
namespace SPANN
{

struct PayloadTraceRecord
{
    int m_postingID = -1;
    int m_chunkID = 0;
    SizeType m_vectorID = -1;
    uint32_t m_payloadPageID = 0;
    uint32_t m_payloadPageCount = 0;
    uint64_t m_payloadPhysicalOffset = 0;
    uint32_t m_payloadBytes = 0;
    float m_coarseDist = MaxDist;
    // M2-H Phase 1: Per-posting I/O metrics for bad posting identification
    double m_ioWaitMs = 0.0;          // I/O wait time for this posting (legacy path)
    uint32_t m_listEleCount = 0;      // Number of elements in this posting
};

// M2-H Phase 1: Per-posting I/O trace for bad posting identification
struct PostingTraceRecord
{
    int m_postingID = -1;
    uint32_t m_listPageCount = 0;     // Number of pages this posting occupies
    uint32_t m_listEleCount = 0;      // Number of vectors in this posting
    uint64_t m_requestedBytes = 0;    // Bytes requested for this posting
    double m_ioWaitMs = 0.0;          // I/O wait time for this posting
    bool m_cacheHit = false;          // Whether this posting was served from cache
};

// M4-0: Pre-dedupe trace for primary-secondary payload dedupe analysis
struct PreDedupeTraceRecord
{
    int m_postingID = -1;
    SizeType m_vectorID = -1;
    uint32_t m_payloadBytes = 0;      // Full vector payload size (e.g., 128 bytes for SIFT1M)
    float m_coarseDist = MaxDist;     // Coarse distance (filled only if not deduped)
    bool m_wasDeduped = true;         // Default: deduped; set false if VID survived dedupe
};

struct SearchStats
{
    SearchStats()
    {
        Reset();
    }

    void Reset()
    {
        m_check = 0;
        m_exCheck = 0;
        m_totalListElementsCount = 0;
        m_diskIOCount = 0;
        m_diskAccessCount = 0;
        m_totalSearchLatency = 0;
        m_totalLatency = 0;
        m_exLatency = 0;
        m_asyncLatency0 = 0;
        m_asyncLatency1 = 0;
        m_asyncLatency2 = 0;
        m_queueLatency = 0;
        m_sleepLatency = 0;
        m_compLatency = 0;
        m_diskReadLatency = 0;
        m_exSetUpLatency = 0;
        m_requestedReadBytes = 0;
        m_readPages = 0;
        m_postingsTouched = 0;
        m_postingElementsRaw = 0;
        m_distanceEvaluatedCount = 0;
        m_duplicateVectorCount = 0;
        m_finalResultCount = 0;
        m_rerankCandidateCount = 0;
        m_ioIssueLatencyMs = 0;
        m_ioWaitLatencyMs = 0;
        m_batchReadTotalLatencyMs = 0;
        m_postingDecodeLatencyMs = 0;
        m_postingParseLatencyMs = 0;
        m_distanceCalcLatencyMs = 0;
        // P1: Per-phase timing metrics
        m_readHeaderDirMs = 0;
        m_scanCompactCodesMs = 0;
        m_mergeCoarseCandidatesMs = 0;
        m_buildPayloadReadPlanMs = 0;
        m_fetchPayloadAndRerankMs = 0;
        // P2: FetchPayloadPagesAndRerank sub-phase timing
        m_payloadReadWaitMs = 0;
        m_payloadCopyMs = 0;
        m_exactDistanceMs = 0;
        m_resultInsertionMs = 0;
        m_queryStartNs = 0;
        m_queryEndNs = 0;
        m_metadataBytesRead = 0;
        m_codePhysicalBytesRead = 0;
        m_codeBytesRead = 0;
        m_payloadLogicalBytesRead = 0;
        m_payloadBytesRead = 0;
        m_chunksConsidered = 0;
        m_chunksPruned = 0;
        m_chunksScanned = 0;
        m_coarseCandidateCount = 0;
        m_coarseCandidateCountAfterDedupe = 0;
        m_coarseCandidateHash = 0;
        m_payloadPageHash = 0;
        m_finalResultHash = 0;
        // Payload locality metrics
        m_uniquePayloadPages = 0;
        m_payloadCandidates = 0;
        m_postingsWithPayload = 0;
        m_totalPayloadPageSpans = 0;
        // P2: Coarse recall / miss-case attribution metrics
        m_truthCount = 0;
        m_coarseRecall = 0;
        m_coarseRecallAfterDedupe = 0;
        m_rerankRecall = 0;
        m_finalRecall = 0;
        m_truthRecoveredByHeadResult = 0;
        m_truthDroppedByPerPostingTopR = 0;
        m_truthDroppedByGlobalTopR = 0;
        m_truthDroppedByRerankTopK = 0;
        m_truthMissingPostingNotVisited = 0;
        m_truthMissingNotInPosting = 0;
        m_resultLimit = 0;
        // M1: Page cache statistics
        m_cacheHitCount = 0;
        m_cacheMissCount = 0;
        m_cacheBytesServed = 0;
        m_coalescedReads = 0;
        m_cacheLockWaitMs = 0;
        m_threadID = 0;
        m_searchRequestTime = std::chrono::steady_clock::time_point();
        m_payloadTraceRecords.clear();
        m_postingTraceRecords.clear();
        m_preDedupeTraceRecords.clear();
    }

    void Add(const SearchStats &other)
    {
        m_check += other.m_check;
        m_exCheck += other.m_exCheck;
        m_totalListElementsCount += other.m_totalListElementsCount;
        m_diskIOCount += other.m_diskIOCount;
        m_diskAccessCount += other.m_diskAccessCount;
        m_totalSearchLatency += other.m_totalSearchLatency;
        m_totalLatency += other.m_totalLatency;
        m_exLatency += other.m_exLatency;
        m_asyncLatency0 += other.m_asyncLatency0;
        m_asyncLatency1 += other.m_asyncLatency1;
        m_asyncLatency2 += other.m_asyncLatency2;
        m_queueLatency += other.m_queueLatency;
        m_sleepLatency += other.m_sleepLatency;
        m_compLatency += other.m_compLatency;
        m_diskReadLatency += other.m_diskReadLatency;
        m_exSetUpLatency += other.m_exSetUpLatency;
        m_requestedReadBytes += other.m_requestedReadBytes;
        m_readPages += other.m_readPages;
        m_postingsTouched += other.m_postingsTouched;
        m_postingElementsRaw += other.m_postingElementsRaw;
        m_distanceEvaluatedCount += other.m_distanceEvaluatedCount;
        m_duplicateVectorCount += other.m_duplicateVectorCount;
        m_finalResultCount += other.m_finalResultCount;
        m_rerankCandidateCount += other.m_rerankCandidateCount;
        m_ioIssueLatencyMs += other.m_ioIssueLatencyMs;
        m_ioWaitLatencyMs += other.m_ioWaitLatencyMs;
        m_batchReadTotalLatencyMs += other.m_batchReadTotalLatencyMs;
        m_postingDecodeLatencyMs += other.m_postingDecodeLatencyMs;
        m_postingParseLatencyMs += other.m_postingParseLatencyMs;
        m_distanceCalcLatencyMs += other.m_distanceCalcLatencyMs;
        // P1: Per-phase timing metrics
        m_readHeaderDirMs += other.m_readHeaderDirMs;
        m_scanCompactCodesMs += other.m_scanCompactCodesMs;
        m_mergeCoarseCandidatesMs += other.m_mergeCoarseCandidatesMs;
        m_buildPayloadReadPlanMs += other.m_buildPayloadReadPlanMs;
        m_fetchPayloadAndRerankMs += other.m_fetchPayloadAndRerankMs;
        // P2: FetchPayloadPagesAndRerank sub-phase timing
        m_payloadReadWaitMs += other.m_payloadReadWaitMs;
        m_payloadCopyMs += other.m_payloadCopyMs;
        m_exactDistanceMs += other.m_exactDistanceMs;
        m_resultInsertionMs += other.m_resultInsertionMs;
        m_metadataBytesRead += other.m_metadataBytesRead;
        m_codePhysicalBytesRead += other.m_codePhysicalBytesRead;
        m_codeBytesRead += other.m_codeBytesRead;
        m_payloadLogicalBytesRead += other.m_payloadLogicalBytesRead;
        m_payloadBytesRead += other.m_payloadBytesRead;
        m_chunksConsidered += other.m_chunksConsidered;
        m_chunksPruned += other.m_chunksPruned;
        m_chunksScanned += other.m_chunksScanned;
        m_coarseCandidateCount += other.m_coarseCandidateCount;
        m_coarseCandidateCountAfterDedupe += other.m_coarseCandidateCountAfterDedupe;
        m_uniquePayloadPages += other.m_uniquePayloadPages;
        m_payloadCandidates += other.m_payloadCandidates;
        m_postingsWithPayload += other.m_postingsWithPayload;
        m_totalPayloadPageSpans += other.m_totalPayloadPageSpans;
        // P2: Coarse recall / miss-case attribution metrics
        m_truthCount += other.m_truthCount;
        m_coarseRecall += other.m_coarseRecall;
        m_coarseRecallAfterDedupe += other.m_coarseRecallAfterDedupe;
        m_rerankRecall += other.m_rerankRecall;
        m_finalRecall += other.m_finalRecall;
        m_truthRecoveredByHeadResult += other.m_truthRecoveredByHeadResult;
        m_truthDroppedByPerPostingTopR += other.m_truthDroppedByPerPostingTopR;
        m_truthDroppedByGlobalTopR += other.m_truthDroppedByGlobalTopR;
        m_truthDroppedByRerankTopK += other.m_truthDroppedByRerankTopK;
        m_truthMissingPostingNotVisited += other.m_truthMissingPostingNotVisited;
        m_truthMissingNotInPosting += other.m_truthMissingNotInPosting;
        // M1: Page cache statistics
        m_cacheHitCount += other.m_cacheHitCount;
        m_cacheMissCount += other.m_cacheMissCount;
        m_cacheBytesServed += other.m_cacheBytesServed;
        m_coalescedReads += other.m_coalescedReads;
        m_cacheLockWaitMs += other.m_cacheLockWaitMs;
    }

    void Divide(double divisor)
    {
        if (divisor <= 0)
            return;

        m_check = static_cast<int>(m_check / divisor);
        m_exCheck = static_cast<int>(m_exCheck / divisor);
        m_totalListElementsCount = static_cast<int>(m_totalListElementsCount / divisor);
        m_diskIOCount = static_cast<int>(m_diskIOCount / divisor);
        m_diskAccessCount = static_cast<int>(m_diskAccessCount / divisor);
        m_totalSearchLatency /= divisor;
        m_totalLatency /= divisor;
        m_exLatency /= divisor;
        m_asyncLatency0 /= divisor;
        m_asyncLatency1 /= divisor;
        m_asyncLatency2 /= divisor;
        m_queueLatency /= divisor;
        m_sleepLatency /= divisor;
        m_compLatency /= divisor;
        m_diskReadLatency /= divisor;
        m_exSetUpLatency /= divisor;
        m_requestedReadBytes = static_cast<uint64_t>(m_requestedReadBytes / divisor);
        m_readPages = static_cast<uint64_t>(m_readPages / divisor);
        m_postingsTouched = static_cast<uint64_t>(m_postingsTouched / divisor);
        m_postingElementsRaw = static_cast<uint64_t>(m_postingElementsRaw / divisor);
        m_distanceEvaluatedCount = static_cast<uint64_t>(m_distanceEvaluatedCount / divisor);
        m_duplicateVectorCount = static_cast<uint64_t>(m_duplicateVectorCount / divisor);
        m_finalResultCount = static_cast<uint64_t>(m_finalResultCount / divisor);
        m_rerankCandidateCount = static_cast<uint64_t>(m_rerankCandidateCount / divisor);
        m_ioIssueLatencyMs /= divisor;
        m_ioWaitLatencyMs /= divisor;
        m_batchReadTotalLatencyMs /= divisor;
        m_postingDecodeLatencyMs /= divisor;
        m_postingParseLatencyMs /= divisor;
        m_distanceCalcLatencyMs /= divisor;
        // P1: Per-phase timing metrics
        m_readHeaderDirMs /= divisor;
        m_scanCompactCodesMs /= divisor;
        m_mergeCoarseCandidatesMs /= divisor;
        m_buildPayloadReadPlanMs /= divisor;
        m_fetchPayloadAndRerankMs /= divisor;
        // P2: FetchPayloadPagesAndRerank sub-phase timing
        m_payloadReadWaitMs /= divisor;
        m_payloadCopyMs /= divisor;
        m_exactDistanceMs /= divisor;
        m_resultInsertionMs /= divisor;
        m_metadataBytesRead = static_cast<uint64_t>(m_metadataBytesRead / divisor);
        m_codePhysicalBytesRead = static_cast<uint64_t>(m_codePhysicalBytesRead / divisor);
        m_codeBytesRead = static_cast<uint64_t>(m_codeBytesRead / divisor);
        m_payloadLogicalBytesRead = static_cast<uint64_t>(m_payloadLogicalBytesRead / divisor);
        m_payloadBytesRead = static_cast<uint64_t>(m_payloadBytesRead / divisor);
        m_chunksConsidered = static_cast<uint64_t>(m_chunksConsidered / divisor);
        m_chunksPruned = static_cast<uint64_t>(m_chunksPruned / divisor);
        m_chunksScanned = static_cast<uint64_t>(m_chunksScanned / divisor);
        m_coarseCandidateCount = static_cast<uint64_t>(m_coarseCandidateCount / divisor);
        m_coarseCandidateCountAfterDedupe = static_cast<uint64_t>(m_coarseCandidateCountAfterDedupe / divisor);
        m_uniquePayloadPages = static_cast<uint64_t>(m_uniquePayloadPages / divisor);
        m_payloadCandidates = static_cast<uint64_t>(m_payloadCandidates / divisor);
        m_postingsWithPayload = static_cast<uint64_t>(m_postingsWithPayload / divisor);
        m_totalPayloadPageSpans = static_cast<uint64_t>(m_totalPayloadPageSpans / divisor);
        // P2: Coarse recall / miss-case attribution metrics
        m_truthCount = static_cast<uint64_t>(m_truthCount / divisor);
        m_coarseRecall = static_cast<uint64_t>(m_coarseRecall / divisor);
        m_coarseRecallAfterDedupe = static_cast<uint64_t>(m_coarseRecallAfterDedupe / divisor);
        m_rerankRecall = static_cast<uint64_t>(m_rerankRecall / divisor);
        m_finalRecall = static_cast<uint64_t>(m_finalRecall / divisor);
        m_truthRecoveredByHeadResult = static_cast<uint64_t>(m_truthRecoveredByHeadResult / divisor);
        m_truthDroppedByPerPostingTopR = static_cast<uint64_t>(m_truthDroppedByPerPostingTopR / divisor);
        m_truthDroppedByGlobalTopR = static_cast<uint64_t>(m_truthDroppedByGlobalTopR / divisor);
        m_truthDroppedByRerankTopK = static_cast<uint64_t>(m_truthDroppedByRerankTopK / divisor);
        m_truthMissingPostingNotVisited = static_cast<uint64_t>(m_truthMissingPostingNotVisited / divisor);
        m_truthMissingNotInPosting = static_cast<uint64_t>(m_truthMissingNotInPosting / divisor);
        // M1: Page cache statistics
        m_cacheHitCount = static_cast<uint64_t>(m_cacheHitCount / divisor);
        m_cacheMissCount = static_cast<uint64_t>(m_cacheMissCount / divisor);
        m_cacheBytesServed = static_cast<uint64_t>(m_cacheBytesServed / divisor);
        m_coalescedReads = static_cast<uint64_t>(m_coalescedReads / divisor);
        m_cacheLockWaitMs /= divisor;
    }

    int m_check;

    int m_exCheck;

    int m_totalListElementsCount;

    int m_diskIOCount;

    int m_diskAccessCount;

    double m_totalSearchLatency;

    double m_totalLatency;

    double m_exLatency;

    double m_asyncLatency0;

    double m_asyncLatency1;

    double m_asyncLatency2;

    double m_queueLatency;

    double m_sleepLatency;

    double m_compLatency;

    double m_diskReadLatency;

    double m_exSetUpLatency;

    // Query-level I/O contract metrics
    uint64_t m_requestedReadBytes;
    uint64_t m_readPages;
    uint64_t m_postingsTouched;
    uint64_t m_postingElementsRaw;
    uint64_t m_distanceEvaluatedCount;
    uint64_t m_duplicateVectorCount;
    uint64_t m_finalResultCount;
    uint64_t m_rerankCandidateCount;

    // Latency split metrics (milliseconds)
    double m_ioIssueLatencyMs;
    double m_ioWaitLatencyMs;
    double m_batchReadTotalLatencyMs;
    double m_postingDecodeLatencyMs;
    double m_postingParseLatencyMs;
    double m_distanceCalcLatencyMs;

    // P1: Per-phase timing metrics for two-stage pipeline (milliseconds)
    double m_readHeaderDirMs;         // ReadPostingHeaderAndDirectory
    double m_scanCompactCodesMs;      // ScanCompactCodes
    double m_mergeCoarseCandidatesMs; // MergeCoarseCandidates
    double m_buildPayloadReadPlanMs;  // BuildPayloadReadPlan
    double m_fetchPayloadAndRerankMs; // FetchPayloadPagesAndRerank

    // P2: FetchPayloadPagesAndRerank sub-phase timing (milliseconds)
    double m_payloadReadWaitMs; // I/O wait for payload pages
    double m_payloadCopyMs;     // Multi-page payload buffer copy
    double m_exactDistanceMs;   // Exact distance calculation
    double m_resultInsertionMs; // Result insertion into QueryResultSet

    // Query timeline in monotonic steady-clock nanoseconds
    uint64_t m_queryStartNs;
    uint64_t m_queryEndNs;

    // Phase 1/2 posting pipeline counters
    uint64_t m_metadataBytesRead;
    uint64_t m_codePhysicalBytesRead;
    uint64_t m_codeBytesRead;
    uint64_t m_payloadLogicalBytesRead;
    uint64_t m_payloadBytesRead;
    uint64_t m_chunksConsidered;
    uint64_t m_chunksPruned;
    uint64_t m_chunksScanned;
    uint64_t m_coarseCandidateCount;
    uint64_t m_coarseCandidateCountAfterDedupe;
    uint64_t m_coarseCandidateHash;
    uint64_t m_payloadPageHash;
    uint64_t m_finalResultHash;

    // Payload locality metrics
    uint64_t m_uniquePayloadPages;
    uint64_t m_payloadCandidates;
    uint64_t m_postingsWithPayload;
    uint64_t m_totalPayloadPageSpans;

    // P2: Coarse recall / miss-case attribution metrics
    uint64_t m_truthCount;                    // Total truth vectors for this query (typically 10)
    uint64_t m_coarseRecall;                  // Truth found in coarse candidates (before dedupe)
    uint64_t m_coarseRecallAfterDedupe;       // Truth found in merged candidates (after dedupe)
    uint64_t m_rerankRecall;                  // Truth found in rerank candidates
    uint64_t m_finalRecall;                   // Truth found in final results
    uint64_t m_truthRecoveredByHeadResult;    // Truth present in final topK without entering rerank candidates
    uint64_t m_truthDroppedByPerPostingTopR;  // Truth seen in scanned posting but lost at per-posting topR cutoff
    uint64_t m_truthDroppedByGlobalTopR;      // Truth lost at global topR cutoff
    uint64_t m_truthDroppedByRerankTopK;      // Truth reranked but not present in final topK
    uint64_t m_truthMissingPostingNotVisited; // Truth not observed in any scanned posting/chunk
    uint64_t m_truthMissingNotInPosting;      // Reserved for offline full-membership attribution
    uint64_t m_resultLimit;                   // Requested final topK for P2 attribution

    // M1: Page cache statistics
    uint64_t m_cacheHitCount;                // Number of page cache hits
    uint64_t m_cacheMissCount;               // Number of page cache misses
    uint64_t m_cacheBytesServed;             // Bytes served from cache
    uint64_t m_coalescedReads;               // Number of coalesced I/O requests
    double m_cacheLockWaitMs;                // Per-query cache lock wait time (milliseconds)

    std::chrono::steady_clock::time_point m_searchRequestTime;

    int m_threadID;

    std::vector<PayloadTraceRecord> m_payloadTraceRecords;
    // M2-H Phase 1: Per-posting I/O trace for bad posting identification
    std::vector<PostingTraceRecord> m_postingTraceRecords;
    // M4-0: Pre-dedupe trace for primary-secondary payload dedupe analysis
    std::vector<PreDedupeTraceRecord> m_preDedupeTraceRecords;
};

struct IndexStats
{
    std::atomic_uint32_t m_headMiss{0};
    uint32_t m_appendTaskNum{0};
    uint32_t m_splitNum{0};
    uint32_t m_theSameHeadNum{0};
    uint32_t m_reAssignNum{0};
    uint32_t m_garbageNum{0};
    uint64_t m_reAssignScanNum{0};
    uint32_t m_mergeNum{0};

    // Split
    double m_splitCost{0};
    double m_getCost{0};
    double m_putCost{0};
    double m_clusteringCost{0};
    double m_updateHeadCost{0};
    double m_reassignScanCost{0};
    double m_reassignScanIOCost{0};

    // Append
    double m_appendCost{0};
    double m_appendIOCost{0};

    // reAssign
    double m_reAssignCost{0};
    double m_selectCost{0};
    double m_reAssignAppendCost{0};

    // GC
    double m_garbageCost{0};

    void PrintStat(int finishedInsert, bool cost = false, bool reset = false)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                     "After %d insertion, head vectors split %d times, head missing %d times, same head %d times, "
                     "reassign %d times, reassign scan %ld times, garbage collection %d times, merge %d times\n",
                     finishedInsert, m_splitNum, m_headMiss.load(), m_theSameHeadNum, m_reAssignNum, m_reAssignScanNum,
                     m_garbageNum, m_mergeNum);

        if (cost)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "AppendTaskNum: %d, TotalCost: %.3lf us, PerCost: %.3lf us\n",
                         m_appendTaskNum, m_appendCost, m_appendCost / m_appendTaskNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "AppendTaskNum: %d, AppendIO TotalCost: %.3lf us, PerCost: %.3lf us\n", m_appendTaskNum,
                         m_appendIOCost, m_appendIOCost / m_appendTaskNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, TotalCost: %.3lf ms, PerCost: %.3lf ms\n",
                         m_splitNum, m_splitCost, m_splitCost / m_splitNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, Read TotalCost: %.3lf us, PerCost: %.3lf us\n",
                         m_splitNum, m_getCost, m_getCost / m_splitNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, Clustering TotalCost: %.3lf us, PerCost: %.3lf us\n",
                         m_splitNum, m_clusteringCost, m_clusteringCost / m_splitNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, UpdateHead TotalCost: %.3lf ms, PerCost: %.3lf ms\n",
                         m_splitNum, m_updateHeadCost, m_updateHeadCost / m_splitNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, Write TotalCost: %.3lf us, PerCost: %.3lf us\n",
                         m_splitNum, m_putCost, m_putCost / m_splitNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "SplitNum: %d, ReassignScan TotalCost: %.3lf ms, PerCost: %.3lf ms\n", m_splitNum,
                         m_reassignScanCost, m_reassignScanCost / m_splitNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "SplitNum: %d, ReassignScanIO TotalCost: %.3lf us, PerCost: %.3lf us\n", m_splitNum,
                         m_reassignScanIOCost, m_reassignScanIOCost / m_splitNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "GCNum: %d, TotalCost: %.3lf us, PerCost: %.3lf us\n", m_garbageNum,
                         m_garbageCost, m_garbageCost / m_garbageNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "ReassignNum: %d, TotalCost: %.3lf us, PerCost: %.3lf us\n",
                         m_reAssignNum, m_reAssignCost, m_reAssignCost / m_reAssignNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "ReassignNum: %d, Select TotalCost: %.3lf us, PerCost: %.3lf us\n",
                         m_reAssignNum, m_selectCost, m_selectCost / m_reAssignNum);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "ReassignNum: %d, ReassignAppend TotalCost: %.3lf us, PerCost: %.3lf us\n", m_reAssignNum,
                         m_reAssignAppendCost, m_reAssignAppendCost / m_reAssignNum);
        }

        if (reset)
        {
            m_splitNum = 0;
            m_headMiss = 0;
            m_theSameHeadNum = 0;
            m_reAssignNum = 0;
            m_reAssignScanNum = 0;
            m_mergeNum = 0;
            m_garbageNum = 0;
            m_appendTaskNum = 0;
            m_splitCost = 0;
            m_clusteringCost = 0;
            m_garbageCost = 0;
            m_updateHeadCost = 0;
            m_getCost = 0;
            m_putCost = 0;
            m_reassignScanCost = 0;
            m_reassignScanIOCost = 0;
            m_appendCost = 0;
            m_appendIOCost = 0;
            m_reAssignCost = 0;
            m_selectCost = 0;
            m_reAssignAppendCost = 0;
        }
    }
};

struct CoarseCandidate
{
    SizeType m_vectorID = -1;
    float m_coarseDist = MaxDist;
    int m_postingID = -1;
    int m_chunkID = 0;
    size_t m_blockIndex = static_cast<size_t>(-1);
    uint32_t m_payloadOffset = 0;
    uint64_t m_payloadPhysicalOffset = 0;
    uint32_t m_payloadPageID = 0;
    uint32_t m_payloadBytes = 0;
    size_t m_payloadRequestStart = static_cast<size_t>(-1);
    uint32_t m_payloadPageCount = 0;
};

struct PayloadReadRequest
{
    int m_postingID = -1;
    int m_chunkID = 0;
    uint32_t m_pageID = 0;
    uint64_t m_pageKey = 0;
    uint64_t m_pageOffset = 0;
    uint32_t m_pageBytes = 0;
    uint32_t m_payloadOffset = 0;
    uint32_t m_payloadBytes = 0;
    SizeType m_vectorID = -1;
};

struct PostingBlockInfo
{
    int m_postingID = -1;
    int m_chunkID = 0;
    size_t m_diskRequestIndex = 0;
    const char *m_cachedCode = nullptr;
    uint32_t m_centroidOffset = 0;
    uint32_t m_centroidBytes = 0;
    uint32_t m_codeOffset = 0;
    uint32_t m_codeBytes = 0;
    uint32_t m_codeRecordBytes = 0;
    uint32_t m_payloadOffset = 0;
    uint32_t m_payloadBytes = 0;
    uint32_t m_payloadRecordBytes = 0;
    uint32_t m_recordCount = 0;
    float m_radius = 0.0f;
    float m_lowerBound = 0.0f;
    uint32_t m_codeBufferOffset = 0;
};

struct ExtraWorkSpace : public SPTAG::COMMON::IWorkSpace
{
    ExtraWorkSpace()
    {
    }

    ~ExtraWorkSpace()
    {
        if (m_callback)
        {
            m_callback();
        }
    }

    ExtraWorkSpace(ExtraWorkSpace &other)
    {
        Initialize(other.m_deduper.MaxCheck(), other.m_deduper.HashTableExponent(), (int)other.m_pageBuffers.size(),
                   (int)(other.m_pageBuffers[0].GetPageSize()), other.m_blockIO, other.m_enableDataCompression);
    }

    void Initialize(int p_maxCheck, int p_hashExp, int p_internalResultNum, int p_maxPages, bool p_blockIO,
                    bool enableDataCompression)
    {
        m_deduper.Init(p_maxCheck, p_hashExp);
        Clear(p_internalResultNum, p_maxPages, p_blockIO, enableDataCompression);
        m_relaxedMono = false;
    }

    void Initialize(va_list &arg)
    {
        int maxCheck = va_arg(arg, int);
        int hashExp = va_arg(arg, int);
        int internalResultNum = va_arg(arg, int);
        int maxPages = va_arg(arg, int);
        bool blockIo = bool(va_arg(arg, int));
        bool enableDataCompression = bool(va_arg(arg, int));
        Initialize(maxCheck, hashExp, internalResultNum, maxPages, blockIo, enableDataCompression);
    }

    void Clear(int p_internalResultNum, int p_maxPages, bool p_blockIO, bool enableDataCompression)
    {
        if (m_pageBuffers.empty() || p_internalResultNum > m_pageBuffers.size() ||
            p_maxPages > m_pageBuffers[0].GetPageSize())
        {
            m_postingIDs.reserve(p_internalResultNum);
            m_pageBuffers.resize(p_internalResultNum);
            for (int pi = 0; pi < p_internalResultNum; pi++)
            {
                m_pageBuffers[pi].ReservePageBuffer(p_maxPages);
            }
            m_blockIO = p_blockIO;
            if (p_blockIO)
            {
                int numPages = (p_maxPages >> PageSizeEx);
                m_diskRequests.resize(p_internalResultNum * numPages);
                // SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "WorkSpace Init %d*%d reqs\n", p_internalResultNum,
                // numPages);
                for (int pi = 0; pi < p_internalResultNum; pi++)
                {
                    for (int pg = 0; pg < numPages; pg++)
                    {
                        int rid = pi * numPages + pg;
                        auto &req = m_diskRequests[rid];

                        req.m_buffer = (char *)(m_pageBuffers[pi].GetBuffer() + ((std::uint64_t)pg << PageSizeEx));
                        req.m_extension = &m_processIocp;
#ifdef _MSC_VER
                        memset(&(req.myres.m_col), 0, sizeof(OVERLAPPED));
                        req.myres.m_col.m_data = (void *)(&req);
#else
                        memset(&(req.myiocb), 0, sizeof(struct iocb));
                        req.myiocb.aio_buf = reinterpret_cast<uint64_t>(req.m_buffer);
                        req.myiocb.aio_data = reinterpret_cast<uintptr_t>(&req);
#endif
                    }
                }
            }
            else
            {
                m_diskRequests.resize(p_internalResultNum);
                // SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "WorkSpace Init %d reqs\n", p_internalResultNum);
                for (int pi = 0; pi < p_internalResultNum; pi++)
                {
                    auto &req = m_diskRequests[pi];

                    req.m_buffer = (char *)(m_pageBuffers[pi].GetBuffer());
                    req.m_extension = &m_processIocp;
#ifdef _MSC_VER
                    memset(&(req.myres.m_col), 0, sizeof(OVERLAPPED));
                    req.myres.m_col.m_data = (void *)(&req);
#else
                    memset(&(req.myiocb), 0, sizeof(struct iocb));
                    req.myiocb.aio_buf = reinterpret_cast<uint64_t>(req.m_buffer);
                    req.myiocb.aio_data = reinterpret_cast<uintptr_t>(&req);
#endif
                }
            }
        }

        m_enableDataCompression = enableDataCompression;
        if (enableDataCompression)
        {
            m_decompressBuffer.ReservePageBuffer(p_maxPages);
        }

        m_coarseCandidates.clear();
        m_mergedCandidates.clear();
        m_payloadReadRequests.clear();
        m_postingBlocks.clear();
        m_chunkCodeBuffers.clear();
        m_chunkCodeDiskRequests.clear();
        m_payloadPageBuffers.clear();
        m_payloadDiskRequests.clear();
        m_bestCoarseCandidateByVector.clear();
        m_payloadPageRequestIndex.clear();
        m_truthBestScannedRank.clear();
        m_truthScannedPostingCount.clear();
    }

    void EnsurePostingMetadataBufferCapacity(size_t p_bufferBytes)
    {
        m_postingMetadataBuffer.ReservePageBuffer(p_bufferBytes);
    }

    void EnsureChunkCodeRequestCapacity(size_t p_requestCount, size_t p_bufferBytes)
    {
        if (m_chunkCodeBuffers.size() < p_requestCount)
        {
            size_t oldSize = m_chunkCodeBuffers.size();
            m_chunkCodeBuffers.resize(p_requestCount);
            m_chunkCodeDiskRequests.resize(p_requestCount);
            for (size_t i = oldSize; i < p_requestCount; ++i)
            {
                auto &req = m_chunkCodeDiskRequests[i];
                req.m_extension = &m_processIocp;
#ifdef _MSC_VER
                memset(&(req.myres.m_col), 0, sizeof(OVERLAPPED));
                req.myres.m_col.m_data = (void *)(&req);
#else
                memset(&(req.myiocb), 0, sizeof(struct iocb));
                req.myiocb.aio_data = reinterpret_cast<uintptr_t>(&req);
#endif
            }
        }

        for (size_t i = 0; i < p_requestCount; ++i)
        {
            m_chunkCodeBuffers[i].ReservePageBuffer(p_bufferBytes);
            auto &req = m_chunkCodeDiskRequests[i];
            req.m_buffer = reinterpret_cast<char *>(m_chunkCodeBuffers[i].GetBuffer());
            req.m_callback = [](bool) {};
            req.m_status = m_asyncChannel;
#ifndef _MSC_VER
            req.myiocb.aio_buf = reinterpret_cast<uint64_t>(req.m_buffer);
#endif
        }
    }

    void EnsurePayloadRequestCapacity(size_t p_requestCount)
    {
        if (m_payloadPageBuffers.size() >= p_requestCount)
        {
            return;
        }

        size_t oldSize = m_payloadPageBuffers.size();
        m_payloadPageBuffers.resize(p_requestCount);
        m_payloadDiskRequests.resize(p_requestCount);
        for (size_t i = oldSize; i < p_requestCount; ++i)
        {
            m_payloadPageBuffers[i].ReservePageBuffer(PageSize);
            auto &req = m_payloadDiskRequests[i];
            req.m_buffer = reinterpret_cast<char *>(m_payloadPageBuffers[i].GetBuffer());
            req.m_extension = &m_processIocp;
            req.m_callback = [](bool) {};
            req.m_status = m_asyncChannel;
#ifdef _MSC_VER
            memset(&(req.myres.m_col), 0, sizeof(OVERLAPPED));
            req.myres.m_col.m_data = (void *)(&req);
#else
            memset(&(req.myiocb), 0, sizeof(struct iocb));
            req.myiocb.aio_buf = reinterpret_cast<uint64_t>(req.m_buffer);
            req.myiocb.aio_data = reinterpret_cast<uintptr_t>(&req);
#endif
        }
    }

    void EnsurePostingQuantizedTargetCapacity(size_t p_bufferBytes)
    {
        if (m_postingQuantizedTarget.size() < p_bufferBytes)
        {
            m_postingQuantizedTarget.resize(p_bufferBytes);
        }
    }

    void EnsurePayloadScratchCapacity(size_t p_bufferBytes)
    {
        if (m_payloadScratch.size() < p_bufferBytes)
        {
            m_payloadScratch.resize(p_bufferBytes);
        }
    }

    std::vector<int> m_postingIDs;

    COMMON::OptHashPosVector m_deduper;

    Helper::RequestQueue m_processIocp;

    std::vector<Helper::PageBuffer<std::uint8_t>> m_pageBuffers;

    bool m_blockIO = false;

    bool m_enableDataCompression = false;

    Helper::PageBuffer<std::uint8_t> m_decompressBuffer;

    Helper::PageBuffer<std::uint8_t> m_postingMetadataBuffer;

    std::vector<Helper::AsyncReadRequest> m_diskRequests;

    int m_ri = 0;

    int m_pi = 0;

    int m_offset = 0;

    bool m_loadPosting = false;

    bool m_relaxedMono = false;

    int m_loadedPostingNum = 0;

    int m_asyncChannel = 0;

    std::vector<CoarseCandidate> m_coarseCandidates;

    std::vector<CoarseCandidate> m_mergedCandidates;

    std::vector<PayloadReadRequest> m_payloadReadRequests;

    std::vector<PostingBlockInfo> m_postingBlocks;

    std::unordered_map<SizeType, CoarseCandidate> m_bestCoarseCandidateByVector;

    std::unordered_map<uint64_t, size_t> m_payloadPageRequestIndex;

    // P2 attribution scratch: populated only when truth is passed to two-stage search.
    std::unordered_map<SizeType, uint32_t> m_truthBestScannedRank;

    std::unordered_map<SizeType, uint32_t> m_truthScannedPostingCount;

    std::vector<Helper::PageBuffer<std::uint8_t>> m_chunkCodeBuffers;

    std::vector<Helper::AsyncReadRequest> m_chunkCodeDiskRequests;

    std::vector<Helper::PageBuffer<std::uint8_t>> m_payloadPageBuffers;

    std::vector<Helper::AsyncReadRequest> m_payloadDiskRequests;

    std::vector<uint8_t> m_postingQuantizedTarget;

    std::vector<uint8_t> m_payloadScratch;

    std::function<bool(const ByteArray &)> m_filterFunc;

    std::function<void()> m_callback;
};

class IExtraSearcher
{
  public:
    IExtraSearcher()
    {
    }

    ~IExtraSearcher()
    {
    }
    virtual bool Available() = 0;

    virtual bool LoadIndex(Options &p_options, COMMON::VersionLabel &p_versionMap,
                           COMMON::Dataset<std::uint64_t> &m_vectorTranslateMap,
                           std::shared_ptr<VectorIndex> m_index) = 0;

    virtual ErrorCode SearchIndex(ExtraWorkSpace *p_exWorkSpace, QueryResult &p_queryResults,
                                  std::shared_ptr<VectorIndex> p_index, SearchStats *p_stats,
                                  std::set<int> *truth = nullptr, std::map<int, std::set<int>> *found = nullptr) = 0;

    virtual ErrorCode SearchIterativeNext(ExtraWorkSpace *p_exWorkSpace, QueryResult &p_headResults,
                                          QueryResult &p_queryResults, std::shared_ptr<VectorIndex> p_index,
                                          const VectorIndex *p_spann) = 0;

    virtual ErrorCode SearchIndexWithoutParsing(ExtraWorkSpace *p_exWorkSpace) = 0;

    virtual ErrorCode SearchNextInPosting(ExtraWorkSpace *p_exWorkSpace, QueryResult &p_headResults,
                                          QueryResult &p_queryResults, std::shared_ptr<VectorIndex> &p_index,
                                          const VectorIndex *p_spann) = 0;

    virtual bool BuildIndex(std::shared_ptr<Helper::VectorSetReader> &p_reader, std::shared_ptr<VectorIndex> p_index,
                            Options &p_opt, COMMON::VersionLabel &p_versionMap,
                            COMMON::Dataset<std::uint64_t> &p_vectorTranslateMap, SizeType upperBound = -1) = 0;

    virtual void InitWorkSpace(ExtraWorkSpace *p_exWorkSpace, bool clear = false) = 0;

    virtual ErrorCode GetPostingDebug(ExtraWorkSpace *p_exWorkSpace, std::shared_ptr<VectorIndex> p_index, SizeType vid,
                                      std::vector<SizeType> &VIDs, std::shared_ptr<VectorSet> &vecs) = 0;

    virtual ErrorCode RefineIndex(std::shared_ptr<VectorIndex> &p_index, bool p_prereassign = true,
                                  std::vector<SizeType> *p_headmapping = nullptr,
                                  std::vector<SizeType> *p_mapping = nullptr)
    {
        return ErrorCode::Undefined;
    }
    virtual ErrorCode AddIndex(ExtraWorkSpace *p_exWorkSpace, std::shared_ptr<VectorSet> &p_vectorSet,
                               std::shared_ptr<VectorIndex> p_index, SizeType p_begin)
    {
        return ErrorCode::Undefined;
    }
    virtual ErrorCode DeleteIndex(SizeType p_id)
    {
        return ErrorCode::Undefined;
    }

    virtual bool AllFinished()
    {
        return false;
    }
    virtual void GetDBStats()
    {
        return;
    }
    virtual int64_t GetNumBlocks()
    {
        return 0;
    }
    virtual void GetIndexStats(int finishedInsert, bool cost, bool reset)
    {
        return;
    }
    virtual void ForceCompaction()
    {
        return;
    }

    virtual bool CheckValidPosting(SizeType postingID) = 0;
    virtual ErrorCode CheckPosting(SizeType postingiD, std::vector<std::uint8_t> *visited = nullptr,
                                   ExtraWorkSpace *p_exWorkSpace = nullptr) = 0;
    virtual SizeType SearchVector(ExtraWorkSpace *p_exWorkSpace, std::shared_ptr<VectorSet> &p_vectorSet,
                                  std::shared_ptr<VectorIndex> p_index, int testNum = 64, SizeType VID = -1)
    {
        return -1;
    }
    virtual void ForceGC(ExtraWorkSpace *p_exWorkSpace, VectorIndex *p_index)
    {
        return;
    }

    virtual ErrorCode GetWritePosting(ExtraWorkSpace *p_exWorkSpace, SizeType pid, std::string &posting,
                                      bool write = false)
    {
        return ErrorCode::Undefined;
    }

    virtual ErrorCode Checkpoint(std::string prefix)
    {
        return ErrorCode::Success;
    }
};
} // namespace SPANN
} // namespace SPTAG

#endif // _SPTAG_SPANN_IEXTRASEARCHER_H_
