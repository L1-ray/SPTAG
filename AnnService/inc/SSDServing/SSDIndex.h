// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include "inc/Core/Common.h"
#include "inc/Core/Common/DistanceUtils.h"
#include "inc/Core/Common/QueryResultSet.h"
#include "inc/Core/SPANN/Index.h"
#include "inc/Core/SPANN/PageCache.h"
#include "inc/Helper/StringConvert.h"
#include "inc/Helper/VectorSetReader.h"
#include "inc/SSDServing/Utils.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>

namespace SPTAG
{
namespace SSDServing
{
namespace SSDIndex
{

template <typename ValueType>
ErrorCode OutputResult(const std::string &p_output, std::vector<QueryResult> &p_results, int p_resultNum)
{
    if (!p_output.empty())
    {
        auto ptr = f_createIO();
        if (ptr == nullptr || !ptr->Initialize(p_output.c_str(), std::ios::binary | std::ios::out))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed create file: %s\n", p_output.c_str());
            return ErrorCode::FailedCreateFile;
        }
        int32_t i32Val = static_cast<int32_t>(p_results.size());
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
            return ErrorCode::DiskIOFail;
        }
        i32Val = p_resultNum;
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
            return ErrorCode::DiskIOFail;
        }

        float fVal = 0;
        for (size_t i = 0; i < p_results.size(); ++i)
        {
            for (int j = 0; j < p_resultNum; ++j)
            {
                i32Val = p_results[i].GetResult(j)->VID;
                if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
                    return ErrorCode::DiskIOFail;
                }

                fVal = p_results[i].GetResult(j)->Dist;
                if (ptr->WriteBinary(sizeof(fVal), reinterpret_cast<char *>(&fVal)) != sizeof(fVal))
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Fail to write result file!\n");
                    return ErrorCode::DiskIOFail;
                }
            }
        }
    }
    return ErrorCode::Success;
}

template <typename T, typename V>
void PrintPercentiles(const std::vector<V> &p_values, std::function<T(const V &)> p_get, const char *p_format)
{
    if (p_values.empty())
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Avg\t50tiles\t90tiles\t95tiles\t99tiles\t99.9tiles\tMax\n");
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "NA\tNA\tNA\tNA\tNA\tNA\tNA\n");
        return;
    }

    double sum = 0;
    std::vector<T> collects;
    collects.reserve(p_values.size());
    for (const auto &v : p_values)
    {
        T tmp = p_get(v);
        sum += tmp;
        collects.push_back(tmp);
    }

    std::sort(collects.begin(), collects.end());

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Avg\t50tiles\t90tiles\t95tiles\t99tiles\t99.9tiles\tMax\n");

    std::string formatStr("%.3lf");
    for (int i = 1; i < 7; ++i)
    {
        formatStr += '\t';
        formatStr += p_format;
    }

    formatStr += '\n';

    auto percentileIndex = [&](double ratio) -> size_t {
        size_t idx = static_cast<size_t>(collects.size() * ratio);
        if (idx >= collects.size())
            idx = collects.size() - 1;
        return idx;
    };

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, formatStr.c_str(), sum / collects.size(), collects[percentileIndex(0.50)],
                 collects[percentileIndex(0.90)], collects[percentileIndex(0.95)], collects[percentileIndex(0.99)],
                 collects[percentileIndex(0.999)], collects[static_cast<size_t>(collects.size() - 1)]);
}

inline double SafeRatio(uint64_t numerator, uint64_t denominator)
{
    if (denominator == 0)
        return 0.0;
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

template <typename ValueType>
void SearchSequential(SPANN::Index<ValueType> *p_index, int p_numThreads, std::vector<QueryResult> &p_results,
                      std::vector<SPANN::SearchStats> &p_stats, int p_maxQueryCount, int p_internalResultNum,
                      std::vector<std::set<int>> *p_truth = nullptr)
{
    int numQueries = min(static_cast<int>(p_results.size()), p_maxQueryCount);

    std::atomic_size_t queriesSent(0);

    std::vector<std::thread> threads;
    threads.reserve(p_numThreads);
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Searching: numThread: %d, numQueries: %d.\n", p_numThreads, numQueries);

    Utils::StopW sw;

    for (int i = 0; i < p_numThreads; i++)
    {
        threads.emplace_back([&, i]() {
            NumaStrategy ns =
                (p_index->GetDiskIndex() != nullptr)
                    ? NumaStrategy::SCATTER
                    : NumaStrategy::LOCAL; // Only for SPANN, we need to avoid IO threads overlap with search threads.
            Helper::SetThreadAffinity(i, threads[i], ns, OrderStrategy::ASC);

            Utils::StopW threadws;
            size_t index = 0;
            while (true)
            {
                index = queriesSent.fetch_add(1);
                if (index < numQueries)
                {
                    if ((index & ((1 << 14) - 1)) == 0)
                    {
                        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Sent %.2lf%%...\n", index * 100.0 / numQueries);
                    }

                    auto queryStartTp = std::chrono::steady_clock::now();
                    p_stats[index].m_queryStartNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(queryStartTp.time_since_epoch()).count());
                    p_stats[index].m_threadID = i;

                    double startTime = threadws.getElapsedMs();
                    p_index->GetMemoryIndex()->SearchIndex(p_results[index]);
                    double endTime = threadws.getElapsedMs();
                    std::set<int> *truthPtr =
                        (p_truth != nullptr && index < p_truth->size()) ? &((*p_truth)[index]) : nullptr;
                    p_index->SearchDiskIndex(p_results[index], &(p_stats[index]), truthPtr);
                    double exEndTime = threadws.getElapsedMs();
                    auto queryEndTp = std::chrono::steady_clock::now();
                    p_stats[index].m_queryEndNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(queryEndTp.time_since_epoch()).count());

                    p_stats[index].m_exLatency = exEndTime - endTime;
                    p_stats[index].m_totalLatency = p_stats[index].m_totalSearchLatency = exEndTime - startTime;
                }
                else
                {
                    return;
                }
            }
        });
    }
    for (auto &thread : threads)
    {
        thread.join();
    }

    double sendingCost = sw.getElapsedSec();

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Finish sending in %.3lf seconds, actuallQPS is %.2lf, query count %u.\n",
                 sendingCost, numQueries / sendingCost, static_cast<uint32_t>(numQueries));

    for (int i = 0; i < numQueries; i++)
    {
        p_results[i].CleanQuantizedTarget();
    }
}

