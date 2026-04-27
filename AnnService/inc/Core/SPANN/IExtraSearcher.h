// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _SPTAG_SPANN_IEXTRASEARCHER_H_
#define _SPTAG_SPANN_IEXTRASEARCHER_H_

#include "Options.h"

#include "inc/Core/VectorIndex.h"
#include "inc/Core/Common/VersionLabel.h"
#include "inc/Helper/AsyncFileReader.h"
#include "inc/Helper/VectorSetReader.h"
#include "inc/Helper/ConcurrentSet.h"
#include <memory>
#include <vector>
#include <chrono>
#include <atomic>
#include <set>
#include <cstdint>

namespace SPTAG {
    namespace SPANN {

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
                m_queryStartNs = 0;
                m_queryEndNs = 0;
                m_threadID = 0;
                m_searchRequestTime = std::chrono::steady_clock::time_point();
            }

            void Add(const SearchStats& other)
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
            }

            void Divide(double divisor)
            {
                if (divisor <= 0) return;

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

            // Query timeline in monotonic steady-clock nanoseconds
            uint64_t m_queryStartNs;
            uint64_t m_queryEndNs;

            std::chrono::steady_clock::time_point m_searchRequestTime;

            int m_threadID;
        };

        struct IndexStats {
            std::atomic_uint32_t m_headMiss{ 0 };
            uint32_t m_appendTaskNum{ 0 };
            uint32_t m_splitNum{ 0 };
            uint32_t m_theSameHeadNum{ 0 };
            uint32_t m_reAssignNum{ 0 };
            uint32_t m_garbageNum{ 0 };
            uint64_t m_reAssignScanNum{ 0 };
            uint32_t m_mergeNum{ 0 };

            //Split
            double m_splitCost{ 0 };
            double m_getCost{ 0 };
            double m_putCost{ 0 };
            double m_clusteringCost{ 0 };
            double m_updateHeadCost{ 0 };
            double m_reassignScanCost{ 0 };
            double m_reassignScanIOCost{ 0 };

            // Append
            double m_appendCost{ 0 };
            double m_appendIOCost{ 0 };

            // reAssign
            double m_reAssignCost{ 0 };
            double m_selectCost{ 0 };
            double m_reAssignAppendCost{ 0 };

            // GC
            double m_garbageCost{ 0 };

            void PrintStat(int finishedInsert, bool cost = false, bool reset = false) {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "After %d insertion, head vectors split %d times, head missing %d times, same head %d times, reassign %d times, reassign scan %ld times, garbage collection %d times, merge %d times\n",
                    finishedInsert, m_splitNum, m_headMiss.load(), m_theSameHeadNum, m_reAssignNum, m_reAssignScanNum, m_garbageNum, m_mergeNum);

                if (cost) {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "AppendTaskNum: %d, TotalCost: %.3lf us, PerCost: %.3lf us\n", m_appendTaskNum, m_appendCost, m_appendCost / m_appendTaskNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "AppendTaskNum: %d, AppendIO TotalCost: %.3lf us, PerCost: %.3lf us\n", m_appendTaskNum, m_appendIOCost, m_appendIOCost / m_appendTaskNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, TotalCost: %.3lf ms, PerCost: %.3lf ms\n", m_splitNum, m_splitCost, m_splitCost / m_splitNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, Read TotalCost: %.3lf us, PerCost: %.3lf us\n", m_splitNum, m_getCost, m_getCost / m_splitNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, Clustering TotalCost: %.3lf us, PerCost: %.3lf us\n", m_splitNum, m_clusteringCost, m_clusteringCost / m_splitNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, UpdateHead TotalCost: %.3lf ms, PerCost: %.3lf ms\n", m_splitNum, m_updateHeadCost, m_updateHeadCost / m_splitNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, Write TotalCost: %.3lf us, PerCost: %.3lf us\n", m_splitNum, m_putCost, m_putCost / m_splitNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, ReassignScan TotalCost: %.3lf ms, PerCost: %.3lf ms\n", m_splitNum, m_reassignScanCost, m_reassignScanCost / m_splitNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SplitNum: %d, ReassignScanIO TotalCost: %.3lf us, PerCost: %.3lf us\n", m_splitNum, m_reassignScanIOCost, m_reassignScanIOCost / m_splitNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "GCNum: %d, TotalCost: %.3lf us, PerCost: %.3lf us\n", m_garbageNum, m_garbageCost, m_garbageCost / m_garbageNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "ReassignNum: %d, TotalCost: %.3lf us, PerCost: %.3lf us\n", m_reAssignNum, m_reAssignCost, m_reAssignCost / m_reAssignNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "ReassignNum: %d, Select TotalCost: %.3lf us, PerCost: %.3lf us\n", m_reAssignNum, m_selectCost, m_selectCost / m_reAssignNum);
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "ReassignNum: %d, ReassignAppend TotalCost: %.3lf us, PerCost: %.3lf us\n", m_reAssignNum, m_reAssignAppendCost, m_reAssignAppendCost / m_reAssignNum);
                }

                if (reset) {
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

        struct ExtraWorkSpace : public SPTAG::COMMON::IWorkSpace
        {
            ExtraWorkSpace() {}

            ~ExtraWorkSpace() {
                if (m_callback) {
                    m_callback();
                }
            }

            ExtraWorkSpace(ExtraWorkSpace& other) {
                Initialize(other.m_deduper.MaxCheck(), other.m_deduper.HashTableExponent(), (int)other.m_pageBuffers.size(), (int)(other.m_pageBuffers[0].GetPageSize()), other.m_blockIO, other.m_enableDataCompression);
            }

            void Initialize(int p_maxCheck, int p_hashExp, int p_internalResultNum, int p_maxPages, bool p_blockIO, bool enableDataCompression) {
                m_deduper.Init(p_maxCheck, p_hashExp);
                Clear(p_internalResultNum, p_maxPages, p_blockIO, enableDataCompression);
                m_relaxedMono = false;
            }

            void Initialize(va_list& arg) {
                int maxCheck = va_arg(arg, int);
                int hashExp = va_arg(arg, int);
                int internalResultNum = va_arg(arg, int);
                int maxPages = va_arg(arg, int);
                bool blockIo = bool(va_arg(arg, int));
                bool enableDataCompression = bool(va_arg(arg, int));
                Initialize(maxCheck, hashExp, internalResultNum, maxPages, blockIo, enableDataCompression);
            }

            void Clear(int p_internalResultNum, int p_maxPages, bool p_blockIO, bool enableDataCompression) {
                if (p_internalResultNum > m_pageBuffers.size() || p_maxPages > m_pageBuffers[0].GetPageSize()) {
                    m_postingIDs.reserve(p_internalResultNum);
                    m_pageBuffers.resize(p_internalResultNum);
                    for (int pi = 0; pi < p_internalResultNum; pi++) {
                        m_pageBuffers[pi].ReservePageBuffer(p_maxPages);
                    }
                    m_blockIO = p_blockIO;
                    if (p_blockIO) {
                        int numPages = (p_maxPages >> PageSizeEx);
                        m_diskRequests.resize(p_internalResultNum * numPages);
			            //SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "WorkSpace Init %d*%d reqs\n", p_internalResultNum, numPages);
                        for (int pi = 0; pi < p_internalResultNum; pi++) {
                            for (int pg = 0; pg < numPages; pg++) {
                                int rid = pi * numPages + pg;
                                auto& req = m_diskRequests[rid];

                                req.m_buffer = (char*)(m_pageBuffers[pi].GetBuffer() + ((std::uint64_t)pg << PageSizeEx));
                                req.m_extension = &m_processIocp;
#ifdef _MSC_VER
                                memset(&(req.myres.m_col), 0, sizeof(OVERLAPPED));
                                req.myres.m_col.m_data = (void*)(&req);
#else
                                memset(&(req.myiocb), 0, sizeof(struct iocb));
                                req.myiocb.aio_buf = reinterpret_cast<uint64_t>(req.m_buffer);
                                req.myiocb.aio_data = reinterpret_cast<uintptr_t>(&req);
#endif
                            }
                        }
                    }
                    else {
                        m_diskRequests.resize(p_internalResultNum);
			            //SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "WorkSpace Init %d reqs\n", p_internalResultNum);
                        for (int pi = 0; pi < p_internalResultNum; pi++) {
                            auto& req = m_diskRequests[pi];

                            req.m_buffer = (char*)(m_pageBuffers[pi].GetBuffer());
                            req.m_extension = &m_processIocp;
#ifdef _MSC_VER
                            memset(&(req.myres.m_col), 0, sizeof(OVERLAPPED));
                            req.myres.m_col.m_data = (void*)(&req);
#else
                            memset(&(req.myiocb), 0, sizeof(struct iocb));
                            req.myiocb.aio_buf = reinterpret_cast<uint64_t>(req.m_buffer);
                            req.myiocb.aio_data = reinterpret_cast<uintptr_t>(&req);
#endif
                        }
                    }
                }

                m_enableDataCompression = enableDataCompression;
                if (enableDataCompression) {
                    m_decompressBuffer.ReservePageBuffer(p_maxPages);
                }
            }

            std::vector<int> m_postingIDs;

            COMMON::OptHashPosVector m_deduper;

            Helper::RequestQueue m_processIocp;

            std::vector<Helper::PageBuffer<std::uint8_t>> m_pageBuffers;

            bool m_blockIO = false;

            bool m_enableDataCompression = false;

            Helper::PageBuffer<std::uint8_t> m_decompressBuffer;

            std::vector<Helper::AsyncReadRequest> m_diskRequests;

            int m_ri = 0;

            int m_pi = 0;

            int m_offset = 0;

            bool m_loadPosting = false;

            bool m_relaxedMono = false;

            int m_loadedPostingNum = 0;

            std::function<bool(const ByteArray&)> m_filterFunc;

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

            virtual bool LoadIndex(Options& p_options, COMMON::VersionLabel& p_versionMap, COMMON::Dataset<std::uint64_t>& m_vectorTranslateMap,  std::shared_ptr<VectorIndex> m_index) = 0;

            virtual ErrorCode SearchIndex(ExtraWorkSpace* p_exWorkSpace,
                QueryResult& p_queryResults,
                std::shared_ptr<VectorIndex> p_index,
                SearchStats* p_stats, std::set<int>* truth = nullptr, std::map<int, std::set<int>>* found = nullptr) = 0;

            virtual ErrorCode SearchIterativeNext(ExtraWorkSpace* p_exWorkSpace, QueryResult& p_headResults,
                QueryResult& p_queryResults,
                std::shared_ptr<VectorIndex> p_index, const VectorIndex* p_spann) = 0;

            virtual ErrorCode SearchIndexWithoutParsing(ExtraWorkSpace* p_exWorkSpace) = 0;

            virtual ErrorCode SearchNextInPosting(ExtraWorkSpace* p_exWorkSpace, QueryResult& p_headResults,
                QueryResult& p_queryResults,
                std::shared_ptr<VectorIndex>& p_index, const VectorIndex* p_spann) = 0;

            virtual bool BuildIndex(std::shared_ptr<Helper::VectorSetReader>& p_reader, 
                std::shared_ptr<VectorIndex> p_index, 
                Options& p_opt, COMMON::VersionLabel& p_versionMap, COMMON::Dataset<std::uint64_t>& p_vectorTranslateMap, SizeType upperBound = -1) = 0;

            virtual void InitWorkSpace(ExtraWorkSpace* p_exWorkSpace, bool clear = false) = 0;

            virtual ErrorCode GetPostingDebug(ExtraWorkSpace* p_exWorkSpace, std::shared_ptr<VectorIndex> p_index, SizeType vid, std::vector<SizeType>& VIDs, std::shared_ptr<VectorSet>& vecs) = 0;
            
            virtual ErrorCode RefineIndex(std::shared_ptr<VectorIndex>& p_index, bool p_prereassign = true,
                                          std::vector<SizeType> *p_headmapping = nullptr,
                                          std::vector<SizeType> *p_mapping = nullptr)
            {
                return ErrorCode::Undefined;
            }
            virtual ErrorCode AddIndex(ExtraWorkSpace* p_exWorkSpace, std::shared_ptr<VectorSet>& p_vectorSet,
                std::shared_ptr<VectorIndex> p_index, SizeType p_begin) { return ErrorCode::Undefined; }
            virtual ErrorCode DeleteIndex(SizeType p_id) { return ErrorCode::Undefined; }

            virtual bool AllFinished() { return false; }
            virtual void GetDBStats() { return; }
            virtual int64_t GetNumBlocks() { return 0; }
            virtual void GetIndexStats(int finishedInsert, bool cost, bool reset) { return; }
            virtual void ForceCompaction() { return; }

            virtual bool CheckValidPosting(SizeType postingID) = 0;
            virtual ErrorCode CheckPosting(SizeType postingiD, std::vector<std::uint8_t> *visited = nullptr,
                                           ExtraWorkSpace *p_exWorkSpace = nullptr) = 0;
            virtual SizeType SearchVector(ExtraWorkSpace* p_exWorkSpace, std::shared_ptr<VectorSet>& p_vectorSet,
                std::shared_ptr<VectorIndex> p_index, int testNum = 64, SizeType VID = -1) { return -1; }
            virtual void ForceGC(ExtraWorkSpace* p_exWorkSpace, VectorIndex* p_index) { return; }

            virtual ErrorCode GetWritePosting(ExtraWorkSpace *p_exWorkSpace, SizeType pid, std::string &posting,
                                              bool write = false)
            {
                return ErrorCode::Undefined;
            }

            virtual ErrorCode Checkpoint(std::string prefix) { return ErrorCode::Success; }
        };
    } // SPANN
} // SPTAG

#endif // _SPTAG_SPANN_IEXTRASEARCHER_H_