template <typename ValueType> ErrorCode Search(SPANN::Index<ValueType> *p_index)
{
    SPANN::Options &p_opts = *(p_index->GetOptions());
    std::string outputFile = p_opts.m_searchResult;
    std::string truthFile = p_opts.m_truthPath;
    std::string warmupFile = p_opts.m_warmupPath;

    // M1: Initialize sharded page cache if enabled
    if (p_opts.m_enablePageCache)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Initializing sharded page cache with %lu bytes...\n",
                     static_cast<unsigned long>(p_opts.m_pageCacheMaxBytes));
        SPANN::InitGlobalShardedPageCache(p_opts.m_pageCacheMaxBytes);
    }

    // M1 Phase B: Initialize coalescer if enabled (separate from cache)
    if (p_opts.m_enableInFlightCoalescing && !p_opts.m_enablePageCache)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Initializing global in-flight coalescer...\n");
        SPANN::InitGlobalCoalescer();
    }

    if (p_index->m_pQuantizer)
    {
        p_index->m_pQuantizer->SetEnableADC(p_opts.m_enableADC);
    }

    if (!p_opts.m_logFile.empty())
    {
        SetLogger(std::make_shared<Helper::FileLogger>(Helper::LogLevel::LL_Info, p_opts.m_logFile.c_str()));
    }
    int numThreads = p_opts.m_searchThreadNum;
    int internalResultNum = p_opts.m_searchInternalResultNum;
    int K = p_opts.m_resultNum;
    int truthK = (p_opts.m_truthResultNum <= 0) ? K : p_opts.m_truthResultNum;
    const bool useTwoStageStaticPosting =
        p_opts.m_storage == Storage::STATIC && p_opts.m_ssdPostingFormatVersion >= 2 && p_opts.m_enableTwoStagePosting;
    auto headQueryQuantizer = useTwoStageStaticPosting ? nullptr : p_index->m_pQuantizer;
    ErrorCode ret;

    if (!warmupFile.empty())
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start loading warmup query set...\n");
        std::shared_ptr<Helper::ReaderOptions> queryOptions(
            new Helper::ReaderOptions(p_opts.m_valueType, p_opts.m_dim, p_opts.m_warmupType, p_opts.m_warmupDelimiter));
        auto queryReader = Helper::VectorSetReader::CreateInstance(queryOptions);
        if (ErrorCode::Success != (ret = queryReader->LoadFile(p_opts.m_warmupPath)))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read query file.\n");
            return ret;
        }
        auto warmupQuerySet = queryReader->GetVectorSet();
        int warmupNumQueries = warmupQuerySet->Count();

        std::vector<QueryResult> warmupResults(warmupNumQueries, QueryResult(NULL, max(K, internalResultNum), false));
        std::vector<SPANN::SearchStats> warmpUpStats(warmupNumQueries);
        for (int i = 0; i < warmupNumQueries; ++i)
        {
            (*((COMMON::QueryResultSet<ValueType> *)&warmupResults[i]))
                .SetTarget(reinterpret_cast<ValueType *>(warmupQuerySet->GetVector(i)), headQueryQuantizer);
            warmupResults[i].Reset();
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start warmup...\n");
        SearchSequential(p_index, numThreads, warmupResults, warmpUpStats, p_opts.m_queryCountLimit, internalResultNum);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nFinish warmup...\n");
    }

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start loading QuerySet...\n");
    std::shared_ptr<Helper::ReaderOptions> queryOptions(
        new Helper::ReaderOptions(p_opts.m_valueType, p_opts.m_dim, p_opts.m_queryType, p_opts.m_queryDelimiter));
    auto queryReader = Helper::VectorSetReader::CreateInstance(queryOptions);
    if (ErrorCode::Success != (ret = queryReader->LoadFile(p_opts.m_queryPath)))
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read query file.\n");
        return ret;
    }
    auto querySet = queryReader->GetVectorSet();
    int numQueries = querySet->Count();
    int effectiveQueries = min(numQueries, p_opts.m_queryCountLimit);

    std::vector<QueryResult> results(numQueries, QueryResult(NULL, max(K, internalResultNum), false));
    std::vector<SPANN::SearchStats> stats(numQueries);
    for (int i = 0; i < numQueries; ++i)
    {
        (*((COMMON::QueryResultSet<ValueType> *)&results[i]))
            .SetTarget(reinterpret_cast<ValueType *>(querySet->GetVector(i)), headQueryQuantizer);
        results[i].Reset();
    }

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start ANN Search...\n");

    // Load truth before search for miss-case attribution (P2)
    std::vector<std::set<SizeType>> truth;
    if (!truthFile.empty())
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Pre-loading TruthFile for miss-case attribution...\n");
        auto ptr = f_createIO();
        if (ptr == nullptr || !ptr->Initialize(truthFile.c_str(), std::ios::in | std::ios::binary))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed open truth file: %s\n", truthFile.c_str());
            return ErrorCode::FailedOpenFile;
        }
        int originalK = truthK;
        COMMON::TruthSet::LoadTruth(ptr, truth, numQueries, originalK, truthK, p_opts.m_truthType);
        char tmp[4];
        if (ptr->ReadBinary(4, tmp) == 4)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Truth number is larger than query number(%d)!\n", numQueries);
        }
    }

    SearchSequential(p_index, numThreads, results, stats, p_opts.m_queryCountLimit, internalResultNum, &truth);

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nFinish ANN Search...\n");

    std::shared_ptr<VectorSet> vectorSet;

    if (!p_opts.m_vectorPath.empty() && fileexists(p_opts.m_vectorPath.c_str()))
    {
        std::shared_ptr<Helper::ReaderOptions> vectorOptions(
            new Helper::ReaderOptions(p_opts.m_valueType, p_opts.m_dim, p_opts.m_vectorType, p_opts.m_vectorDelimiter));
        auto vectorReader = Helper::VectorSetReader::CreateInstance(vectorOptions);
        if (ErrorCode::Success == vectorReader->LoadFile(p_opts.m_vectorPath))
        {
            vectorSet = vectorReader->GetVectorSet();
            if (p_opts.m_distCalcMethod == DistCalcMethod::Cosine)
                vectorSet->Normalize(numThreads);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nLoad VectorSet(%d,%d).\n", vectorSet->Count(),
                         vectorSet->Dimension());
        }
    }

    if (p_opts.m_rerank > 0 && vectorSet != nullptr)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n Begin rerank...\n");
        for (int i = 0; i < results.size(); i++)
        {
            for (int j = 0; j < K; j++)
            {
                if (results[i].GetResult(j)->VID < 0)
                    continue;
                results[i].GetResult(j)->Dist = COMMON::DistanceUtils::ComputeDistance(
                    (const ValueType *)querySet->GetVector(i),
                    (const ValueType *)vectorSet->GetVector(results[i].GetResult(j)->VID), querySet->Dimension(),
                    p_opts.m_distCalcMethod);
            }
            BasicResult *re = results[i].GetResults();
            std::sort(re, re + K, COMMON::Compare);
        }
        K = p_opts.m_rerank;
    }

    for (int i = 0; i < effectiveQueries; ++i)
    {
        uint64_t validCount = 0;
        int resultNum = min(K, results[i].GetResultNum());
        for (int j = 0; j < resultNum; ++j)
        {
            if (results[i].GetResult(j)->VID >= 0)
                ++validCount;
        }
        stats[i].m_finalResultCount = validCount;
        if (stats[i].m_rerankCandidateCount == 0)
        {
            stats[i].m_rerankCandidateCount = validCount;
        }
    }

    float recall = 0, MRR = 0;
    std::vector<double> perQueryRecall(max(0, effectiveQueries), -1.0);
    if (!truth.empty())
    {
        recall =
            COMMON::TruthSet::CalculateRecall<ValueType>((p_index->GetMemoryIndex()).get(), results, truth, K, truthK,
                                                         querySet, vectorSet, numQueries, nullptr, false, &MRR);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Recall%d@%d: %f MRR@%d: %f\n", truthK, K, recall, K, MRR);

        if (truthK > 0)
        {
            int bound = min<int>(effectiveQueries, static_cast<int>(truth.size()));
            for (int i = 0; i < bound; ++i)
            {
                int hit = 0;
                int resultNum = min(K, results[i].GetResultNum());
                for (int j = 0; j < resultNum; ++j)
                {
                    auto vid = results[i].GetResult(j)->VID;
                    if (vid >= 0 && truth[i].count(vid))
                        ++hit;
                }
                perQueryRecall[i] = static_cast<double>(hit) / static_cast<double>(truthK);
            }
        }
    }

    if (p_opts.m_enablePayloadTrace && !p_opts.m_payloadTraceOutput.empty())
    {
        std::ofstream traceOut(p_opts.m_payloadTraceOutput.c_str(), std::ios::out | std::ios::trunc);
        if (!traceOut.is_open())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open payload trace CSV: %s\n",
                         p_opts.m_payloadTraceOutput.c_str());
        }
        else
        {
            double sampleRate = p_opts.m_payloadTraceSampleRate;
            if (sampleRate < 0.0)
                sampleRate = 0.0;
            if (sampleRate > 1.0)
                sampleRate = 1.0;

            std::mt19937_64 rng(20260502ULL);
            std::uniform_real_distribution<double> sampleDist(0.0, 1.0);

            traceOut << "query_id,trace_index,posting_id,chunk_id,vector_id,payload_page_id,payload_page_count,"
                        "payload_physical_offset,payload_bytes,coarse_dist\n";
            traceOut << std::fixed << std::setprecision(6);

            uint64_t rowsWritten = 0;
            for (int i = 0; i < effectiveQueries; ++i)
            {
                if (sampleRate < 1.0 && sampleDist(rng) > sampleRate)
                    continue;

                const auto &records = stats[i].m_payloadTraceRecords;
                for (size_t traceIndex = 0; traceIndex < records.size(); ++traceIndex)
                {
                    const auto &record = records[traceIndex];
                    traceOut << i << "," << traceIndex << "," << record.m_postingID << "," << record.m_chunkID << ","
                             << record.m_vectorID << "," << record.m_payloadPageID << "," << record.m_payloadPageCount
                             << "," << record.m_payloadPhysicalOffset << "," << record.m_payloadBytes << ","
                             << record.m_coarseDist << "\n";
                    ++rowsWritten;
                }
            }

            traceOut.close();
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Payload trace CSV saved to %s, rows=%llu\n",
                         p_opts.m_payloadTraceOutput.c_str(), static_cast<unsigned long long>(rowsWritten));
        }

        for (int i = 0; i < effectiveQueries; ++i)
        {
            stats[i].m_payloadTraceRecords.clear();
            stats[i].m_payloadTraceRecords.shrink_to_fit();
        }
    }

    // M2-H Phase 1: Output per-posting I/O trace for bad posting identification
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Posting trace check: enable=%d output=%s\n",
                 p_opts.m_enablePostingTrace ? 1 : 0, p_opts.m_postingTraceOutput.c_str());
    if (p_opts.m_enablePostingTrace && !p_opts.m_postingTraceOutput.empty())
    {
        std::ofstream traceOut(p_opts.m_postingTraceOutput.c_str(), std::ios::out | std::ios::trunc);
        if (!traceOut.is_open())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open posting trace CSV: %s\n",
                         p_opts.m_postingTraceOutput.c_str());
        }
        else
        {
            traceOut << "query_id,posting_index,posting_id,list_page_count,list_ele_count,requested_bytes,"
                        "io_wait_ms,cache_hit\n";
            traceOut << std::fixed << std::setprecision(6);

            uint64_t rowsWritten = 0;
            for (int i = 0; i < effectiveQueries; ++i)
            {
                const auto &records = stats[i].m_postingTraceRecords;
                for (size_t postingIndex = 0; postingIndex < records.size(); ++postingIndex)
                {
                    const auto &record = records[postingIndex];
                    traceOut << i << "," << postingIndex << "," << record.m_postingID << ","
                             << record.m_listPageCount << "," << record.m_listEleCount << ","
                             << record.m_requestedBytes << "," << record.m_ioWaitMs << ","
                             << (record.m_cacheHit ? 1 : 0) << "\n";
                    ++rowsWritten;
                }
            }

            traceOut.close();
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Posting trace CSV saved to %s, rows=%llu\n",
                         p_opts.m_postingTraceOutput.c_str(), static_cast<unsigned long long>(rowsWritten));
        }

        for (int i = 0; i < effectiveQueries; ++i)
        {
            stats[i].m_postingTraceRecords.clear();
            stats[i].m_postingTraceRecords.shrink_to_fit();
        }
    }

    // M4-0: Output pre-dedupe trace for primary-secondary payload dedupe analysis
    if (p_opts.m_enablePreDedupeTrace && !p_opts.m_preDedupeTraceOutput.empty())
    {
        std::ofstream traceOut(p_opts.m_preDedupeTraceOutput.c_str(), std::ios::out | std::ios::trunc);
        if (!traceOut.is_open())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open pre-dedupe trace CSV: %s\n",
                         p_opts.m_preDedupeTraceOutput.c_str());
        }
        else
        {
            traceOut << "query_id,posting_id,vector_id,payload_bytes,coarse_dist,was_deduped\n";
            traceOut << std::fixed << std::setprecision(6);

            uint64_t rowsWritten = 0;
            for (int i = 0; i < effectiveQueries; ++i)
            {
                const auto &records = stats[i].m_preDedupeTraceRecords;
                for (size_t traceIndex = 0; traceIndex < records.size(); ++traceIndex)
                {
                    const auto &record = records[traceIndex];
                    traceOut << i << "," << record.m_postingID << "," << record.m_vectorID << ","
                             << record.m_payloadBytes << "," << record.m_coarseDist << ","
                             << (record.m_wasDeduped ? 1 : 0) << "\n";
                    ++rowsWritten;
                }
            }
            traceOut.close();
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Pre-dedupe trace CSV saved to %s, rows=%llu\n",
                         p_opts.m_preDedupeTraceOutput.c_str(), static_cast<unsigned long long>(rowsWritten));
        }
        // Clear records to free memory
        for (int i = 0; i < effectiveQueries; ++i)
        {
            stats[i].m_preDedupeTraceRecords.clear();
            stats[i].m_preDedupeTraceRecords.shrink_to_fit();
        }
    }

    // Phase 2: Output head distance trace for adaptive budget feature extraction
    if (p_opts.m_enableHeadDistanceTrace && !p_opts.m_headDistanceTraceOutput.empty())
    {
        std::ofstream traceOut(p_opts.m_headDistanceTraceOutput.c_str(), std::ios::out | std::ios::trunc);
        if (!traceOut.is_open())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open head distance trace CSV: %s\n",
                         p_opts.m_headDistanceTraceOutput.c_str());
        }
        else
        {
            traceOut << "query_id,posting_index,posting_id,head_dist\n";
            traceOut << std::fixed << std::setprecision(6);

            uint64_t rowsWritten = 0;
            for (int i = 0; i < effectiveQueries; ++i)
            {
                const auto &records = stats[i].m_headDistanceTraceRecords;
                for (size_t traceIndex = 0; traceIndex < records.size(); ++traceIndex)
                {
                    const auto &record = records[traceIndex];
                    traceOut << i << "," << record.m_postingIndex << "," << record.m_postingID << ","
                             << record.m_headDist << "\n";
                    ++rowsWritten;
                }
            }
            traceOut.close();
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Head distance trace CSV saved to %s, rows=%llu\n",
                         p_opts.m_headDistanceTraceOutput.c_str(), static_cast<unsigned long long>(rowsWritten));
        }
        // Clear records to free memory
        for (int i = 0; i < effectiveQueries; ++i)
        {
            stats[i].m_headDistanceTraceRecords.clear();
            stats[i].m_headDistanceTraceRecords.shrink_to_fit();
        }
    }

    std::vector<SPANN::SearchStats> statsForPrint;
    if (effectiveQueries > 0)
    {
        statsForPrint.assign(stats.begin(), stats.begin() + effectiveQueries);
    }

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nEx Elements Count:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_totalListElementsCount; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nHead Latency Distribution:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_totalSearchLatency - ss.m_exLatency; },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nEx Latency Distribution:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_exLatency; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTotal Latency Distribution:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_totalSearchLatency; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTotal Disk Page Access Distribution:\n");
    PrintPercentiles<int, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> int { return ss.m_diskAccessCount; }, "%4d");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTotal Disk IO Distribution:\n");
    PrintPercentiles<int, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> int { return ss.m_diskIOCount; }, "%4d");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n=== Detailed I/O Statistics ===\n");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nRequested Bytes Read Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_requestedReadBytes); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nMetadata Bytes Read Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_metadataBytesRead); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPages Read Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_readPages); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPostings Touched Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_postingsTouched); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nRaw Posting Elements Scanned Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_postingElementsRaw); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDistance Evaluated Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_distanceEvaluatedCount); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDuplicate Vector Read Ratio:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double {
            return SafeRatio(ss.m_duplicateVectorCount, ss.m_postingElementsRaw);
        },
        "%.6lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDistance Eval Ratio:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double {
            return SafeRatio(ss.m_distanceEvaluatedCount, ss.m_postingElementsRaw);
        },
        "%.6lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nFinal Result Ratio:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double {
            return SafeRatio(ss.m_finalResultCount, ss.m_postingElementsRaw);
        },
        "%.6lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCode Logical Bytes Read Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_codeBytesRead); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCode Physical Bytes Read Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_codePhysicalBytesRead); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPayload Logical Bytes Read Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_payloadLogicalBytesRead); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPayload Physical Bytes Read Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_payloadBytesRead); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nChunks Considered Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_chunksConsidered); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nChunks Scanned Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_chunksScanned); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nChunks Pruned Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_chunksPruned); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nChunk Prune Ratio:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return SafeRatio(ss.m_chunksPruned, ss.m_chunksConsidered); },
        "%.6lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCoarse Candidate Count Before Dedupe Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_coarseCandidateCount); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCoarse Candidate Count After Dedupe Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double {
            return static_cast<double>(ss.m_coarseCandidateCountAfterDedupe);
        },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nRerank Candidate Count Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_rerankCandidateCount); }, "%.3lf");

    // Payload locality metrics
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nUnique Payload Pages Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_uniquePayloadPages); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPayload Candidates Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_payloadCandidates); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCandidates Per Payload Page:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double {
            return SafeRatio(ss.m_payloadCandidates, ss.m_uniquePayloadPages);
        },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPostings With Payload Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_postingsWithPayload); }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTotal Payload Page Spans Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_totalPayloadPageSpans); },
        "%.3lf");

    // P2: Coarse recall / miss-case attribution statistics
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n--- P2: Miss-Case Attribution ---\n");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTruth Count Per Query:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_truthCount); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCoarse Recall (truth in coarse candidates):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_coarseRecall); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCoarse Recall After Dedupe:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_coarseRecallAfterDedupe); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nRerank Recall (truth in rerank candidates):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_rerankRecall); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nFinal Recall (truth in final results):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_finalRecall); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTruth Recovered By Head Result:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_truthRecoveredByHeadResult); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTruth Dropped By Per-Posting TopR:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_truthDroppedByPerPostingTopR); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTruth Dropped By Global TopR:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_truthDroppedByGlobalTopR); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTruth Dropped By Rerank TopK:\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_truthDroppedByRerankTopK); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTruth Missing (Not Observed In Scanned Posting/Chunk):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_truthMissingPostingNotVisited); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nTruth Missing (Not In Any Posting, Reserved):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint,
        [](const SPANN::SearchStats &ss) -> double { return static_cast<double>(ss.m_truthMissingNotInPosting); },
        "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nI/O Issue Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_ioIssueLatencyMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nI/O Wait Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_ioWaitLatencyMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nBatch Read Total Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_batchReadTotalLatencyMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPosting Decode Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_postingDecodeLatencyMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPosting Parse Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_postingParseLatencyMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDistance Calc Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_distanceCalcLatencyMs; }, "%.3lf");

    // P1: Per-phase timing metrics for two-stage pipeline
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n--- P1: Two-Stage Per-Phase Timing (ms) ---\n");
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nRead Header & Directory Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_readHeaderDirMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nScan Compact Codes Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_scanCompactCodesMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nMerge Coarse Candidates Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_mergeCoarseCandidatesMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nBuild Payload Read Plan Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_buildPayloadReadPlanMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nFetch Payload & Rerank Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_fetchPayloadAndRerankMs; }, "%.3lf");

    // P2: Fetch Payload & Rerank sub-phase timing
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n--- P2: Fetch Payload & Rerank Sub-Phase Timing (ms) ---\n");
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPayload Read Wait Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_payloadReadWaitMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPayload Copy Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_payloadCopyMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nExact Distance Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_exactDistanceMs; }, "%.3lf");

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nResult Insertion Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_resultInsertionMs; }, "%.3lf");

    // M1: Per-query cache lock wait statistics
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nCache Lock Wait Latency (ms):\n");
    PrintPercentiles<double, SPANN::SearchStats>(
        statsForPrint, [](const SPANN::SearchStats &ss) -> double { return ss.m_cacheLockWaitMs; }, "%.6lf");

    if (p_opts.m_enableDetailedIOStats && !p_opts.m_detailedIOStatsOutput.empty())
    {
        std::ofstream csvOut(p_opts.m_detailedIOStatsOutput.c_str(), std::ios::out | std::ios::trunc);
        if (!csvOut.is_open())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open detailed I/O stats CSV: %s\n",
                         p_opts.m_detailedIOStatsOutput.c_str());
        }
        else
        {
            std::ostringstream runIdBuilder;
            runIdBuilder << "run_"
                         << std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            std::string runId = runIdBuilder.str();

            double sampleRate = p_opts.m_detailedIOStatsSampleRate;
            if (sampleRate < 0.0)
                sampleRate = 0.0;
            if (sampleRate > 1.0)
                sampleRate = 1.0;

            std::mt19937_64 rng(20260427ULL);
            std::uniform_real_distribution<double> sampleDist(0.0, 1.0);

            csvOut
                << "run_id,thread_count,io_threads,ssd_threads,query_id,thread_id,query_start_ns,query_end_ns,total_"
                   "latency_ms,head_latency_ms,ex_latency_ms,io_issue_ms,io_wait_ms,batch_read_total_ms,posting_decode_"
                   "ms,posting_parse_ms,distance_calc_ms,postings_touched,pages_read,requested_read_bytes,posting_"
                   "elements_raw,metadata_read_bytes,code_logical_bytes,code_physical_bytes,payload_logical_bytes,"
                   "payload_physical_bytes,distance_evaluated_count,duplicate_vector_count,coarse_candidate_count_"
                   "before_dedupe,coarse_candidate_count_after_dedupe,rerank_candidate_count,final_result_count,"
                   "coarse_candidate_hash,payload_page_hash,final_result_hash,recall,"
                   "unique_payload_pages,payload_candidates,postings_with_payload,total_payload_page_spans,"
                   "truth_count,coarse_recall,coarse_recall_after_dedupe,rerank_recall,final_recall,"
                   "truth_recovered_by_head_result,truth_dropped_per_posting_topr,truth_dropped_global_topr,"
                   "truth_dropped_rerank_topk,truth_missing_not_observed_in_scanned_posting,"
                   "truth_missing_not_in_posting_reserved,"
                   "read_header_dir_ms,scan_compact_codes_ms,merge_coarse_candidates_ms,build_payload_read_plan_ms,"
                   "fetch_payload_and_rerank_ms,"
                   "payload_read_wait_ms,payload_copy_ms,exact_distance_ms,result_insertion_ms\n";
            csvOut << std::fixed << std::setprecision(6);

            for (int i = 0; i < effectiveQueries; ++i)
            {
                if (sampleRate < 1.0 && sampleDist(rng) > sampleRate)
                    continue;
                double queryRecall = (i < static_cast<int>(perQueryRecall.size())) ? perQueryRecall[i] : -1.0;
                double headLatency = stats[i].m_totalSearchLatency - stats[i].m_exLatency;

                csvOut << runId << "," << p_opts.m_searchThreadNum << "," << p_opts.m_ioThreads << ","
                       << p_opts.m_iSSDNumberOfThreads << "," << i << "," << stats[i].m_threadID << ","
                       << stats[i].m_queryStartNs << "," << stats[i].m_queryEndNs << ","
                       << stats[i].m_totalSearchLatency << "," << headLatency << "," << stats[i].m_exLatency << ","
                       << stats[i].m_ioIssueLatencyMs << "," << stats[i].m_ioWaitLatencyMs << ","
                       << stats[i].m_batchReadTotalLatencyMs << "," << stats[i].m_postingDecodeLatencyMs << ","
                       << stats[i].m_postingParseLatencyMs << "," << stats[i].m_distanceCalcLatencyMs << ","
                       << stats[i].m_postingsTouched << "," << stats[i].m_readPages << ","
                       << stats[i].m_requestedReadBytes << "," << stats[i].m_postingElementsRaw << ","
                       << stats[i].m_metadataBytesRead << "," << stats[i].m_codeBytesRead << ","
                       << stats[i].m_codePhysicalBytesRead << "," << stats[i].m_payloadLogicalBytesRead << ","
                       << stats[i].m_payloadBytesRead << "," << stats[i].m_distanceEvaluatedCount << ","
                       << stats[i].m_duplicateVectorCount << "," << stats[i].m_coarseCandidateCount << ","
                       << stats[i].m_coarseCandidateCountAfterDedupe << "," << stats[i].m_rerankCandidateCount << ","
                       << stats[i].m_finalResultCount << "," << stats[i].m_coarseCandidateHash << ","
                       << stats[i].m_payloadPageHash << "," << stats[i].m_finalResultHash << "," << queryRecall << ","
                       << stats[i].m_uniquePayloadPages << "," << stats[i].m_payloadCandidates << ","
                       << stats[i].m_postingsWithPayload << "," << stats[i].m_totalPayloadPageSpans << ","
                       << stats[i].m_truthCount << "," << stats[i].m_coarseRecall << ","
                       << stats[i].m_coarseRecallAfterDedupe << "," << stats[i].m_rerankRecall << ","
                       << stats[i].m_finalRecall << "," << stats[i].m_truthRecoveredByHeadResult << ","
                       << stats[i].m_truthDroppedByPerPostingTopR << "," << stats[i].m_truthDroppedByGlobalTopR << ","
                       << stats[i].m_truthDroppedByRerankTopK << "," << stats[i].m_truthMissingPostingNotVisited << ","
                       << stats[i].m_truthMissingNotInPosting << "," << stats[i].m_readHeaderDirMs << ","
                       << stats[i].m_scanCompactCodesMs << "," << stats[i].m_mergeCoarseCandidatesMs << ","
                       << stats[i].m_buildPayloadReadPlanMs << "," << stats[i].m_fetchPayloadAndRerankMs << ","
                       << stats[i].m_payloadReadWaitMs << "," << stats[i].m_payloadCopyMs << ","
                       << stats[i].m_exactDistanceMs << "," << stats[i].m_resultInsertionMs << "\n";
            }

            csvOut.close();
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Detailed I/O stats CSV saved to %s\n",
                         p_opts.m_detailedIOStatsOutput.c_str());
        }
    }

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n");

    if (!outputFile.empty())
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start output to %s\n", outputFile.c_str());
        OutputResult<ValueType>(outputFile, results, K);
    }

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Recall@%d: %f MRR@%d: %f\n", K, recall, K, MRR);

    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n");

    if (p_opts.m_recall_analysis)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start recall analysis...\n");

        std::shared_ptr<VectorIndex> headIndex = p_index->GetMemoryIndex();
        SizeType sampleSize = numQueries < 100 ? numQueries : 100;
        SizeType sampleK = headIndex->GetNumSamples() < 1000 ? headIndex->GetNumSamples() : 1000;
        float sampleE = 1e-6f;

        std::vector<SizeType> samples(sampleSize, 0);
        std::vector<float> queryHeadRecalls(sampleSize, 0);
        std::vector<float> truthRecalls(sampleSize, 0);
        std::vector<int> shouldSelect(sampleSize, 0);
        std::vector<int> shouldSelectLong(sampleSize, 0);
        std::vector<int> nearQueryHeads(sampleSize, 0);
        std::vector<int> annNotFound(sampleSize, 0);
        std::vector<int> rngRule(sampleSize, 0);
        std::vector<int> postingCut(sampleSize, 0);
        for (int i = 0; i < sampleSize; i++)
            samples[i] = COMMON::Utils::rand(numQueries);

        std::vector<std::thread> mythreads;
        mythreads.reserve(p_opts.m_iSSDNumberOfThreads);
        std::atomic_size_t sent(0);
        for (int tid = 0; tid < p_opts.m_iSSDNumberOfThreads; tid++)
        {
            mythreads.emplace_back([&, tid]() {
                size_t i = 0;
                while (true)
                {
                    i = sent.fetch_add(1);
                    if (i < sampleSize)
                    {
                        COMMON::QueryResultSet<ValueType> queryANNHeads(
                            (const ValueType *)(querySet->GetVector(samples[i])), max(K, internalResultNum));
                        headIndex->SearchIndex(queryANNHeads);
                        float queryANNHeadsLongestDist = queryANNHeads.GetResult(internalResultNum - 1)->Dist;

                        COMMON::QueryResultSet<ValueType> queryBFHeads(
                            (const ValueType *)(querySet->GetVector(samples[i])), max(sampleK, internalResultNum));
                        for (SizeType y = 0; y < headIndex->GetNumSamples(); y++)
                        {
                            float dist =
                                headIndex->ComputeDistance(queryBFHeads.GetQuantizedTarget(), headIndex->GetSample(y));
                            queryBFHeads.AddPoint(y, dist);
                        }
                        queryBFHeads.SortResult();

                        {
                            std::vector<bool> visited(internalResultNum, false);
                            for (SizeType y = 0; y < internalResultNum; y++)
                            {
                                for (SizeType z = 0; z < internalResultNum; z++)
                                {
                                    if (visited[z])
                                        continue;

                                    if (fabs(queryANNHeads.GetResult(z)->Dist - queryBFHeads.GetResult(y)->Dist) <
                                        sampleE)
                                    {
                                        queryHeadRecalls[i] += 1;
                                        visited[z] = true;
                                        break;
                                    }
                                }
                            }
                        }

                        std::map<int, std::set<int>> tmpFound; // headID->truths
                        p_index->DebugSearchDiskIndex(queryBFHeads, internalResultNum, sampleK, nullptr,
                                                      &truth[samples[i]], &tmpFound);

                        for (SizeType z = 0; z < K; z++)
                        {
                            truthRecalls[i] += truth[samples[i]].count(queryBFHeads.GetResult(z)->VID);
                        }

                        for (SizeType z = 0; z < K; z++)
                        {
                            truth[samples[i]].erase(results[samples[i]].GetResult(z)->VID);
                        }

                        for (std::map<int, std::set<int>>::iterator it = tmpFound.begin(); it != tmpFound.end(); it++)
                        {
                            float q2truthposting = headIndex->ComputeDistance(querySet->GetVector(samples[i]),
                                                                              headIndex->GetSample(it->first));
                            for (auto vid : it->second)
                            {
                                if (!truth[samples[i]].count(vid))
                                    continue;

                                if (q2truthposting < queryANNHeadsLongestDist)
                                    shouldSelect[i] += 1;
                                else
                                {
                                    shouldSelectLong[i] += 1;

                                    std::set<int> nearQuerySelectedHeads;
                                    float v2vhead = headIndex->ComputeDistance(vectorSet->GetVector(vid),
                                                                               headIndex->GetSample(it->first));
                                    for (SizeType z = 0; z < internalResultNum; z++)
                                    {
                                        if (queryANNHeads.GetResult(z)->VID < 0)
                                            break;
                                        float v2qhead = headIndex->ComputeDistance(
                                            vectorSet->GetVector(vid),
                                            headIndex->GetSample(queryANNHeads.GetResult(z)->VID));
                                        if (v2qhead < v2vhead)
                                        {
                                            nearQuerySelectedHeads.insert(queryANNHeads.GetResult(z)->VID);
                                        }
                                    }
                                    if (nearQuerySelectedHeads.size() == 0)
                                        continue;

                                    nearQueryHeads[i] += 1;

                                    COMMON::QueryResultSet<ValueType> annTruthHead(
                                        (const ValueType *)(vectorSet->GetVector(vid)),
                                        p_opts.m_debugBuildInternalResultNum);
                                    headIndex->SearchIndex(annTruthHead);

                                    bool found = false;
                                    for (SizeType z = 0; z < annTruthHead.GetResultNum(); z++)
                                    {
                                        if (nearQuerySelectedHeads.count(annTruthHead.GetResult(z)->VID))
                                        {
                                            found = true;
                                            break;
                                        }
                                    }

                                    if (!found)
                                    {
                                        annNotFound[i] += 1;
                                        continue;
                                    }

                                    // RNG rule and posting cut
                                    std::set<int> replicas;
                                    for (SizeType z = 0;
                                         z < annTruthHead.GetResultNum() && replicas.size() < p_opts.m_replicaCount;
                                         z++)
                                    {
                                        BasicResult *item = annTruthHead.GetResult(z);
                                        if (item->VID < 0)
                                            break;

                                        bool good = true;
                                        for (auto r : replicas)
                                        {
                                            if (p_opts.m_rngFactor *
                                                    headIndex->ComputeDistance(headIndex->GetSample(r),
                                                                               headIndex->GetSample(item->VID)) <
                                                item->Dist)
                                            {
                                                good = false;
                                                break;
                                            }
                                        }
                                        if (good)
                                            replicas.insert(item->VID);
                                    }

                                    found = false;
                                    for (auto r : nearQuerySelectedHeads)
                                    {
                                        if (replicas.count(r))
                                        {
                                            found = true;
                                            break;
                                        }
                                    }

                                    if (found)
                                        postingCut[i] += 1;
                                    else
                                        rngRule[i] += 1;
                                }
                            }
                        }
                    }
                    else
                    {
                        return;
                    }
                }
            });
        }
        for (auto &t : mythreads)
        {
            t.join();
        }
        mythreads.clear();

        float headacc = 0, truthacc = 0, shorter = 0, longer = 0, lost = 0, buildNearQueryHeads = 0,
              buildAnnNotFound = 0, buildRNGRule = 0, buildPostingCut = 0;
        for (int i = 0; i < sampleSize; i++)
        {
            headacc += queryHeadRecalls[i];
            truthacc += truthRecalls[i];

            lost += shouldSelect[i] + shouldSelectLong[i];
            shorter += shouldSelect[i];
            longer += shouldSelectLong[i];

            buildNearQueryHeads += nearQueryHeads[i];
            buildAnnNotFound += annNotFound[i];
            buildRNGRule += rngRule[i];
            buildPostingCut += postingCut[i];
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Query head recall @%d:%f.\n", internalResultNum,
                     headacc / sampleSize / internalResultNum);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "BF top %d postings truth recall @%d:%f.\n", sampleK, truthK,
                     truthacc / sampleSize / truthK);

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                     "Percent of truths in postings have shorter distance than query selected heads: %f percent\n",
                     shorter / lost * 100);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                     "Percent of truths in postings have longer distance than query selected heads: %f percent\n",
                     longer / lost * 100);

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                     "\tPercent of truths no shorter distance in query selected heads: %f percent\n",
                     (longer - buildNearQueryHeads) / lost * 100);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                     "\tPercent of truths exists shorter distance in query selected heads: %f percent\n",
                     buildNearQueryHeads / lost * 100);

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\t\tRNG rule ANN search loss: %f percent\n",
                     buildAnnNotFound / lost * 100);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\t\tPosting cut loss: %f percent\n", buildPostingCut / lost * 100);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\t\tRNG rule loss: %f percent\n", buildRNGRule / lost * 100);
    }

    // M1: Shutdown page cache if it was initialized
    if (p_opts.m_enablePageCache)
    {
        auto* cache = SPANN::GetGlobalShardedPageCache();
        if (cache)
        {
            const auto& stats = cache->GetStats();
            // B4: Four hard metrics
            double hitRate = stats.HitRate();
            uint64_t savedIoPages = stats.m_savedIoPages.load();
            double lockWaitUs = static_cast<double>(stats.m_cacheLockWaitNs.load()) / 1000.0;
            uint64_t enqueueDropped = stats.m_asyncInsertDropped.load();
            uint64_t enqueueTotal = stats.m_asyncInsertQueued.load() + enqueueDropped;
            double dropRate = enqueueTotal > 0 ? static_cast<double>(enqueueDropped) / enqueueTotal : 0.0;

            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "Sharded page cache stats: hit_rate=%.4f, saved_io_pages=%lu, lock_wait_us=%.2f, enqueue_drop_rate=%.4f\n",
                         hitRate,
                         static_cast<unsigned long>(savedIoPages),
                         lockWaitUs,
                         dropRate);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "  (hits=%lu, misses=%lu, evictions=%lu, bytes_served=%lu, queued=%lu, dropped=%lu)\n",
                         static_cast<unsigned long>(stats.m_hits.load()),
                         static_cast<unsigned long>(stats.m_misses.load()),
                         static_cast<unsigned long>(stats.m_evictions.load()),
                         static_cast<unsigned long>(stats.m_bytesServed.load()),
                         static_cast<unsigned long>(stats.m_asyncInsertQueued.load()),
                         static_cast<unsigned long>(enqueueDropped));
        }
        SPANN::ShutdownGlobalShardedPageCache();
    }

    // M1 Phase B: Shutdown coalescer if it was initialized
    if (p_opts.m_enableInFlightCoalescing && !p_opts.m_enablePageCache)
    {
        auto* coalescer = SPANN::GetGlobalCoalescer();
        if (coalescer)
        {
            const auto& stats = coalescer->GetStats();
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "In-flight coalescer stats: total_requests=%lu, coalesced=%lu\n",
                         static_cast<unsigned long>(stats.m_totalRequests.load()),
                         static_cast<unsigned long>(stats.m_coalescedReads.load()));
        }
        SPANN::ShutdownGlobalCoalescer();
    }

    return ErrorCode::Success;
}
} // namespace SSDIndex
} // namespace SSDServing
} // namespace SPTAG
