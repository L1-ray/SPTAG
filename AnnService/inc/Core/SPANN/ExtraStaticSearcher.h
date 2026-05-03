// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef _SPTAG_SPANN_EXTRASTATICSEARCHER_H_
#define _SPTAG_SPANN_EXTRASTATICSEARCHER_H_

#include "Compressor.h"
#include "IExtraSearcher.h"
#include "PageCache.h"
#include "inc/Core/Common/IQuantizer.h"
#include "inc/Core/Common/SIMDUtils.h"
#include "inc/Core/Common/TruthSet.h"
#include "inc/Helper/AsyncFileReader.h"
#include "inc/Helper/SimpleIniReader.h"
#include "inc/Helper/VectorSetReader.h"

#include <cctype>
#include <climits>
#include <cmath>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace SPTAG
{
namespace SPANN
{
extern std::function<std::shared_ptr<Helper::DiskIO>(void)> f_createAsyncIO;

struct Selection
{
    std::string m_tmpfile;
    size_t m_totalsize;
    size_t m_start;
    size_t m_end;
    std::vector<Edge> m_selections;
    static EdgeCompare g_edgeComparer;

    Selection(size_t totalsize, std::string tmpdir)
        : m_tmpfile(tmpdir + FolderSep + "selection_tmp"), m_totalsize(totalsize), m_start(0), m_end(totalsize)
    {
        remove(m_tmpfile.c_str());
        m_selections.resize(totalsize);
    }

    ErrorCode SaveBatch()
    {
        auto f_out = f_createIO();
        if (f_out == nullptr ||
            !f_out->Initialize(m_tmpfile.c_str(),
                               std::ios::out | std::ios::binary | (fileexists(m_tmpfile.c_str()) ? std::ios::in : 0)))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Cannot open %s to save selection for batching!\n",
                         m_tmpfile.c_str());
            return ErrorCode::FailedOpenFile;
        }
        if (f_out->WriteBinary(sizeof(Edge) * (m_end - m_start), (const char *)m_selections.data(),
                               sizeof(Edge) * m_start) != sizeof(Edge) * (m_end - m_start))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Cannot write to %s!\n", m_tmpfile.c_str());
            return ErrorCode::DiskIOFail;
        }
        std::vector<Edge> batch_selection;
        m_selections.swap(batch_selection);
        m_start = m_end = 0;
        return ErrorCode::Success;
    }

    ErrorCode LoadBatch(size_t start, size_t end)
    {
        auto f_in = f_createIO();
        if (f_in == nullptr || !f_in->Initialize(m_tmpfile.c_str(), std::ios::in | std::ios::binary))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Cannot open %s to load selection batch!\n", m_tmpfile.c_str());
            return ErrorCode::FailedOpenFile;
        }

        size_t readsize = end - start;
        m_selections.resize(readsize);
        if (f_in->ReadBinary(readsize * sizeof(Edge), (char *)m_selections.data(), start * sizeof(Edge)) !=
            readsize * sizeof(Edge))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Cannot read from %s! start:%zu size:%zu\n", m_tmpfile.c_str(),
                         start, readsize);
            return ErrorCode::DiskIOFail;
        }
        m_start = start;
        m_end = end;
        return ErrorCode::Success;
    }

    size_t lower_bound(SizeType node)
    {
        auto ptr = std::lower_bound(m_selections.begin(), m_selections.end(), node, g_edgeComparer);
        return m_start + (ptr - m_selections.begin());
    }

    Edge &operator[](size_t offset)
    {
        if (offset < m_start || offset >= m_end)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Error read offset in selections:%zu\n", offset);
        }
        return m_selections[offset - m_start];
    }
};

struct PostingFormatMetadata
{
    bool m_exists = false;
    int m_formatVersion = 0;
    std::string m_layoutType = "legacy";
    std::string m_codeType = "None";
    std::string m_chunkPruneMode = "None";
    std::string m_payloadLayout = "legacy_full_vector";
};

struct NewPostingHeader
{
    uint32_t m_magic = 0x53504732; // "SPG2"
    uint32_t m_version = 2;
    uint32_t m_chunkCount = 1;
    uint32_t m_recordCount = 0;
    uint32_t m_codeRecordBytes = 0;
    uint32_t m_codeBytes = 0;
    uint32_t m_payloadRecordBytes = 0;
    uint32_t m_payloadBytes = 0;
};

struct NewPostingChunkDirectoryEntry
{
    uint32_t m_codeOffset = 0;
    uint32_t m_codeBytes = 0;
    uint32_t m_payloadOffset = 0;
    uint32_t m_payloadBytes = 0;
    uint32_t m_payloadRecordBytes = 0;
    uint32_t m_recordCount = 0;
};

struct NewPostingChunkDirectoryEntryV2
{
    uint32_t m_centroidOffset = 0;
    uint32_t m_centroidBytes = 0;
    float m_radius = 0.0f;
    uint32_t m_reserved = 0;
    uint32_t m_codeOffset = 0;
    uint32_t m_codeBytes = 0;
    uint32_t m_payloadOffset = 0;
    uint32_t m_payloadBytes = 0;
    uint32_t m_payloadRecordBytes = 0;
    uint32_t m_recordCount = 0;
};

struct NewPostingCodeRecordPrefix
{
    int m_vectorID = -1;
    uint32_t m_payloadOffset = 0;
};

enum class PostingPayloadLayoutKind
{
    FullVector,
    PQCode,
    ChunkLocality,
    CoHit,
    Invalid
};

#define DecompressPosting()                                                                                            \
    {                                                                                                                  \
        p_postingListFullData = (char *)p_exWorkSpace->m_decompressBuffer.GetBuffer();                                 \
        if (listInfo->listEleCount != 0)                                                                               \
        {                                                                                                              \
            std::size_t sizePostingListFullData;                                                                       \
            try                                                                                                        \
            {                                                                                                          \
                sizePostingListFullData = m_pCompressor->Decompress(                                                   \
                    buffer + listInfo->pageOffset, listInfo->listTotalBytes, p_postingListFullData,                    \
                    listInfo->listEleCount * m_vectorInfoSize, m_enableDictTraining);                                  \
            }                                                                                                          \
            catch (std::runtime_error & err)                                                                           \
            {                                                                                                          \
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Decompress postingList %d  failed! %s, \n",                  \
                             listInfo - m_listInfos.data(), err.what());                                               \
                return;                                                                                                \
            }                                                                                                          \
            if (sizePostingListFullData != listInfo->listEleCount * m_vectorInfoSize)                                  \
            {                                                                                                          \
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "PostingList %d decompressed size not match! %zu, %d, \n",    \
                             listInfo - m_listInfos.data(), sizePostingListFullData,                                   \
                             listInfo->listEleCount * m_vectorInfoSize);                                               \
                return;                                                                                                \
            }                                                                                                          \
        }                                                                                                              \
    }

#define DecompressPostingIterative()                                                                                   \
    {                                                                                                                  \
        p_postingListFullData = (char *)p_exWorkSpace->m_decompressBuffer.GetBuffer();                                 \
        if (listInfo->listEleCount != 0)                                                                               \
        {                                                                                                              \
            std::size_t sizePostingListFullData;                                                                       \
            try                                                                                                        \
            {                                                                                                          \
                sizePostingListFullData = m_pCompressor->Decompress(                                                   \
                    buffer + listInfo->pageOffset, listInfo->listTotalBytes, p_postingListFullData,                    \
                    listInfo->listEleCount * m_vectorInfoSize, m_enableDictTraining);                                  \
                if (sizePostingListFullData != listInfo->listEleCount * m_vectorInfoSize)                              \
                {                                                                                                      \
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,                                                           \
                                 "PostingList %d decompressed size not match! %zu, %d, \n",                            \
                                 listInfo - m_listInfos.data(), sizePostingListFullData,                               \
                                 listInfo->listEleCount * m_vectorInfoSize);                                           \
                }                                                                                                      \
            }                                                                                                          \
            catch (std::runtime_error & err)                                                                           \
            {                                                                                                          \
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Decompress postingList %d  failed! %s, \n",                  \
                             listInfo - m_listInfos.data(), err.what());                                               \
            }                                                                                                          \
        }                                                                                                              \
    }

#define ProcessPosting()                                                                                               \
    for (int i = 0; i < listInfo->listEleCount; i++)                                                                   \
    {                                                                                                                  \
        uint64_t offsetVectorID, offsetVector;                                                                         \
        (this->*m_parsePosting)(offsetVectorID, offsetVector, i, listInfo->listEleCount);                              \
        int vectorID = *(reinterpret_cast<int *>(p_postingListFullData + offsetVectorID));                             \
        if (p_exWorkSpace->m_deduper.CheckAndSet(vectorID))                                                            \
        {                                                                                                              \
            listElements--;                                                                                            \
            continue;                                                                                                  \
        }                                                                                                              \
        (this->*m_parseEncoding)(p_index, listInfo, (ValueType *)(p_postingListFullData + offsetVector));              \
        auto distance2leaf =                                                                                           \
            p_index->ComputeDistance(queryResults.GetQuantizedTarget(), p_postingListFullData + offsetVector);         \
        queryResults.AddPoint(vectorID, distance2leaf);                                                                \
    }

#define ProcessPostingOffset()                                                                                         \
    while (p_exWorkSpace->m_offset < listInfo->listEleCount)                                                           \
    {                                                                                                                  \
        uint64_t offsetVectorID, offsetVector;                                                                         \
        (this->*m_parsePosting)(offsetVectorID, offsetVector, p_exWorkSpace->m_offset, listInfo->listEleCount);        \
        p_exWorkSpace->m_offset++;                                                                                     \
        int vectorID = *(reinterpret_cast<int *>(p_postingListFullData + offsetVectorID));                             \
        if (p_exWorkSpace->m_deduper.CheckAndSet(vectorID))                                                            \
            continue;                                                                                                  \
        if (p_exWorkSpace->m_filterFunc != nullptr && !p_exWorkSpace->m_filterFunc(p_spann->GetMetadata(vectorID)))    \
            continue;                                                                                                  \
        (this->*m_parseEncoding)(p_index, listInfo, (ValueType *)(p_postingListFullData + offsetVector));              \
        auto distance2leaf =                                                                                           \
            p_index->ComputeDistance(queryResults.GetQuantizedTarget(), p_postingListFullData + offsetVector);         \
        queryResults.AddPoint(vectorID, distance2leaf);                                                                \
        foundResult = true;                                                                                            \
        break;                                                                                                         \
    }                                                                                                                  \
    if (p_exWorkSpace->m_offset == listInfo->listEleCount)                                                             \
    {                                                                                                                  \
        p_exWorkSpace->m_pi++;                                                                                         \
        p_exWorkSpace->m_offset = 0;                                                                                   \
    }

template <typename ValueType> class ExtraStaticSearcher : public IExtraSearcher
{
  public:
    ExtraStaticSearcher()
    {
        m_enableDeltaEncoding = false;
        m_enablePostingListRearrange = false;
        m_enableDataCompression = false;
        m_enableDictTraining = true;
    }

    virtual ~ExtraStaticSearcher()
    {
    }

    virtual bool Available() override
    {
        return m_available;
    }

    bool IsTwoStagePostingFormat() const
    {
        return m_postingFormatMetadata.m_exists &&
               (Helper::StrUtils::StrEqualIgnoreCase(m_postingFormatMetadata.m_layoutType.c_str(), "twostage_v1") ||
                Helper::StrUtils::StrEqualIgnoreCase(m_postingFormatMetadata.m_layoutType.c_str(),
                                                     "chunked_twostage_v1"));
    }

    bool IsChunkedTwoStagePostingFormat() const
    {
        return m_postingFormatMetadata.m_exists &&
               Helper::StrUtils::StrEqualIgnoreCase(m_postingFormatMetadata.m_layoutType.c_str(),
                                                    "chunked_twostage_v1");
    }

    PostingPayloadLayoutKind GetPostingPayloadLayoutKind() const
    {
        if (m_opt == nullptr)
        {
            return PostingPayloadLayoutKind::FullVector;
        }

        const std::string layout = TrimCopy(m_opt->m_postingPayloadLayout);
        if (layout.empty() || Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "FullVector") ||
            Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "full_vector_payload_v1"))
        {
            return m_opt->m_enablePayloadReorderByCode ? PostingPayloadLayoutKind::PQCode
                                                       : PostingPayloadLayoutKind::FullVector;
        }
        if (Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "PQCode") ||
            Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "CodeSort") ||
            Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "pq_code_sort_v1"))
        {
            return PostingPayloadLayoutKind::PQCode;
        }
        if (Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "ChunkLocality") ||
            Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "chunk_locality_payload_v1"))
        {
            return PostingPayloadLayoutKind::ChunkLocality;
        }
        if (Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "CoHit") ||
            Helper::StrUtils::StrEqualIgnoreCase(layout.c_str(), "cohit_payload_v1"))
        {
            return PostingPayloadLayoutKind::CoHit;
        }
        return PostingPayloadLayoutKind::Invalid;
    }

    std::string GetPostingPayloadLayoutName() const
    {
        switch (GetPostingPayloadLayoutKind())
        {
        case PostingPayloadLayoutKind::FullVector:
            return "full_vector_payload_v1";
        case PostingPayloadLayoutKind::PQCode:
            return "pq_code_sort_v1";
        case PostingPayloadLayoutKind::ChunkLocality:
            return "chunk_locality_payload_v1";
        case PostingPayloadLayoutKind::CoHit:
            return "cohit_payload_v1";
        default:
            return "invalid";
        }
    }

    uint64_t GetPostingPageKey(int postingID, uint32_t pageID) const
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(postingID)) << 32) | static_cast<uint64_t>(pageID);
    }

    uint64_t GetPostingChunkKey(int postingID, int chunkID) const
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(postingID)) << 32) |
               static_cast<uint64_t>(static_cast<uint32_t>(chunkID));
    }

    size_t GetInitialPostingMetadataReadBytes(uint32_t pageOffset, size_t totalBytes) const
    {
        const size_t minDirectoryBytes =
            std::max(sizeof(NewPostingChunkDirectoryEntry), sizeof(NewPostingChunkDirectoryEntryV2));
        const size_t minHeaderBytes = static_cast<size_t>(pageOffset) + sizeof(NewPostingHeader) + minDirectoryBytes;
        return std::min(totalBytes, AlignToPageSize(minHeaderBytes));
    }

    void InitWorkSpace(ExtraWorkSpace *p_exWorkSpace, bool clear = false) override
    {
        if (clear)
        {
            p_exWorkSpace->Clear(m_opt->m_searchInternalResultNum,
                                 max(m_opt->m_postingPageLimit, m_opt->m_searchPostingPageLimit + 1) << PageSizeEx,
                                 false, m_opt->m_enableDataCompression);
        }
        else
        {
            p_exWorkSpace->Initialize(m_opt->m_maxCheck, m_opt->m_hashExp, m_opt->m_searchInternalResultNum,
                                      max(m_opt->m_postingPageLimit, m_opt->m_searchPostingPageLimit + 1) << PageSizeEx,
                                      false, m_opt->m_enableDataCompression);
            int wid = m_workspaceCount.fetch_add(1);
            p_exWorkSpace->m_asyncChannel = wid;
            for (auto &req : p_exWorkSpace->m_diskRequests)
            {
                req.m_status = wid;
            }
            p_exWorkSpace->m_callback = nullptr;
        }
    }

    virtual bool LoadIndex(Options &p_opt, COMMON::VersionLabel &p_versionMap,
                           COMMON::Dataset<std::uint64_t> &p_vectorTranslateMap, std::shared_ptr<VectorIndex> m_index)
    {
        m_extraFullGraphFile = p_opt.m_indexDirectory + FolderSep + p_opt.m_ssdIndex;
        m_postingFormatMetadata = LoadPostingFormatMetadata(m_extraFullGraphFile);
        if (m_postingFormatMetadata.m_exists &&
            !Helper::StrUtils::StrEqualIgnoreCase(m_postingFormatMetadata.m_layoutType.c_str(), "legacy") &&
            !Helper::StrUtils::StrEqualIgnoreCase(m_postingFormatMetadata.m_layoutType.c_str(), "twostage_v1") &&
            !Helper::StrUtils::StrEqualIgnoreCase(m_postingFormatMetadata.m_layoutType.c_str(), "chunked_twostage_v1"))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Posting metadata %s declares unsupported layout %s.\n",
                         GetPostingMetadataFilePath(m_extraFullGraphFile).c_str(),
                         m_postingFormatMetadata.m_layoutType.c_str());
            return false;
        }

        // For two-stage posting format, load the quantizer for search
        if (IsTwoStagePostingFormat())
        {
            m_twoStageQuantizer = LoadQuantizerFromFile(p_opt.m_quantizerFilePath);
            if (m_twoStageQuantizer == nullptr)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Two-stage posting search requires a quantizer file. path=%s\n",
                             p_opt.m_quantizerFilePath.c_str());
                return false;
            }
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Loaded posting quantizer for two-stage search.\n");
        }

        std::string curFile = m_extraFullGraphFile;
        m_postingRuntimeMetadata.clear();
        p_opt.m_searchPostingPageLimit =
            max(p_opt.m_searchPostingPageLimit,
                static_cast<int>(
                    (p_opt.m_postingVectorLimit * (p_opt.m_dim * sizeof(ValueType) + sizeof(int)) + PageSize - 1) /
                    PageSize));
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Load index with posting page limit:%d\n",
                     p_opt.m_searchPostingPageLimit);
        do
        {
            auto curIndexFile = f_createAsyncIO();
            if (curIndexFile == nullptr ||
                !curIndexFile->Initialize(curFile.c_str(),
#ifndef _MSC_VER
#ifdef BATCH_READ
                                          O_RDONLY | O_DIRECT, p_opt.m_searchInternalResultNum, 2, 2,
                                          p_opt.m_iSSDNumberOfThreads
#else
                                          O_RDONLY | O_DIRECT,
                                          p_opt.m_searchInternalResultNum * p_opt.m_iSSDNumberOfThreads /
                                                  p_opt.m_ioThreads +
                                              1,
                                          2, 2, p_opt.m_ioThreads
#endif
#else
                                          GENERIC_READ, (p_opt.m_searchPostingPageLimit + 1) * PageSize, 2, 2,
                                          (std::uint16_t)p_opt.m_ioThreads
#endif
                                          ))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Cannot open file:%s!\n", curFile.c_str());
                return false;
            }

            m_indexFiles.emplace_back(curIndexFile);
            try
            {
                m_totalListCount += LoadingHeadInfo(curFile, p_opt.m_searchPostingPageLimit, m_listInfos);
            }
            catch (std::exception &e)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Error occurs when loading HeadInfo:%s\n", e.what());
                return false;
            }

            curFile = m_extraFullGraphFile + "_" + std::to_string(m_indexFiles.size());
        } while (fileexists(curFile.c_str()));
        m_oneContext = (m_indexFiles.size() == 1);

        if (IsTwoStagePostingFormat())
        {
            int maxPostingPages = 0;
            for (const auto &listInfo : m_listInfos)
            {
                maxPostingPages = std::max(maxPostingPages, static_cast<int>(listInfo.listPageCount));
            }
            p_opt.m_searchPostingPageLimit = std::max(p_opt.m_searchPostingPageLimit, maxPostingPages);
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                         "Two-stage posting max page count:%d search posting page limit:%d\n", maxPostingPages,
                         p_opt.m_searchPostingPageLimit);
        }

        m_opt = &p_opt;
        m_enableDeltaEncoding = p_opt.m_enableDeltaEncoding;
        m_enablePostingListRearrange = p_opt.m_enablePostingListRearrange;
        m_enableDataCompression = p_opt.m_enableDataCompression;
        m_enableDictTraining = p_opt.m_enableDictTraining;

        if (m_enablePostingListRearrange)
            m_parsePosting = &ExtraStaticSearcher<ValueType>::ParsePostingListRearrange;
        else
            m_parsePosting = &ExtraStaticSearcher<ValueType>::ParsePostingList;
        if (m_enableDeltaEncoding)
            m_parseEncoding = &ExtraStaticSearcher<ValueType>::ParseDeltaEncoding;
        else
            m_parseEncoding = &ExtraStaticSearcher<ValueType>::ParseEncoding;

        m_listPerFile = static_cast<int>((m_totalListCount + m_indexFiles.size() - 1) / m_indexFiles.size());

        p_versionMap.Load(p_opt.m_indexDirectory + FolderSep + p_opt.m_deleteIDFile, p_opt.m_datasetRowsInBlock,
                          p_opt.m_datasetCapacity);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Current vector num: %d.\n", p_versionMap.Count());

#ifndef _MSC_VER
        Helper::AIOTimeout.tv_nsec = p_opt.m_iotimeout * 1000;
#endif

        m_freeWorkSpaceIds.reset();
        m_workspaceCount = 0;
        m_available = true;
        return true;
    }

    virtual ErrorCode SearchIndex(ExtraWorkSpace *p_exWorkSpace, QueryResult &p_queryResults,
                                  std::shared_ptr<VectorIndex> p_index, SearchStats *p_stats, std::set<int> *truth,
                                  std::map<int, std::set<int>> *found)
    {
        const uint32_t postingListCount = static_cast<uint32_t>(p_exWorkSpace->m_postingIDs.size());

        COMMON::QueryResultSet<ValueType> &queryResults = *((COMMON::QueryResultSet<ValueType> *)&p_queryResults);

        if (IsTwoStagePostingFormat())
        {
            return SearchIndexNewFormat(p_exWorkSpace, queryResults, p_queryResults, p_index, p_stats, truth, found);
        }

        int diskRead = 0;
        int diskIO = 0;
        int listElements = 0;
        uint64_t requestedReadBytes = 0;
        uint64_t readPages = 0;
        uint64_t postingsTouched = 0;
        uint64_t postingElementsRaw = 0;
        uint64_t distanceEvaluatedCount = 0;
        uint64_t duplicateVectorCount = 0;
        double ioIssueLatencyMs = 0;
        double ioWaitLatencyMs = 0;
        double batchReadTotalLatencyMs = 0;
        double postingDecodeLatencyMs = 0;
        double postingParseLatencyMs = 0;
        double distanceCalcLatencyMs = 0;

        auto durationMs = [](const std::chrono::steady_clock::time_point &start,
                             const std::chrono::steady_clock::time_point &end) -> double {
            return std::chrono::duration<double, std::milli>(end - start).count();
        };

        auto decodePostingBuffer = [&](char *buffer, ListInfo *listInfo, int postingID) -> char * {
            char *postingListData = buffer + listInfo->pageOffset;
            if (!m_enableDataCompression || listInfo->listEleCount == 0)
            {
                return postingListData;
            }

            char *fullData = (char *)p_exWorkSpace->m_decompressBuffer.GetBuffer();
            auto decodeStart = std::chrono::steady_clock::now();
            std::size_t decompressedSize = 0;
            try
            {
                decompressedSize =
                    m_pCompressor->Decompress(buffer + listInfo->pageOffset, listInfo->listTotalBytes, fullData,
                                              listInfo->listEleCount * m_vectorInfoSize, m_enableDictTraining);
            }
            catch (std::runtime_error &err)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Decompress postingList %d failed! %s\n", postingID,
                             err.what());
                throw;
            }
            auto decodeEnd = std::chrono::steady_clock::now();
            postingDecodeLatencyMs += durationMs(decodeStart, decodeEnd);

            if (decompressedSize != listInfo->listEleCount * m_vectorInfoSize)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "PostingList %d decompressed size not match! %zu, %d\n",
                             postingID, decompressedSize, listInfo->listEleCount * m_vectorInfoSize);
                throw std::runtime_error("Posting decompressed size mismatch");
            }

            return fullData;
        };

        auto processPostingBuffer = [&](ListInfo *listInfo, char *postingListData, int postingID) {
            for (int i = 0; i < listInfo->listEleCount; ++i)
            {
                uint64_t offsetVectorID, offsetVector;

                auto parseStart = std::chrono::steady_clock::now();
                (this->*m_parsePosting)(offsetVectorID, offsetVector, i, listInfo->listEleCount);
                int vectorID = *(reinterpret_cast<int *>(postingListData + offsetVectorID));
                postingParseLatencyMs += durationMs(parseStart, std::chrono::steady_clock::now());

                if (p_exWorkSpace->m_deduper.CheckAndSet(vectorID))
                {
                    --listElements;
                    ++duplicateVectorCount;
                    continue;
                }

                auto parseEncodingStart = std::chrono::steady_clock::now();
                (this->*m_parseEncoding)(p_index, listInfo, (ValueType *)(postingListData + offsetVector));
                postingParseLatencyMs += durationMs(parseEncodingStart, std::chrono::steady_clock::now());

                auto distanceStart = std::chrono::steady_clock::now();
                auto distance2leaf =
                    p_index->ComputeDistance(queryResults.GetQuantizedTarget(), postingListData + offsetVector);
                queryResults.AddPoint(vectorID, distance2leaf);
                distanceCalcLatencyMs += durationMs(distanceStart, std::chrono::steady_clock::now());
                ++distanceEvaluatedCount;

                // S0: Record payload trace for legacy format
                if (p_stats != nullptr && m_opt != nullptr && m_opt->m_enablePayloadTrace)
                {
                    PayloadTraceRecord record;
                    record.m_postingID = postingID;
                    record.m_chunkID = 0;
                    record.m_vectorID = vectorID;
                    record.m_payloadPageID = static_cast<uint32_t>(listInfo->listOffset >> PageSizeEx);
                    record.m_payloadPageCount = listInfo->listPageCount;
                    record.m_payloadPhysicalOffset = listInfo->listOffset;
                    record.m_payloadBytes = m_vectorInfoSize;
                    record.m_coarseDist = distance2leaf;
                    p_stats->m_payloadTraceRecords.emplace_back(record);
                }
            }
        };

#if defined(ASYNC_READ) && !defined(BATCH_READ)
        int unprocessed = 0;
        std::vector<std::chrono::steady_clock::time_point> ioIssueStart(postingListCount);
        std::vector<bool> ioSubmitted(postingListCount, false);
#endif

        for (uint32_t pi = 0; pi < postingListCount; ++pi)
        {
            auto curPostingID = p_exWorkSpace->m_postingIDs[pi];
            ListInfo *listInfo = &(m_listInfos[curPostingID]);
            int fileid = m_oneContext ? 0 : curPostingID / m_listPerFile;

#ifndef BATCH_READ
            Helper::DiskIO *indexFile = m_indexFiles[fileid].get();
#endif

            diskRead += listInfo->listPageCount;
            diskIO += 1;
            listElements += listInfo->listEleCount;
            readPages += static_cast<uint64_t>(listInfo->listPageCount);
            postingsTouched += 1;
            postingElementsRaw += static_cast<uint64_t>(listInfo->listEleCount);

            size_t totalBytes = (static_cast<size_t>(listInfo->listPageCount) << PageSizeEx);
            requestedReadBytes += totalBytes;

#ifdef ASYNC_READ
            auto &request = p_exWorkSpace->m_diskRequests[pi];
            request.m_offset = listInfo->listOffset;
            request.m_readSize = totalBytes;
            request.m_status = (fileid << 16) | (request.m_status & 0xffff);
            request.m_payload = (void *)listInfo;
            request.m_success = false;

#ifdef BATCH_READ
            // M1 B1-B2: Read cache + async insert (NO admission for ablation)
            // B1: TryGet only covers single-page requests
            // B2: Async enqueue on I/O complete, drop if queue full
            // B3: DISABLED for this ablation - always cache single-page
            ShardedPageCache* pc = GetGlobalShardedPageCache();
            bool enableCache = (pc != nullptr && m_opt != nullptr && m_opt->m_enablePageCache);
            bool isSinglePage = (listInfo->listPageCount == 1);

            // B1: Check cache for single-page postings
            if (enableCache && isSinglePage)
            {
                uint32_t firstPageId = static_cast<uint32_t>(listInfo->listOffset >> PageSizeEx);
                PageCacheKey cacheKey(static_cast<uint32_t>(fileid), firstPageId, PageSize);

                char *buffer = (char *)((p_exWorkSpace->m_pageBuffers[pi]).GetBuffer());
                uint32_t bytesCopied = 0;
                uint64_t lockWaitNs = 0;
                if (pc->TryGet(cacheKey, buffer, &bytesCopied, &lockWaitNs))
                {
                    // Cache hit! Process directly without I/O
                    if (p_stats)
                    {
                        p_stats->m_cacheLockWaitMs += static_cast<double>(lockWaitNs) / 1000000.0;
                        p_stats->m_cacheHitCount++;
                    }
                    try
                    {
                        char *postingData = decodePostingBuffer(buffer, listInfo, curPostingID);
                        processPostingBuffer(listInfo, postingData, curPostingID);
                    }
                    catch (...)
                    {
                        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to decode/process posting %d from cache.\n", curPostingID);
                    }
                    // Mark this request as completed (no I/O needed)
                    // Set dummy callback to avoid bad_function_call in BatchReadFileAsync
                    request.m_readSize = 0;
                    request.m_callback = [](bool) {};
                    continue;
                }
                else if (p_stats)
                {
                    // Cache miss - still record lock wait time
                    p_stats->m_cacheLockWaitMs += static_cast<double>(lockWaitNs) / 1000000.0;
                    p_stats->m_cacheMissCount++;
                }
            }

            // B2: Cache all single-page postings (no admission control)
            // B3 disabled: Always cache single-page, no 2-hit check

            // Cache miss or multi-page posting: issue I/O with callback
            request.m_callback = [&request, &decodePostingBuffer, &processPostingBuffer, pc, enableCache, isSinglePage, fileid, this](bool success) {
                if (!success)
                    return;
                char *buffer = request.m_buffer;
                ListInfo *listInfo = (ListInfo *)(request.m_payload);
                int postingID = static_cast<int>(listInfo - m_listInfos.data());

                // B2: Async insert all single-page postings (no admission)
                if (enableCache && isSinglePage && pc != nullptr)
                {
                    uint32_t firstPageId = static_cast<uint32_t>(request.m_offset >> PageSizeEx);
                    PageCacheKey cacheKey(static_cast<uint32_t>(fileid), firstPageId, PageSize);
                    pc->AsyncInsert(cacheKey, buffer, PageSize);  // Drops if queue full
                }

                try
                {
                    char *postingData = decodePostingBuffer(buffer, listInfo, postingID);
                    processPostingBuffer(listInfo, postingData, postingID);
                }
                catch (...)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to decode/process posting %d.\n", postingID);
                }
            };
#else // async read
            request.m_callback = [&p_exWorkSpace, &request](bool success) {
                p_exWorkSpace->m_processIocp.push(&request);
            };

            ++unprocessed;
            ioIssueStart[pi] = std::chrono::steady_clock::now();
            if (!(indexFile->ReadFileAsync(request)))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read file!\n");
                unprocessed--;
            }
            else
            {
                ioSubmitted[pi] = true;
            }
            ioIssueLatencyMs += durationMs(ioIssueStart[pi], std::chrono::steady_clock::now());
#endif
#else // sync read
            char *buffer = (char *)((p_exWorkSpace->m_pageBuffers[pi]).GetBuffer());
            auto ioWaitStart = std::chrono::steady_clock::now();
            auto numRead = indexFile->ReadBinary(totalBytes, buffer, listInfo->listOffset);
            ioWaitLatencyMs += durationMs(ioWaitStart, std::chrono::steady_clock::now());
            if (numRead != totalBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, acutal: %llu.\n",
                             m_extraFullGraphFile.c_str(), totalBytes, numRead);
                throw std::runtime_error("File read mismatch");
            }
            try
            {
                char *postingData = decodePostingBuffer(buffer, listInfo, curPostingID);
                processPostingBuffer(listInfo, postingData, curPostingID);
            }
            catch (...)
            {
                return ErrorCode::DiskIOFail;
            }
#endif
        }

#ifdef ASYNC_READ
#ifdef BATCH_READ
        auto batchReadStart = std::chrono::steady_clock::now();
        bool batchReadSuccess =
            BatchReadFileAsync(m_indexFiles, (p_exWorkSpace->m_diskRequests).data(), postingListCount);
        batchReadTotalLatencyMs = durationMs(batchReadStart, std::chrono::steady_clock::now());
        if (!batchReadSuccess)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "BatchReadFileAsync failed!\n");
            return ErrorCode::DiskIOFail;
        }
#else
        while (unprocessed > 0)
        {
            Helper::AsyncReadRequest *request;
            if (!(p_exWorkSpace->m_processIocp.pop(request)))
                break;

            --unprocessed;
            char *buffer = request->m_buffer;
            ListInfo *listInfo = static_cast<ListInfo *>(request->m_payload);
            auto requestBase = p_exWorkSpace->m_diskRequests.data();
            ptrdiff_t requestOffset = request - requestBase;
            if (requestOffset >= 0 && static_cast<size_t>(requestOffset) < ioSubmitted.size() &&
                ioSubmitted[requestOffset])
            {
                ioWaitLatencyMs += durationMs(ioIssueStart[requestOffset], std::chrono::steady_clock::now());
            }
            int postingID = static_cast<int>(listInfo - m_listInfos.data());
            try
            {
                char *postingData = decodePostingBuffer(buffer, listInfo, postingID);
                processPostingBuffer(listInfo, postingData, postingID);
            }
            catch (...)
            {
                return ErrorCode::DiskIOFail;
            }
        }
#endif
#endif
        if (truth)
        {
            for (uint32_t pi = 0; pi < postingListCount; ++pi)
            {
                auto curPostingID = p_exWorkSpace->m_postingIDs[pi];

                ListInfo *listInfo = &(m_listInfos[curPostingID]);
                char *buffer = (char *)((p_exWorkSpace->m_pageBuffers[pi]).GetBuffer());

                char *p_postingListFullData = buffer + listInfo->pageOffset;
                if (m_enableDataCompression)
                {
                    p_postingListFullData = (char *)p_exWorkSpace->m_decompressBuffer.GetBuffer();
                    if (listInfo->listEleCount != 0)
                    {
                        try
                        {
                            m_pCompressor->Decompress(buffer + listInfo->pageOffset, listInfo->listTotalBytes,
                                                      p_postingListFullData, listInfo->listEleCount * m_vectorInfoSize,
                                                      m_enableDictTraining);
                        }
                        catch (std::runtime_error &err)
                        {
                            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Decompress postingList %d  failed! %s, \n",
                                         curPostingID, err.what());
                            continue;
                        }
                    }
                }

                for (size_t i = 0; i < listInfo->listEleCount; ++i)
                {
                    uint64_t offsetVectorID =
                        m_enablePostingListRearrange
                            ? (m_vectorInfoSize - sizeof(int)) * listInfo->listEleCount + sizeof(int) * i
                            : m_vectorInfoSize * i;
                    int vectorID = *(reinterpret_cast<int *>(p_postingListFullData + offsetVectorID));
                    if (truth && truth->count(vectorID) && found)
                        (*found)[curPostingID].insert(vectorID);
                }
            }
        }

        if (p_stats)
        {
            p_stats->m_totalListElementsCount = listElements;
            p_stats->m_diskIOCount = diskIO;
            p_stats->m_diskAccessCount = diskRead;
            p_stats->m_requestedReadBytes = requestedReadBytes;
            p_stats->m_readPages = readPages;
            p_stats->m_postingsTouched = postingsTouched;
            p_stats->m_postingElementsRaw = postingElementsRaw;
            p_stats->m_distanceEvaluatedCount = distanceEvaluatedCount;
            p_stats->m_duplicateVectorCount = duplicateVectorCount;
            p_stats->m_ioIssueLatencyMs = ioIssueLatencyMs;
            p_stats->m_ioWaitLatencyMs = ioWaitLatencyMs;
            p_stats->m_batchReadTotalLatencyMs = batchReadTotalLatencyMs;
            p_stats->m_postingDecodeLatencyMs = postingDecodeLatencyMs;
            p_stats->m_postingParseLatencyMs = postingParseLatencyMs;
            p_stats->m_distanceCalcLatencyMs = distanceCalcLatencyMs;
            p_stats->m_compLatency = distanceCalcLatencyMs;
            p_stats->m_diskReadLatency = (ioWaitLatencyMs > 0.0) ? ioWaitLatencyMs : batchReadTotalLatencyMs;
        }
        queryResults.SetScanned(listElements);
        return ErrorCode::Success;
    }

    ErrorCode SearchIndexNewFormat(ExtraWorkSpace *p_exWorkSpace, COMMON::QueryResultSet<ValueType> &queryResults,
                                   QueryResult &p_queryResults, std::shared_ptr<VectorIndex> p_index,
                                   SearchStats *p_stats, std::set<int> *truth, std::map<int, std::set<int>> *found)
    {
        p_exWorkSpace->m_coarseCandidates.clear();
        p_exWorkSpace->m_mergedCandidates.clear();
        p_exWorkSpace->m_payloadReadRequests.clear();
        if (p_stats)
        {
            p_stats->m_coarseCandidateCount = 0;
            p_stats->m_coarseCandidateCountAfterDedupe = 0;
            p_stats->m_rerankCandidateCount = 0;
        }

        // P1: Per-phase timing
        auto phaseStart = std::chrono::steady_clock::now();
        auto phaseEnd = phaseStart;

        ErrorCode ret = ReadPostingHeaderAndDirectory(p_exWorkSpace, queryResults, p_stats);
        if (ret != ErrorCode::Success)
            return ret;
        if (p_stats)
        {
            phaseEnd = std::chrono::steady_clock::now();
            p_stats->m_readHeaderDirMs += std::chrono::duration<double, std::milli>(phaseEnd - phaseStart).count();
            phaseStart = phaseEnd;
        }

        ret = ScanCompactCodes(p_exWorkSpace, queryResults, p_index, p_stats, truth, found);
        if (ret != ErrorCode::Success)
            return ret;
        if (p_stats)
        {
            phaseEnd = std::chrono::steady_clock::now();
            p_stats->m_scanCompactCodesMs += std::chrono::duration<double, std::milli>(phaseEnd - phaseStart).count();
            phaseStart = phaseEnd;
        }

        ret = MergeCoarseCandidates(p_exWorkSpace, p_stats);
        if (ret != ErrorCode::Success)
            return ret;
        if (p_stats)
        {
            phaseEnd = std::chrono::steady_clock::now();
            p_stats->m_mergeCoarseCandidatesMs +=
                std::chrono::duration<double, std::milli>(phaseEnd - phaseStart).count();
            phaseStart = phaseEnd;
        }

        ret = BuildPayloadReadPlan(p_exWorkSpace, p_stats);
        if (ret != ErrorCode::Success)
            return ret;
        if (p_stats)
        {
            phaseEnd = std::chrono::steady_clock::now();
            p_stats->m_buildPayloadReadPlanMs +=
                std::chrono::duration<double, std::milli>(phaseEnd - phaseStart).count();
            phaseStart = phaseEnd;
        }

        ret = FetchPayloadPagesAndRerank(p_exWorkSpace, queryResults, p_queryResults, p_index, p_stats);
        if (ret != ErrorCode::Success)
            return ret;
        if (p_stats)
        {
            phaseEnd = std::chrono::steady_clock::now();
            p_stats->m_fetchPayloadAndRerankMs +=
                std::chrono::duration<double, std::milli>(phaseEnd - phaseStart).count();
        }

        if (p_stats)
        {
            p_stats->m_totalListElementsCount = static_cast<int>(p_stats->m_postingElementsRaw);
            p_stats->m_rerankCandidateCount = p_exWorkSpace->m_mergedCandidates.size();
            p_stats->m_finalResultCount = static_cast<uint64_t>(p_queryResults.GetResultNum());
        }

        // P2: Compute miss-case attribution if truth is provided
        if (truth != nullptr && p_stats != nullptr)
        {
            ComputeMissCaseAttribution(p_exWorkSpace, queryResults, p_queryResults, *truth, p_stats);
        }

        return ErrorCode::Success;
    }

    virtual ErrorCode SearchIndexWithoutParsing(ExtraWorkSpace *p_exWorkSpace) override
    {
        if (IsTwoStagePostingFormat())
        {
            return ErrorCode::Undefined;
        }

        const uint32_t postingListCount = static_cast<uint32_t>(p_exWorkSpace->m_postingIDs.size());

        int diskRead = 0;
        int diskIO = 0;
        int listElements = 0;

#if defined(ASYNC_READ) && !defined(BATCH_READ)
        int unprocessed = 0;
#endif

        for (uint32_t pi = 0; pi < postingListCount; ++pi)
        {
            auto curPostingID = p_exWorkSpace->m_postingIDs[pi];
            ListInfo *listInfo = &(m_listInfos[curPostingID]);
            int fileid = m_oneContext ? 0 : curPostingID / m_listPerFile;

#ifndef BATCH_READ
            Helper::DiskIO *indexFile = m_indexFiles[fileid].get();
#endif

            diskRead += listInfo->listPageCount;
            diskIO += 1;
            listElements += listInfo->listEleCount;

            size_t totalBytes = (static_cast<size_t>(listInfo->listPageCount) << PageSizeEx);

#ifdef ASYNC_READ
            auto &request = p_exWorkSpace->m_diskRequests[pi];
            request.m_offset = listInfo->listOffset;
            request.m_readSize = totalBytes;
            request.m_status = (fileid << 16) | (request.m_status & 0xffff);
            request.m_payload = (void *)listInfo;
            request.m_success = false;

#ifdef BATCH_READ // async batch read
            request.m_callback = [this](bool success){
                        //char* buffer = request.m_buffer;
                        //ListInfo* listInfo = (ListInfo*)(request.m_payload);

                        // decompress posting list
                        /*
                        char* p_postingListFullData = buffer + listInfo->pageOffset;
                        if (m_enableDataCompression)
                        {
                            DecompressPosting();
                        }

                        ProcessPosting();
                        */};
#else // async read
            request.m_callback = [&p_exWorkSpace, &request](bool success) {
                p_exWorkSpace->m_processIocp.push(&request);
            };

            ++unprocessed;
            if (!(indexFile->ReadFileAsync(request)))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read file!\n");
                unprocessed--;
            }
#endif
#else // sync read
            char *buffer = (char *)((p_exWorkSpace->m_pageBuffers[pi]).GetBuffer());
            auto numRead = indexFile->ReadBinary(totalBytes, buffer, listInfo->listOffset);
            if (numRead != totalBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, acutal: %llu.\n",
                             m_extraFullGraphFile.c_str(), totalBytes, numRead);
                return ErrorCode::DiskIOFail;
            }
            // decompress posting list
            /*
            char* p_postingListFullData = buffer + listInfo->pageOffset;
            if (m_enableDataCompression)
            {
                DecompressPosting();
            }

            ProcessPosting();
            */
#endif
        }

#ifdef ASYNC_READ
#ifdef BATCH_READ
        int retry = 0;
        bool success = false;
        while (retry < 2 && !success)
        {
            success = BatchReadFileAsync(m_indexFiles, (p_exWorkSpace->m_diskRequests).data(), postingListCount);
            retry++;
        }
#else
        while (unprocessed > 0)
        {
            Helper::AsyncReadRequest *request;
            if (!(p_exWorkSpace->m_processIocp.pop(request)))
                break;

            --unprocessed;
            char *buffer = request->m_buffer;
            ListInfo *listInfo = static_cast<ListInfo *>(request->m_payload);
            // decompress posting list
            /*
            char* p_postingListFullData = buffer + listInfo->pageOffset;
            if (m_enableDataCompression)
            {
                DecompressPosting();
            }

            ProcessPosting();
            */
        }
#endif
#endif
        return (success) ? ErrorCode::Success : ErrorCode::DiskIOFail;
    }

    virtual ErrorCode SearchNextInPosting(ExtraWorkSpace *p_exWorkSpace, QueryResult &p_headResults,
                                          QueryResult &p_queryResults, std::shared_ptr<VectorIndex> &p_index,
                                          const VectorIndex *p_spann) override
    {
        COMMON::QueryResultSet<ValueType> &headResults = *((COMMON::QueryResultSet<ValueType> *)&p_headResults);
        COMMON::QueryResultSet<ValueType> &queryResults = *((COMMON::QueryResultSet<ValueType> *)&p_queryResults);
        bool foundResult = false;
        BasicResult *head = headResults.GetResult(p_exWorkSpace->m_ri);
        while (!foundResult && p_exWorkSpace->m_pi < p_exWorkSpace->m_postingIDs.size())
        {
            if (head && head->VID != -1 && p_exWorkSpace->m_ri <= p_exWorkSpace->m_pi &&
                (p_exWorkSpace->m_filterFunc == nullptr ||
                 p_exWorkSpace->m_filterFunc(p_spann->GetMetadata(head->VID))))
            {
                queryResults.AddPoint(head->VID, head->Dist);
                head = headResults.GetResult(++p_exWorkSpace->m_ri);
                foundResult = true;
                continue;
            }
            char *buffer = (char *)((p_exWorkSpace->m_pageBuffers[p_exWorkSpace->m_pi]).GetBuffer());
            ListInfo *listInfo = static_cast<ListInfo *>(p_exWorkSpace->m_diskRequests[p_exWorkSpace->m_pi].m_payload);
            // decompress posting list
            char *p_postingListFullData = buffer + listInfo->pageOffset;
            if (m_enableDataCompression && p_exWorkSpace->m_offset == 0)
            {
                DecompressPostingIterative();
            }
            ProcessPostingOffset();
        }
        if (!foundResult && head && head->VID != -1 &&
            (p_exWorkSpace->m_filterFunc == nullptr || p_exWorkSpace->m_filterFunc(p_spann->GetMetadata(head->VID))))
        {
            queryResults.AddPoint(head->VID, head->Dist);
            head = headResults.GetResult(++p_exWorkSpace->m_ri);
            foundResult = true;
        }
        if (foundResult)
            p_queryResults.SetScanned(p_queryResults.GetScanned() + 1);
        return (foundResult) ? ErrorCode::Success : ErrorCode::VectorNotFound;
    }

    virtual ErrorCode SearchIterativeNext(ExtraWorkSpace *p_exWorkSpace, QueryResult &p_headResults,
                                          QueryResult &p_query, std::shared_ptr<VectorIndex> p_index,
                                          const VectorIndex *p_spann) override
    {
        if (IsTwoStagePostingFormat())
        {
            return ErrorCode::Undefined;
        }

        if (p_exWorkSpace->m_loadPosting)
        {
            ErrorCode ret = SearchIndexWithoutParsing(p_exWorkSpace);
            if (ret != ErrorCode::Success)
                return ret;
            p_exWorkSpace->m_ri = 0;
            p_exWorkSpace->m_pi = 0;
            p_exWorkSpace->m_offset = 0;
            p_exWorkSpace->m_loadPosting = false;
        }

        return SearchNextInPosting(p_exWorkSpace, p_headResults, p_query, p_index, p_spann);
    }

    bool CanUseHeuristicChunkPrune(const COMMON::QueryResultSet<ValueType> &queryResults) const
    {
        if (!IsChunkedTwoStagePostingFormat())
            return false;
        if (m_opt == nullptr)
            return false;
        if (m_opt->m_distCalcMethod != DistCalcMethod::L2)
            return false;
        if (!IsHeuristicChunkPruneMode())
            return false;
        // The current pruning threshold comes from the head-search result set, so it is a heuristic bound rather
        // than an exact safe upper bound for payload-level exact rerank.
        return queryResults.worstDist() < MaxDist;
    }

    static std::string TrimCopy(const std::string &value)
    {
        size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
            ++begin;
        size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
            --end;
        return value.substr(begin, end - begin);
    }

    bool IsHeuristicChunkPruneMode() const
    {
        if (m_opt == nullptr)
            return false;

        std::string pruneMode = TrimCopy(m_opt->m_postingChunkPruneMode);
        if (pruneMode.empty() || Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "None") ||
            Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "Off") ||
            Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "False") ||
            Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "0"))
        {
            return false;
        }

        return Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "L2") ||
               Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "Hard") ||
               Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "Heuristic") ||
               Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "HeuristicL2") ||
               Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "True") ||
               Helper::StrUtils::StrEqualIgnoreCase(pruneMode.c_str(), "1");
    }

    std::string NormalizeChunkPruneMode() const
    {
        return IsHeuristicChunkPruneMode() ? "HeuristicL2" : "None";
    }

    static uint64_t HashMix(uint64_t seed, uint64_t value)
    {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    }

    static uint64_t HashFloatBits(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return static_cast<uint64_t>(bits);
    }

    float ComputeChunkL2LowerBound(const COMMON::QueryResultSet<ValueType> &queryResults, const ValueType *centroid,
                                   float radius) const
    {
        const ValueType *target = queryResults.GetTarget();
        if (target == nullptr || centroid == nullptr)
            return 0.0f;

        auto distanceFunc = COMMON::DistanceCalcSelector<ValueType>(DistCalcMethod::L2);
        float centroidDistance = distanceFunc(target, centroid, m_iDataDimension);
        float euclideanLowerBound = std::max(0.0f, std::sqrt(std::max(0.0f, centroidDistance)) - radius);
        return euclideanLowerBound * euclideanLowerBound;
    }

    ErrorCode ReadChunkCodeBlocks(ExtraWorkSpace *p_exWorkSpace, SearchStats *p_stats)
    {
        if (p_exWorkSpace->m_postingBlocks.empty())
        {
            return ErrorCode::Success;
        }

        auto accountLogicalCodeBytes = [&]() {
            if (p_stats == nullptr)
            {
                return;
            }
            for (const auto &block : p_exWorkSpace->m_postingBlocks)
            {
                p_stats->m_codeBytesRead += static_cast<uint64_t>(block.m_codeBytes);
            }
        };

        bool allCached = true;
        for (auto &block : p_exWorkSpace->m_postingBlocks)
        {
            if (block.m_cachedCode == nullptr)
            {
                allCached = false;
            }
            else
            {
                block.m_diskRequestIndex = 0;
                block.m_codeBufferOffset = 0;
            }
        }
        if (allCached)
        {
            accountLogicalCodeBytes();
            return ErrorCode::Success;
        }

        std::vector<size_t> blockOrder(p_exWorkSpace->m_postingBlocks.size());
        std::iota(blockOrder.begin(), blockOrder.end(), 0);
        std::sort(blockOrder.begin(), blockOrder.end(), [this, p_exWorkSpace](size_t lhsIndex, size_t rhsIndex) {
            const PostingBlockInfo &lhs = p_exWorkSpace->m_postingBlocks[lhsIndex];
            const PostingBlockInfo &rhs = p_exWorkSpace->m_postingBlocks[rhsIndex];
            const int lhsFileId = m_oneContext ? 0 : lhs.m_postingID / m_listPerFile;
            const int rhsFileId = m_oneContext ? 0 : rhs.m_postingID / m_listPerFile;
            if (lhsFileId != rhsFileId)
                return lhsFileId < rhsFileId;

            const ListInfo &lhsListInfo = m_listInfos[lhs.m_postingID];
            const ListInfo &rhsListInfo = m_listInfos[rhs.m_postingID];
            const uint64_t lhsOffset =
                lhsListInfo.listOffset + static_cast<uint64_t>(lhsListInfo.pageOffset) + lhs.m_codeOffset;
            const uint64_t rhsOffset =
                rhsListInfo.listOffset + static_cast<uint64_t>(rhsListInfo.pageOffset) + rhs.m_codeOffset;
            if (lhsOffset != rhsOffset)
                return lhsOffset < rhsOffset;
            return lhs.m_chunkID < rhs.m_chunkID;
        });

        size_t maxReadBytes = 0;
        for (const auto &block : p_exWorkSpace->m_postingBlocks)
        {
            maxReadBytes = std::max(maxReadBytes, static_cast<size_t>(block.m_codeBytes) + PageSize * 2);
        }
        p_exWorkSpace->EnsureChunkCodeRequestCapacity(blockOrder.size(), maxReadBytes);

        for (size_t requestIdx = 0; requestIdx < blockOrder.size(); ++requestIdx)
        {
            PostingBlockInfo &block = p_exWorkSpace->m_postingBlocks[blockOrder[requestIdx]];
            if (block.m_cachedCode != nullptr)
            {
                continue;
            }
            const ListInfo &listInfo = m_listInfos[block.m_postingID];
            const int fileid = m_oneContext ? 0 : block.m_postingID / m_listPerFile;
            const uint64_t codePhysicalOffset =
                listInfo.listOffset + static_cast<uint64_t>(listInfo.pageOffset) + block.m_codeOffset;
            const uint64_t alignedCodeOffset = codePhysicalOffset & ~static_cast<uint64_t>(PageSize - 1);
            const uint32_t codeBufferOffset = static_cast<uint32_t>(codePhysicalOffset - alignedCodeOffset);
            const uint32_t codeReadBytes = static_cast<uint32_t>(
                ((static_cast<uint64_t>(codeBufferOffset) + block.m_codeBytes + PageSize - 1) >> PageSizeEx)
                << PageSizeEx);
            if (static_cast<uint64_t>(codeBufferOffset) + block.m_codeBytes > codeReadBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d chunk %d has invalid aligned code read plan. codeOffset=%u codeBytes=%u "
                             "bufferOffset=%u readBytes=%u\n",
                             block.m_postingID, block.m_chunkID, block.m_codeOffset, block.m_codeBytes,
                             codeBufferOffset, codeReadBytes);
                return ErrorCode::Fail;
            }
            auto &diskRequest = p_exWorkSpace->m_chunkCodeDiskRequests[requestIdx];
            diskRequest.m_offset = alignedCodeOffset;
            diskRequest.m_readSize = codeReadBytes;
            diskRequest.m_status = (fileid << 16) | (diskRequest.m_status & 0xffff);
            diskRequest.m_payload = &block;
            diskRequest.m_success = false;
            diskRequest.m_callback = [](bool) {};
            block.m_diskRequestIndex = requestIdx;
            block.m_codeBufferOffset = codeBufferOffset;
        }

        auto codeReadStart = std::chrono::steady_clock::now();
        bool codeReadSuccess = true;
        for (size_t requestIdx = 0; requestIdx < blockOrder.size(); ++requestIdx)
        {
            PostingBlockInfo &block = p_exWorkSpace->m_postingBlocks[blockOrder[requestIdx]];
            if (block.m_cachedCode != nullptr)
            {
                continue;
            }
            const int fileid = m_oneContext ? 0 : block.m_postingID / m_listPerFile;
            auto &diskRequest = p_exWorkSpace->m_chunkCodeDiskRequests[requestIdx];
            auto numRead =
                m_indexFiles[fileid]->ReadBinary(diskRequest.m_readSize, diskRequest.m_buffer, diskRequest.m_offset);
            if (numRead != diskRequest.m_readSize)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Chunk code read mismatch for posting %d chunk %d. expected=%llu actual=%llu "
                             "offset=%llu fileid=%d\n",
                             block.m_postingID, block.m_chunkID,
                             static_cast<unsigned long long>(diskRequest.m_readSize),
                             static_cast<unsigned long long>(numRead),
                             static_cast<unsigned long long>(diskRequest.m_offset), fileid);
                codeReadSuccess = false;
                break;
            }
        }
        double codeReadLatencyMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - codeReadStart).count();
        if (!codeReadSuccess)
        {
            return ErrorCode::DiskIOFail;
        }

        if (p_stats)
        {
            p_stats->m_batchReadTotalLatencyMs += codeReadLatencyMs;
            p_stats->m_diskReadLatency += codeReadLatencyMs;
            for (const auto &block : p_exWorkSpace->m_postingBlocks)
            {
                p_stats->m_codeBytesRead += static_cast<uint64_t>(block.m_codeBytes);
                if (block.m_cachedCode != nullptr)
                    continue;
                const uint64_t codeBufferOffset = static_cast<uint64_t>(block.m_codeBufferOffset);
                const uint64_t codeReadBytes =
                    ((codeBufferOffset + static_cast<uint64_t>(block.m_codeBytes) + PageSize - 1) >> PageSizeEx)
                    << PageSizeEx;
                const uint64_t codePages = codeReadBytes >> PageSizeEx;
                p_stats->m_requestedReadBytes += codeReadBytes;
                p_stats->m_codePhysicalBytesRead += codeReadBytes;
                p_stats->m_readPages += codePages;
                p_stats->m_diskAccessCount += static_cast<int>(codePages);
                p_stats->m_diskIOCount += 1;
            }
        }

        return ErrorCode::Success;
    }

    ErrorCode ReadPostingHeaderAndDirectory(ExtraWorkSpace *p_exWorkSpace,
                                            COMMON::QueryResultSet<ValueType> &queryResults, SearchStats *p_stats)
    {
        p_exWorkSpace->m_coarseCandidates.clear();
        p_exWorkSpace->m_mergedCandidates.clear();
        p_exWorkSpace->m_payloadReadRequests.clear();
        p_exWorkSpace->m_postingBlocks.clear();
        if (p_stats)
        {
            p_stats->m_metadataBytesRead = 0;
            p_stats->m_chunksConsidered = 0;
            p_stats->m_chunksPruned = 0;
            p_stats->m_chunksScanned = 0;
        }

        const bool useHardChunkPrune = CanUseHeuristicChunkPrune(queryResults);

        for (int postingID : p_exWorkSpace->m_postingIDs)
        {
            if (!CheckValidPosting(postingID))
                continue;

            ListInfo *listInfo = &(m_listInfos[postingID]);
            const PostingRuntimeMetadataCache *cachedMetadata =
                (postingID >= 0 && static_cast<size_t>(postingID) < m_postingRuntimeMetadata.size() &&
                 m_postingRuntimeMetadata[postingID].m_valid)
                    ? &m_postingRuntimeMetadata[postingID]
                    : nullptr;
            if (cachedMetadata != nullptr)
            {
                uint32_t totalChunkRecords = 0;
                uint32_t totalChunkCodeBytes = 0;
                uint32_t totalChunkPayloadBytes = 0;
                size_t chunkCountBeforePosting = p_exWorkSpace->m_postingBlocks.size();
                ErrorCode cacheRet = AppendPostingBlocksFromCache(
                    *cachedMetadata, postingID, *listInfo, queryResults, useHardChunkPrune, p_exWorkSpace, p_stats,
                    totalChunkRecords, totalChunkCodeBytes, totalChunkPayloadBytes);
                if (cacheRet != ErrorCode::Success)
                {
                    return cacheRet;
                }

                if (cachedMetadata->m_header.m_recordCount != totalChunkRecords ||
                    cachedMetadata->m_header.m_codeBytes != totalChunkCodeBytes ||
                    cachedMetadata->m_header.m_payloadBytes != totalChunkPayloadBytes)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                                 "Posting %d cached aggregated chunk metadata mismatch. records=%u/%u code=%u/%u "
                                 "payload=%u/%u\n",
                                 postingID, cachedMetadata->m_header.m_recordCount, totalChunkRecords,
                                 cachedMetadata->m_header.m_codeBytes, totalChunkCodeBytes,
                                 cachedMetadata->m_header.m_payloadBytes, totalChunkPayloadBytes);
                    return ErrorCode::Fail;
                }

                if (p_stats)
                {
                    p_stats->m_postingsTouched += 1;
                    p_stats->m_postingElementsRaw += static_cast<uint64_t>(cachedMetadata->m_header.m_recordCount);
                    p_stats->m_chunksConsidered += static_cast<uint64_t>(cachedMetadata->m_header.m_chunkCount);
                    p_stats->m_chunksScanned +=
                        static_cast<uint64_t>(p_exWorkSpace->m_postingBlocks.size() - chunkCountBeforePosting);
                }
                continue;
            }

            int fileid = m_oneContext ? 0 : postingID / m_listPerFile;
            Helper::DiskIO *indexFile = m_indexFiles[fileid].get();
            if (indexFile == nullptr)
            {
                return ErrorCode::DiskIOFail;
            }

            if (listInfo->listEleCount <= 4)
            {
                static std::atomic<int> s_smallPostingWarningBudget(0);
                int warningIndex = s_smallPostingWarningBudget.fetch_add(1);
                if (warningIndex < 8)
                {
                    SPTAGLIB_LOG(
                        Helper::LogLevel::LL_Warning,
                        "Two-stage posting %d has tiny list. eleCount=%d pageCount=%u totalBytes=%zu pageOffset=%u\n",
                        postingID, listInfo->listEleCount, listInfo->listPageCount, listInfo->listTotalBytes,
                        listInfo->pageOffset);
                }
            }

            const size_t totalBytes = static_cast<size_t>(listInfo->listPageCount) << PageSizeEx;
            if (totalBytes <= listInfo->pageOffset)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d has invalid total bytes/page offset. total=%zu pageOffset=%u\n", postingID,
                             totalBytes, listInfo->pageOffset);
                return ErrorCode::Fail;
            }

            const size_t firstReadBytes = GetInitialPostingMetadataReadBytes(listInfo->pageOffset, totalBytes);
            p_exWorkSpace->EnsurePostingMetadataBufferCapacity(firstReadBytes);
            size_t metadataReadBytes = firstReadBytes;
            char *postingMetadataBuffer = reinterpret_cast<char *>(p_exWorkSpace->m_postingMetadataBuffer.GetBuffer());
            auto numRead = indexFile->ReadBinary(firstReadBytes, postingMetadataBuffer, listInfo->listOffset);
            if (numRead != firstReadBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, actual: %llu.\n",
                             m_extraFullGraphFile.c_str(), firstReadBytes, numRead);
                return ErrorCode::DiskIOFail;
            }

            const char *postingBase = postingMetadataBuffer + listInfo->pageOffset;
            const size_t availableBytes = firstReadBytes - listInfo->pageOffset;
            if (availableBytes < sizeof(NewPostingHeader) + sizeof(NewPostingChunkDirectoryEntry))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Posting %d too small for new two-stage header.\n", postingID);
                return ErrorCode::Fail;
            }

            NewPostingHeader header;
            std::memcpy(&header, postingBase, sizeof(NewPostingHeader));
            if (header.m_magic != 0x53504732 || header.m_version != 2 || header.m_chunkCount == 0)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d has invalid new-format header. magic=%u version=%u chunks=%u\n", postingID,
                             header.m_magic, header.m_version, header.m_chunkCount);
                return ErrorCode::Fail;
            }

            const size_t directoryBytes =
                IsChunkedTwoStagePostingFormat()
                    ? static_cast<size_t>(header.m_chunkCount) * sizeof(NewPostingChunkDirectoryEntryV2)
                    : static_cast<size_t>(header.m_chunkCount) * sizeof(NewPostingChunkDirectoryEntry);
            const size_t centroidBytes = IsChunkedTwoStagePostingFormat()
                                             ? static_cast<size_t>(header.m_chunkCount) *
                                                   static_cast<size_t>(m_iDataDimension) * sizeof(ValueType)
                                             : 0;
            const size_t chunkCodeBaseOffset = sizeof(NewPostingHeader) + directoryBytes + centroidBytes;
            const size_t chunkPayloadBaseOffset = chunkCodeBaseOffset + header.m_codeBytes;
            const size_t metadataBytes =
                static_cast<size_t>(listInfo->pageOffset) + sizeof(NewPostingHeader) + directoryBytes + centroidBytes;
            if (metadataBytes > totalBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d metadata bytes exceed posting bytes. metadata=%zu total=%zu\n", postingID,
                             metadataBytes, totalBytes);
                return ErrorCode::Fail;
            }

            if (metadataBytes > firstReadBytes)
            {
                const size_t alignedMetadataReadBytes =
                    std::min(totalBytes, ((metadataBytes + PageSize - 1) >> PageSizeEx) << PageSizeEx);
                if (alignedMetadataReadBytes < metadataBytes)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                                 "Posting %d metadata alignment underflow. metadata=%zu aligned=%zu total=%zu\n",
                                 postingID, metadataBytes, alignedMetadataReadBytes, totalBytes);
                    return ErrorCode::Fail;
                }

                p_exWorkSpace->EnsurePostingMetadataBufferCapacity(alignedMetadataReadBytes);
                postingMetadataBuffer = reinterpret_cast<char *>(p_exWorkSpace->m_postingMetadataBuffer.GetBuffer());
                numRead = indexFile->ReadBinary(alignedMetadataReadBytes, postingMetadataBuffer, listInfo->listOffset);
                if (numRead != alignedMetadataReadBytes)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, actual: %llu.\n",
                                 m_extraFullGraphFile.c_str(), alignedMetadataReadBytes, numRead);
                    return ErrorCode::DiskIOFail;
                }
                metadataReadBytes = alignedMetadataReadBytes;
                postingBase = postingMetadataBuffer + listInfo->pageOffset;
            }

            uint32_t totalChunkRecords = 0;
            uint32_t totalChunkCodeBytes = 0;
            uint32_t totalChunkPayloadBytes = 0;
            size_t chunkCountBeforePosting = p_exWorkSpace->m_postingBlocks.size();

            if (!IsChunkedTwoStagePostingFormat())
            {
                if (header.m_chunkCount != 1)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                                 "Posting %d uses %u chunks with legacy two-stage layout.\n", postingID,
                                 header.m_chunkCount);
                    return ErrorCode::Fail;
                }

                NewPostingChunkDirectoryEntry entry;
                std::memcpy(&entry, postingBase + sizeof(NewPostingHeader), sizeof(NewPostingChunkDirectoryEntry));

                if (header.m_recordCount != entry.m_recordCount ||
                    header.m_payloadRecordBytes != entry.m_payloadRecordBytes ||
                    header.m_codeBytes != entry.m_codeBytes || header.m_payloadBytes != entry.m_payloadBytes ||
                    entry.m_codeOffset + entry.m_codeBytes != entry.m_payloadOffset ||
                    entry.m_payloadOffset > listInfo->listTotalBytes ||
                    entry.m_payloadOffset + entry.m_payloadBytes > listInfo->listTotalBytes)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Posting %d has invalid single-chunk metadata.\n",
                                 postingID);
                    return ErrorCode::Fail;
                }

                PostingBlockInfo block;
                block.m_postingID = postingID;
                block.m_chunkID = 0;
                block.m_codeOffset = entry.m_codeOffset;
                block.m_codeBytes = entry.m_codeBytes;
                block.m_codeRecordBytes = header.m_codeRecordBytes;
                block.m_payloadOffset = entry.m_payloadOffset;
                block.m_payloadBytes = entry.m_payloadBytes;
                block.m_payloadRecordBytes = entry.m_payloadRecordBytes;
                block.m_recordCount = entry.m_recordCount;
                p_exWorkSpace->m_postingBlocks.emplace_back(block);

                totalChunkRecords = entry.m_recordCount;
                totalChunkCodeBytes = entry.m_codeBytes;
                totalChunkPayloadBytes = entry.m_payloadBytes;
            }
            else
            {
                const char *directoryBase = postingBase + sizeof(NewPostingHeader);
                for (uint32_t chunkID = 0; chunkID < header.m_chunkCount; ++chunkID)
                {
                    NewPostingChunkDirectoryEntryV2 entry;
                    std::memcpy(&entry,
                                directoryBase + static_cast<size_t>(chunkID) * sizeof(NewPostingChunkDirectoryEntryV2),
                                sizeof(NewPostingChunkDirectoryEntryV2));

                    if (entry.m_centroidBytes != static_cast<uint32_t>(m_iDataDimension * sizeof(ValueType)) ||
                        entry.m_recordCount == 0 || entry.m_codeOffset + entry.m_codeBytes > listInfo->listTotalBytes ||
                        entry.m_payloadOffset + entry.m_payloadBytes > listInfo->listTotalBytes ||
                        entry.m_codeOffset < chunkCodeBaseOffset ||
                        entry.m_codeOffset + entry.m_codeBytes > chunkPayloadBaseOffset ||
                        entry.m_payloadOffset < chunkPayloadBaseOffset ||
                        entry.m_centroidOffset + entry.m_centroidBytes > listInfo->listTotalBytes)
                    {
                        SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                                     "Posting %d chunk %u has invalid chunked metadata. "
                                     "centroidOffset=%u centroidBytes=%u codeOffset=%u codeBytes=%u payloadOffset=%u "
                                     "payloadBytes=%u codeBase=%zu payloadBase=%zu totalBytes=%zu codeBytesTotal=%u "
                                     "payloadBytesTotal=%u recordCount=%u pageOffset=%u\n",
                                     postingID, chunkID, entry.m_centroidOffset, entry.m_centroidBytes,
                                     entry.m_codeOffset, entry.m_codeBytes, entry.m_payloadOffset, entry.m_payloadBytes,
                                     chunkCodeBaseOffset, chunkPayloadBaseOffset, listInfo->listTotalBytes,
                                     header.m_codeBytes, header.m_payloadBytes, entry.m_recordCount,
                                     listInfo->pageOffset);
                        return ErrorCode::Fail;
                    }

                    PostingBlockInfo block;
                    block.m_postingID = postingID;
                    block.m_chunkID = static_cast<int>(chunkID);
                    block.m_centroidOffset = entry.m_centroidOffset;
                    block.m_centroidBytes = entry.m_centroidBytes;
                    block.m_codeOffset = entry.m_codeOffset;
                    block.m_codeBytes = entry.m_codeBytes;
                    block.m_codeRecordBytes = header.m_codeRecordBytes;
                    block.m_payloadOffset = entry.m_payloadOffset;
                    block.m_payloadBytes = entry.m_payloadBytes;
                    block.m_payloadRecordBytes = entry.m_payloadRecordBytes;
                    block.m_recordCount = entry.m_recordCount;
                    block.m_radius = entry.m_radius;

                    totalChunkRecords += entry.m_recordCount;
                    totalChunkCodeBytes += entry.m_codeBytes;
                    totalChunkPayloadBytes += entry.m_payloadBytes;

                    if (useHardChunkPrune)
                    {
                        const ValueType *centroid =
                            reinterpret_cast<const ValueType *>(postingBase + entry.m_centroidOffset);
                        block.m_lowerBound = ComputeChunkL2LowerBound(queryResults, centroid, entry.m_radius);
                        if (block.m_lowerBound >= queryResults.worstDist())
                        {
                            if (p_stats)
                            {
                                p_stats->m_chunksPruned += 1;
                            }
                            continue;
                        }
                    }

                    p_exWorkSpace->m_postingBlocks.emplace_back(block);
                }
            }

            if (header.m_recordCount != totalChunkRecords || header.m_codeBytes != totalChunkCodeBytes ||
                header.m_payloadBytes != totalChunkPayloadBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d aggregated chunk metadata mismatch. records=%u/%u code=%u/%u payload=%u/%u\n",
                             postingID, header.m_recordCount, totalChunkRecords, header.m_codeBytes,
                             totalChunkCodeBytes, header.m_payloadBytes, totalChunkPayloadBytes);
                return ErrorCode::Fail;
            }

            if (p_stats)
            {
                const uint64_t metadataReadPages =
                    static_cast<uint64_t>((metadataReadBytes + PageSize - 1) >> PageSizeEx);
                p_stats->m_requestedReadBytes += static_cast<uint64_t>(metadataReadBytes);
                p_stats->m_metadataBytesRead += static_cast<uint64_t>(metadataReadBytes);
                p_stats->m_readPages += metadataReadPages;
                p_stats->m_diskAccessCount += static_cast<int>(metadataReadPages);
                p_stats->m_diskIOCount += (metadataReadBytes > firstReadBytes) ? 2 : 1;
                p_stats->m_postingsTouched += 1;
                p_stats->m_postingElementsRaw += static_cast<uint64_t>(header.m_recordCount);
                p_stats->m_chunksConsidered += static_cast<uint64_t>(header.m_chunkCount);
                p_stats->m_chunksScanned +=
                    static_cast<uint64_t>(p_exWorkSpace->m_postingBlocks.size() - chunkCountBeforePosting);
            }
        }
        if (p_exWorkSpace->m_postingBlocks.empty() && !p_exWorkSpace->m_postingIDs.empty())
        {
            static std::atomic<int> s_emptyBlockWarningBudget(0);
            int warningIndex = s_emptyBlockWarningBudget.fetch_add(1);
            if (warningIndex < 5)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Warning,
                             "Two-stage search saw postingIDs but parsed no posting blocks. postingIDs=%zu\n",
                             p_exWorkSpace->m_postingIDs.size());
            }
        }
        return ErrorCode::Success;
    }

    ErrorCode ScanCompactCodes(ExtraWorkSpace *p_exWorkSpace, COMMON::QueryResultSet<ValueType> &queryResults,
                               std::shared_ptr<VectorIndex> p_index, SearchStats *p_stats,
                               const std::set<int> *truth = nullptr, std::map<int, std::set<int>> *found = nullptr)
    {
        auto quantizer = GetPostingQuantizer(p_index);
        if (quantizer == nullptr)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Two-stage posting search requires a quantizer for coarse code scanning.\n");
            return ErrorCode::Undefined;
        }

        // For two-stage static posting, head index uses float vectors.
        // The query target is the original float vector, not quantized.
        // We need to quantize it here for posting search.
        const uint8_t *quantizedTarget = nullptr;

        if (queryResults.HasQuantizedTarget())
        {
            // Already quantized (e.g., for dynamic posting with uint8 head index)
            quantizedTarget = reinterpret_cast<const uint8_t *>(queryResults.GetQuantizedTarget());
        }
        else
        {
            // Quantize the original float vector for posting search
            const ValueType *originalTarget = queryResults.GetTarget();
            if (originalTarget == nullptr)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Query target is null for two-stage posting search.\n");
                return ErrorCode::Undefined;
            }
            p_exWorkSpace->EnsurePostingQuantizedTargetCapacity(quantizer->QuantizeSize());
            quantizer->QuantizeVector(const_cast<ValueType *>(originalTarget),
                                      p_exWorkSpace->m_postingQuantizedTarget.data());
            quantizedTarget = p_exWorkSpace->m_postingQuantizedTarget.data();
        }

        const uint32_t codeBytesPerVector =
            static_cast<uint32_t>(sizeof(NewPostingCodeRecordPrefix) + quantizer->QuantizeSize());
        const int topR = std::max(1, m_opt->m_postingTopRPerPosting);
        auto coarseDistance = quantizer->template DistanceCalcSelector<std::uint8_t>(m_opt->m_distCalcMethod);

        ErrorCode ret = ReadChunkCodeBlocks(p_exWorkSpace, p_stats);
        if (ret != ErrorCode::Success)
            return ret;

        if (truth != nullptr)
        {
            p_exWorkSpace->m_truthBestScannedRank.clear();
            p_exWorkSpace->m_truthScannedPostingCount.clear();
        }

        for (size_t blockIdx = 0; blockIdx < p_exWorkSpace->m_postingBlocks.size(); ++blockIdx)
        {
            const PostingBlockInfo &block = p_exWorkSpace->m_postingBlocks[blockIdx];
            const ListInfo &listInfo = m_listInfos[block.m_postingID];

            if (block.m_codeRecordBytes != codeBytesPerVector)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d code record bytes mismatch. expected=%u actual=%u\n", block.m_postingID,
                             codeBytesPerVector, block.m_codeRecordBytes);
                return ErrorCode::Fail;
            }
            if (block.m_recordCount == 0 || block.m_codeBytes == 0)
            {
                continue;
            }

            const char *codeBase = nullptr;
            if (block.m_cachedCode != nullptr)
            {
                codeBase = block.m_cachedCode;
            }
            else
            {
                const auto &request = p_exWorkSpace->m_chunkCodeDiskRequests[block.m_diskRequestIndex];
                codeBase = request.m_buffer + block.m_codeBufferOffset;
            }
            std::vector<CoarseCandidate> localCandidates;
            localCandidates.reserve(block.m_recordCount);

            for (uint32_t i = 0; i < block.m_recordCount; ++i)
            {
                const char *recordPtr = codeBase + static_cast<size_t>(i) * block.m_codeRecordBytes;
                NewPostingCodeRecordPrefix prefix;
                std::memcpy(&prefix, recordPtr, sizeof(NewPostingCodeRecordPrefix));

                if (prefix.m_vectorID < 0)
                {
                    continue;
                }
                if (block.m_payloadRecordBytes == 0 || prefix.m_payloadOffset % block.m_payloadRecordBytes != 0 ||
                    prefix.m_payloadOffset + block.m_payloadRecordBytes > block.m_payloadBytes)
                {
                    const uint64_t postingPayloadStart =
                        static_cast<uint64_t>(listInfo.pageOffset) + static_cast<uint64_t>(block.m_payloadOffset);
                    SPTAGLIB_LOG(
                        Helper::LogLevel::LL_Error,
                        "Posting %d chunk %d has invalid code prefix at record %u. vectorID=%d payloadOffset=%u "
                        "payloadRecordBytes=%u blockPayloadBytes=%u blockPayloadOffset=%u "
                        "postingPayloadStart=%llu listPageOffset=%u listTotalBytes=%zu codeOffset=%u codeBytes=%u "
                        "recordCount=%u\n",
                        block.m_postingID, block.m_chunkID, i, prefix.m_vectorID, prefix.m_payloadOffset,
                        block.m_payloadRecordBytes, block.m_payloadBytes, block.m_payloadOffset,
                        static_cast<unsigned long long>(postingPayloadStart), listInfo.pageOffset,
                        listInfo.listTotalBytes, block.m_codeOffset, block.m_codeBytes, block.m_recordCount);
                    return ErrorCode::Fail;
                }
                if (p_exWorkSpace->m_filterFunc != nullptr &&
                    !p_exWorkSpace->m_filterFunc(p_index->GetMetadata(prefix.m_vectorID)))
                {
                    continue;
                }

                const uint8_t *codePtr =
                    reinterpret_cast<const uint8_t *>(recordPtr + sizeof(NewPostingCodeRecordPrefix));
                float coarseDist = coarseDistance(quantizedTarget, codePtr, quantizer->QuantizeSize());

                CoarseCandidate candidate;
                candidate.m_vectorID = prefix.m_vectorID;
                candidate.m_coarseDist = coarseDist;
                candidate.m_postingID = block.m_postingID;
                candidate.m_chunkID = block.m_chunkID;
                candidate.m_blockIndex = blockIdx;
                candidate.m_payloadOffset = prefix.m_payloadOffset;
                candidate.m_payloadBytes = block.m_payloadRecordBytes;
                candidate.m_payloadPhysicalOffset = static_cast<uint64_t>(listInfo.pageOffset) +
                                                    static_cast<uint64_t>(block.m_payloadOffset) +
                                                    static_cast<uint64_t>(prefix.m_payloadOffset);
                candidate.m_payloadPageID = static_cast<uint32_t>(candidate.m_payloadPhysicalOffset >> PageSizeEx);
                localCandidates.emplace_back(candidate);
            }

            std::sort(localCandidates.begin(), localCandidates.end(),
                      [](const CoarseCandidate &lhs, const CoarseCandidate &rhs) {
                          if (lhs.m_coarseDist != rhs.m_coarseDist)
                              return lhs.m_coarseDist < rhs.m_coarseDist;
                          return lhs.m_vectorID < rhs.m_vectorID;
                      });

            if (truth != nullptr)
            {
                for (uint32_t rank = 0; rank < static_cast<uint32_t>(localCandidates.size()); ++rank)
                {
                    const auto &candidate = localCandidates[rank];
                    if (candidate.m_vectorID < 0 || truth->count(static_cast<int>(candidate.m_vectorID)) == 0)
                    {
                        continue;
                    }

                    auto bestRankIter = p_exWorkSpace->m_truthBestScannedRank.find(candidate.m_vectorID);
                    const uint32_t oneBasedRank = rank + 1;
                    if (bestRankIter == p_exWorkSpace->m_truthBestScannedRank.end() ||
                        oneBasedRank < bestRankIter->second)
                    {
                        p_exWorkSpace->m_truthBestScannedRank[candidate.m_vectorID] = oneBasedRank;
                    }
                    p_exWorkSpace->m_truthScannedPostingCount[candidate.m_vectorID] += 1;
                    if (found != nullptr)
                    {
                        (*found)[candidate.m_postingID].insert(static_cast<int>(candidate.m_vectorID));
                    }
                }
            }

            if (localCandidates.size() > static_cast<size_t>(topR))
            {
                localCandidates.resize(topR);
            }

            if (p_stats)
            {
                p_stats->m_coarseCandidateCount += static_cast<uint64_t>(localCandidates.size());
            }

            p_exWorkSpace->m_coarseCandidates.insert(p_exWorkSpace->m_coarseCandidates.end(), localCandidates.begin(),
                                                     localCandidates.end());
        }
        return ErrorCode::Success;
    }

    ErrorCode MergeCoarseCandidates(ExtraWorkSpace *p_exWorkSpace, SearchStats *p_stats)
    {
        const size_t globalTopR = static_cast<size_t>(std::max(1, m_opt->m_postingTopRGlobal));
        p_exWorkSpace->m_mergedCandidates.clear();
        auto &bestByVector = p_exWorkSpace->m_bestCoarseCandidateByVector;
        bestByVector.clear();
        bestByVector.reserve(p_exWorkSpace->m_coarseCandidates.size());

        for (const auto &candidate : p_exWorkSpace->m_coarseCandidates)
        {
            if (candidate.m_vectorID < 0)
                continue;

            auto iter = bestByVector.find(candidate.m_vectorID);
            if (iter == bestByVector.end() || candidate.m_coarseDist < iter->second.m_coarseDist ||
                (candidate.m_coarseDist == iter->second.m_coarseDist &&
                 candidate.m_postingID < iter->second.m_postingID))
            {
                bestByVector[candidate.m_vectorID] = candidate;
            }
        }

        p_exWorkSpace->m_mergedCandidates.reserve(bestByVector.size());
        for (const auto &pair : bestByVector)
        {
            p_exWorkSpace->m_mergedCandidates.emplace_back(pair.second);
        }

        std::sort(p_exWorkSpace->m_mergedCandidates.begin(), p_exWorkSpace->m_mergedCandidates.end(),
                  [](const CoarseCandidate &lhs, const CoarseCandidate &rhs) {
                      if (lhs.m_coarseDist != rhs.m_coarseDist)
                          return lhs.m_coarseDist < rhs.m_coarseDist;
                      return lhs.m_vectorID < rhs.m_vectorID;
                  });
        if (p_stats)
        {
            p_stats->m_coarseCandidateCountAfterDedupe =
                static_cast<uint64_t>(p_exWorkSpace->m_mergedCandidates.size());
        }
        if (p_exWorkSpace->m_mergedCandidates.size() > globalTopR)
        {
            p_exWorkSpace->m_mergedCandidates.resize(globalTopR);
        }
        if (p_stats)
        {
            p_stats->m_rerankCandidateCount = static_cast<uint64_t>(p_exWorkSpace->m_mergedCandidates.size());
            uint64_t coarseCandidateHash = 1469598103934665603ULL;
            for (const auto &candidate : p_exWorkSpace->m_mergedCandidates)
            {
                coarseCandidateHash = HashMix(coarseCandidateHash, static_cast<uint64_t>(candidate.m_vectorID));
                coarseCandidateHash = HashMix(coarseCandidateHash, static_cast<uint64_t>(candidate.m_postingID));
                coarseCandidateHash = HashMix(coarseCandidateHash, static_cast<uint64_t>(candidate.m_chunkID));
                coarseCandidateHash = HashMix(coarseCandidateHash, HashFloatBits(candidate.m_coarseDist));
                coarseCandidateHash = HashMix(coarseCandidateHash, static_cast<uint64_t>(candidate.m_payloadOffset));
            }
            p_stats->m_coarseCandidateHash = coarseCandidateHash;
        }
        return ErrorCode::Success;
    }

    ErrorCode BuildPayloadReadPlan(ExtraWorkSpace *p_exWorkSpace, SearchStats *p_stats)
    {
        p_exWorkSpace->m_payloadReadRequests.clear();
        auto &requestByPage = p_exWorkSpace->m_payloadPageRequestIndex;
        requestByPage.clear();
        requestByPage.reserve(p_exWorkSpace->m_mergedCandidates.size() * 2);

        size_t maxPayloadBytes = 0;
        for (auto &candidate : p_exWorkSpace->m_mergedCandidates)
        {
            maxPayloadBytes = std::max(maxPayloadBytes, static_cast<size_t>(candidate.m_payloadBytes));
            candidate.m_payloadRequestStart = static_cast<size_t>(-1);
            candidate.m_payloadPageCount = 0;
            if (candidate.m_blockIndex >= p_exWorkSpace->m_postingBlocks.size())
            {
                continue;
            }

            const ListInfo &listInfo = m_listInfos[candidate.m_postingID];
            const uint64_t payloadPhysicalOffset = candidate.m_payloadPhysicalOffset;
            const uint64_t payloadPhysicalEnd = payloadPhysicalOffset + static_cast<uint64_t>(candidate.m_payloadBytes);
            const uint64_t postingPhysicalBytes =
                static_cast<uint64_t>(listInfo.pageOffset) + static_cast<uint64_t>(listInfo.listTotalBytes);
            if (payloadPhysicalEnd > postingPhysicalBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d payload range exceeds posting bytes. payloadOffset=%llu payloadBytes=%u "
                             "postingBytes=%llu\n",
                             candidate.m_postingID, static_cast<unsigned long long>(payloadPhysicalOffset),
                             candidate.m_payloadBytes, static_cast<unsigned long long>(postingPhysicalBytes));
                return ErrorCode::Fail;
            }

            const uint32_t startPageID = candidate.m_payloadPageID;
            const uint32_t endPageID = static_cast<uint32_t>((payloadPhysicalEnd - 1) >> PageSizeEx);
            candidate.m_payloadPageCount = endPageID - startPageID + 1;

            for (uint32_t pageID = startPageID; pageID <= endPageID; ++pageID)
            {
                const uint64_t pageKey = GetPostingPageKey(candidate.m_postingID, pageID);
                if (requestByPage.find(pageKey) != requestByPage.end())
                {
                    continue;
                }

                PayloadReadRequest request;
                request.m_postingID = candidate.m_postingID;
                request.m_chunkID = candidate.m_chunkID;
                request.m_pageID = pageID;
                request.m_pageKey = pageKey;
                request.m_pageOffset = listInfo.listOffset + (static_cast<uint64_t>(pageID) << PageSizeEx);
                request.m_pageBytes = static_cast<uint32_t>(
                    std::min<uint64_t>(PageSize, postingPhysicalBytes - (static_cast<uint64_t>(pageID) << PageSizeEx)));
                p_exWorkSpace->m_payloadReadRequests.emplace_back(request);
                requestByPage.emplace(pageKey, p_exWorkSpace->m_payloadReadRequests.size() - 1);
            }
        }

        p_exWorkSpace->EnsurePayloadScratchCapacity(maxPayloadBytes);

        if (p_exWorkSpace->m_payloadReadRequests.empty())
        {
            if (p_stats)
            {
                p_stats->m_payloadLogicalBytesRead = 0;
                p_stats->m_payloadBytesRead = 0;
                p_stats->m_payloadPageHash = 0;
            }
            return ErrorCode::Success;
        }

        std::vector<size_t> originalToSorted;
        originalToSorted.resize(p_exWorkSpace->m_payloadReadRequests.size());
        for (size_t i = 0; i < originalToSorted.size(); ++i)
        {
            originalToSorted[i] = i;
        }
        std::sort(originalToSorted.begin(), originalToSorted.end(),
                  [this, &requests = p_exWorkSpace->m_payloadReadRequests](size_t lhs, size_t rhs) {
                      const auto &lhsReq = requests[lhs];
                      const auto &rhsReq = requests[rhs];
                      const int lhsFileId = m_oneContext ? 0 : lhsReq.m_postingID / m_listPerFile;
                      const int rhsFileId = m_oneContext ? 0 : rhsReq.m_postingID / m_listPerFile;
                      if (lhsFileId != rhsFileId)
                          return lhsFileId < rhsFileId;
                      if (lhsReq.m_pageOffset != rhsReq.m_pageOffset)
                          return lhsReq.m_pageOffset < rhsReq.m_pageOffset;
                      return lhsReq.m_postingID < rhsReq.m_postingID;
                  });

        std::vector<PayloadReadRequest> sortedRequests;
        sortedRequests.reserve(p_exWorkSpace->m_payloadReadRequests.size());
        std::vector<size_t> originalToSortedIndex(originalToSorted.size());
        for (size_t sortedIdx = 0; sortedIdx < originalToSorted.size(); ++sortedIdx)
        {
            size_t origIdx = originalToSorted[sortedIdx];
            originalToSortedIndex[origIdx] = sortedIdx;
            sortedRequests.emplace_back(p_exWorkSpace->m_payloadReadRequests[origIdx]);
        }
        p_exWorkSpace->m_payloadReadRequests = std::move(sortedRequests);

        requestByPage.clear();
        requestByPage.reserve(p_exWorkSpace->m_payloadReadRequests.size());
        for (size_t requestIdx = 0; requestIdx < p_exWorkSpace->m_payloadReadRequests.size(); ++requestIdx)
        {
            requestByPage.emplace(p_exWorkSpace->m_payloadReadRequests[requestIdx].m_pageKey, requestIdx);
        }

        for (auto &candidate : p_exWorkSpace->m_mergedCandidates)
        {
            if (candidate.m_blockIndex >= p_exWorkSpace->m_postingBlocks.size())
            {
                continue;
            }
            if (candidate.m_payloadPageCount == 0)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Candidate payload page span is empty for posting %d vector %d.\n", candidate.m_postingID,
                             candidate.m_vectorID);
                return ErrorCode::Fail;
            }

            const uint64_t startPageKey = GetPostingPageKey(candidate.m_postingID, candidate.m_payloadPageID);
            auto requestIter = requestByPage.find(startPageKey);
            if (requestIter == requestByPage.end())
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Missing payload request span start for posting %d page %u.\n",
                             candidate.m_postingID, candidate.m_payloadPageID);
                return ErrorCode::Fail;
            }
            candidate.m_payloadRequestStart = requestIter->second;
        }

        if (p_stats)
        {
            p_stats->m_payloadLogicalBytesRead = 0;
            p_stats->m_payloadBytesRead = 0;
            p_stats->m_uniquePayloadPages = p_exWorkSpace->m_payloadReadRequests.size();
            p_stats->m_payloadCandidates = p_exWorkSpace->m_mergedCandidates.size();

            std::unordered_set<int> postingsWithPayload;
            postingsWithPayload.reserve(p_exWorkSpace->m_mergedCandidates.size());
            uint64_t totalPageSpans = 0;
            for (const auto &candidate : p_exWorkSpace->m_mergedCandidates)
            {
                if (candidate.m_blockIndex < p_exWorkSpace->m_postingBlocks.size() && candidate.m_payloadPageCount > 0)
                {
                    postingsWithPayload.insert(candidate.m_postingID);
                    totalPageSpans += candidate.m_payloadPageCount;
                }
            }
            p_stats->m_postingsWithPayload = postingsWithPayload.size();
            p_stats->m_totalPayloadPageSpans = totalPageSpans;

            uint64_t payloadPageHash = 1469598103934665603ULL;
            for (const auto &request : p_exWorkSpace->m_payloadReadRequests)
            {
                payloadPageHash = HashMix(payloadPageHash, static_cast<uint64_t>(request.m_postingID));
                payloadPageHash = HashMix(payloadPageHash, static_cast<uint64_t>(request.m_pageID));
                payloadPageHash = HashMix(payloadPageHash, static_cast<uint64_t>(request.m_pageOffset));
                payloadPageHash = HashMix(payloadPageHash, static_cast<uint64_t>(request.m_pageBytes));
            }
            p_stats->m_payloadPageHash = payloadPageHash;

            if (m_opt != nullptr && m_opt->m_enablePayloadTrace)
            {
                p_stats->m_payloadTraceRecords.clear();
                p_stats->m_payloadTraceRecords.reserve(p_exWorkSpace->m_mergedCandidates.size());
                for (const auto &candidate : p_exWorkSpace->m_mergedCandidates)
                {
                    if (candidate.m_blockIndex >= p_exWorkSpace->m_postingBlocks.size() ||
                        candidate.m_payloadPageCount == 0)
                    {
                        continue;
                    }

                    PayloadTraceRecord record;
                    record.m_postingID = candidate.m_postingID;
                    record.m_chunkID = candidate.m_chunkID;
                    record.m_vectorID = candidate.m_vectorID;
                    record.m_payloadPageID = candidate.m_payloadPageID;
                    record.m_payloadPageCount = candidate.m_payloadPageCount;
                    record.m_payloadPhysicalOffset = candidate.m_payloadPhysicalOffset;
                    record.m_payloadBytes = candidate.m_payloadBytes;
                    record.m_coarseDist = candidate.m_coarseDist;
                    p_stats->m_payloadTraceRecords.emplace_back(record);
                }
            }
        }
        return ErrorCode::Success;
    }

    ErrorCode FetchPayloadPagesAndRerank(ExtraWorkSpace *p_exWorkSpace, COMMON::QueryResultSet<ValueType> &queryResults,
                                         QueryResult &p_queryResults, std::shared_ptr<VectorIndex> p_index,
                                         SearchStats *p_stats)
    {
        // P2: Sub-phase timing
        auto subPhaseStart = std::chrono::steady_clock::now();
        auto subPhaseEnd = subPhaseStart;
        double payloadReadWaitMs = 0.0;
        double payloadCopyMs = 0.0;
        double exactDistanceMs = 0.0;
        double resultInsertionMs = 0.0;

        if (p_exWorkSpace->m_payloadReadRequests.empty())
        {
            p_queryResults.SetScanned(0);
            if (p_stats)
            {
                p_stats->m_finalResultCount = static_cast<uint64_t>(p_queryResults.GetResultNum());
            }
            return ErrorCode::Success;
        }

        p_exWorkSpace->EnsurePayloadRequestCapacity(p_exWorkSpace->m_payloadReadRequests.size());
        for (size_t requestIdx = 0; requestIdx < p_exWorkSpace->m_payloadReadRequests.size(); ++requestIdx)
        {
            const auto &payloadRequest = p_exWorkSpace->m_payloadReadRequests[requestIdx];
            auto &diskRequest = p_exWorkSpace->m_payloadDiskRequests[requestIdx];
            const int fileid = m_oneContext ? 0 : payloadRequest.m_postingID / m_listPerFile;
            diskRequest.m_offset = payloadRequest.m_pageOffset;
            diskRequest.m_readSize = payloadRequest.m_pageBytes;
            diskRequest.m_status = (fileid << 16) | (diskRequest.m_status & 0xffff);
            diskRequest.m_payload = const_cast<PayloadReadRequest *>(&payloadRequest);
            diskRequest.m_success = false;
        }

        const size_t payloadBatchPages =
            static_cast<size_t>(std::max(1, m_opt != nullptr ? m_opt->m_postingPayloadBatchPages : 1));
        double payloadReadLatencyMs = 0.0;

        // P2: Time payload read wait
        subPhaseStart = std::chrono::steady_clock::now();
        for (size_t batchStart = 0; batchStart < p_exWorkSpace->m_payloadReadRequests.size();
             batchStart += payloadBatchPages)
        {
            const size_t batchCount =
                std::min(payloadBatchPages, p_exWorkSpace->m_payloadReadRequests.size() - batchStart);
            auto payloadReadStart = std::chrono::steady_clock::now();
            bool payloadReadSuccess = BatchReadFileAsync(
                m_indexFiles, p_exWorkSpace->m_payloadDiskRequests.data() + batchStart, static_cast<int>(batchCount));
            payloadReadLatencyMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - payloadReadStart).count();
            if (!payloadReadSuccess)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Batch payload page read failed.\n");
                return ErrorCode::DiskIOFail;
            }
        }
        subPhaseEnd = std::chrono::steady_clock::now();
        payloadReadWaitMs = std::chrono::duration<double, std::milli>(subPhaseEnd - subPhaseStart).count();

        for (size_t requestIdx = 0; requestIdx < p_exWorkSpace->m_payloadReadRequests.size(); ++requestIdx)
        {
            const auto &payloadRequest = p_exWorkSpace->m_payloadReadRequests[requestIdx];
            if (p_stats)
            {
                p_stats->m_requestedReadBytes += payloadRequest.m_pageBytes;
                p_stats->m_payloadBytesRead += payloadRequest.m_pageBytes;
                p_stats->m_readPages += 1;
                p_stats->m_diskAccessCount += 1;
                p_stats->m_diskIOCount += 1;
            }
        }

        // P2: Time payload copy and exact distance phases
        subPhaseStart = std::chrono::steady_clock::now();
        for (const auto &candidate : p_exWorkSpace->m_mergedCandidates)
        {
            if (p_stats)
            {
                p_stats->m_payloadLogicalBytesRead += static_cast<uint64_t>(candidate.m_payloadBytes);
            }

            if (candidate.m_blockIndex >= p_exWorkSpace->m_postingBlocks.size())
            {
                continue;
            }

            uint64_t payloadPhysicalOffset = candidate.m_payloadPhysicalOffset;
            uint32_t remainingBytes = candidate.m_payloadBytes;
            size_t scratchOffset = 0;
            uint32_t pageID = candidate.m_payloadPageID;
            const bool singlePagePayload = (candidate.m_payloadPageCount == 1);
            const ValueType *exactVector = nullptr;
            if (candidate.m_payloadRequestStart == static_cast<size_t>(-1) || candidate.m_payloadPageCount == 0)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Missing payload request span for posting %d vector %d.\n",
                             candidate.m_postingID, candidate.m_vectorID);
                return ErrorCode::Fail;
            }

            size_t payloadRequestIndex = candidate.m_payloadRequestStart;
            const size_t payloadRequestEnd =
                candidate.m_payloadRequestStart + static_cast<size_t>(candidate.m_payloadPageCount);

            // P2: Time multi-page payload copy
            auto copyStart = std::chrono::steady_clock::now();
            while (remainingBytes > 0)
            {
                if (payloadRequestIndex >= payloadRequestEnd ||
                    payloadRequestIndex >= p_exWorkSpace->m_payloadReadRequests.size())
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Payload request span overflow for posting %d page %u.\n",
                                 candidate.m_postingID, pageID);
                    return ErrorCode::Fail;
                }

                const auto &payloadRequest = p_exWorkSpace->m_payloadReadRequests[payloadRequestIndex];
                const auto &diskRequest = p_exWorkSpace->m_payloadDiskRequests[payloadRequestIndex];
                if (payloadRequest.m_postingID != candidate.m_postingID || payloadRequest.m_pageID != pageID)
                {
                    SPTAGLIB_LOG(
                        Helper::LogLevel::LL_Error,
                        "Payload request span mismatch. posting=%d expectedPage=%u actualPosting=%d actualPage=%u\n",
                        candidate.m_postingID, pageID, payloadRequest.m_postingID, payloadRequest.m_pageID);
                    return ErrorCode::Fail;
                }

                const uint64_t pageBaseOffset = static_cast<uint64_t>(pageID) << PageSizeEx;
                if (payloadPhysicalOffset < pageBaseOffset)
                {
                    return ErrorCode::Fail;
                }

                const size_t pageLocalOffset = static_cast<size_t>(payloadPhysicalOffset - pageBaseOffset);
                if (pageLocalOffset >= payloadRequest.m_pageBytes)
                {
                    SPTAGLIB_LOG(
                        Helper::LogLevel::LL_Error,
                        "Payload page local offset overflow. posting=%d page=%u localOffset=%zu pageBytes=%u\n",
                        candidate.m_postingID, pageID, pageLocalOffset, payloadRequest.m_pageBytes);
                    return ErrorCode::Fail;
                }

                const size_t bytesFromPage =
                    std::min<size_t>(remainingBytes, payloadRequest.m_pageBytes - pageLocalOffset);
                if (singlePagePayload)
                {
                    exactVector = reinterpret_cast<const ValueType *>(diskRequest.m_buffer + pageLocalOffset);
                }
                else
                {
                    std::memcpy(p_exWorkSpace->m_payloadScratch.data() + scratchOffset,
                                diskRequest.m_buffer + pageLocalOffset, bytesFromPage);
                }
                payloadPhysicalOffset += bytesFromPage;
                scratchOffset += bytesFromPage;
                remainingBytes -= static_cast<uint32_t>(bytesFromPage);
                if (remainingBytes > 0)
                {
                    ++pageID;
                    ++payloadRequestIndex;
                }
            }
            payloadCopyMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStart).count();

            if (!singlePagePayload)
            {
                exactVector = reinterpret_cast<const ValueType *>(p_exWorkSpace->m_payloadScratch.data());
            }

            // P2: Time exact distance calculation
            auto distStart = std::chrono::steady_clock::now();
            float exactDist = p_index->ComputeDistance(queryResults.GetTarget(), exactVector);
            exactDistanceMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - distStart).count();

            // P2: Time result insertion
            auto insertStart = std::chrono::steady_clock::now();
            queryResults.AddPoint(candidate.m_vectorID, exactDist);
            resultInsertionMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - insertStart).count();

            if (p_stats)
            {
                p_stats->m_distanceEvaluatedCount += 1;
            }
        }
        subPhaseEnd = std::chrono::steady_clock::now();

        if (p_stats)
        {
            p_stats->m_batchReadTotalLatencyMs += payloadReadLatencyMs;
            p_stats->m_diskReadLatency += payloadReadLatencyMs;
            // P2: Store sub-phase timing
            p_stats->m_payloadReadWaitMs += payloadReadWaitMs;
            p_stats->m_payloadCopyMs += payloadCopyMs;
            p_stats->m_exactDistanceMs += exactDistanceMs;
            p_stats->m_resultInsertionMs += resultInsertionMs;
        }

        p_queryResults.SetScanned(static_cast<int>(p_exWorkSpace->m_mergedCandidates.size()));
        if (p_stats)
        {
            p_stats->m_finalResultCount = static_cast<uint64_t>(p_queryResults.GetResultNum());
        }
        return ErrorCode::Success;
    }

    // P2: Compute miss-case attribution for coarse recall analysis
    void ComputeMissCaseAttribution(ExtraWorkSpace *p_exWorkSpace, COMMON::QueryResultSet<ValueType> &queryResults,
                                    QueryResult &p_queryResults, const std::set<int> &truth, SearchStats *p_stats)
    {
        const size_t truthSize = truth.size();
        if (truthSize == 0)
            return;

        p_stats->m_truthCount = static_cast<uint64_t>(truthSize);

        std::unordered_set<int> coarseVectorIDs;
        coarseVectorIDs.reserve(p_exWorkSpace->m_coarseCandidates.size());
        for (const auto &candidate : p_exWorkSpace->m_coarseCandidates)
        {
            if (candidate.m_vectorID >= 0)
                coarseVectorIDs.insert(candidate.m_vectorID);
        }

        std::unordered_set<int> dedupedVectorIDs;
        dedupedVectorIDs.reserve(p_exWorkSpace->m_bestCoarseCandidateByVector.size());
        for (const auto &candidatePair : p_exWorkSpace->m_bestCoarseCandidateByVector)
        {
            if (candidatePair.first >= 0)
                dedupedVectorIDs.insert(static_cast<int>(candidatePair.first));
        }

        std::unordered_set<int> rerankVectorIDs;
        rerankVectorIDs.reserve(p_exWorkSpace->m_mergedCandidates.size());
        for (const auto &candidate : p_exWorkSpace->m_mergedCandidates)
        {
            if (candidate.m_vectorID >= 0)
                rerankVectorIDs.insert(candidate.m_vectorID);
        }

        // Collect requested final topK vector IDs. QueryResultSet is still heap-ordered here, so do not rely on
        // GetResult(i) order before SearchDiskIndex performs SortResult().
        std::unordered_set<int> finalVectorIDs;
        std::vector<BasicResult> finalResults;
        finalResults.reserve(p_queryResults.GetResultNum());
        for (int i = 0; i < p_queryResults.GetResultNum(); ++i)
        {
            const BasicResult *result = p_queryResults.GetResult(i);
            if (result != nullptr && result->VID >= 0)
            {
                finalResults.emplace_back(*result);
            }
        }
        std::sort(finalResults.begin(), finalResults.end(), COMMON::Compare);
        const size_t resultLimit = static_cast<size_t>(
            std::max<uint64_t>(1, p_stats->m_resultLimit > 0 ? p_stats->m_resultLimit : p_queryResults.GetResultNum()));
        finalVectorIDs.reserve(std::min(finalResults.size(), resultLimit));
        for (size_t i = 0; i < finalResults.size() && i < resultLimit; ++i)
        {
            finalVectorIDs.insert(finalResults[i].VID);
        }

        // Count truth found at each stage
        uint64_t coarseRecall = 0;
        uint64_t dedupedRecall = 0;
        uint64_t rerankRecall = 0;
        uint64_t finalRecall = 0;
        uint64_t recoveredByHeadResult = 0;
        uint64_t droppedByPerPostingTopR = 0;
        uint64_t droppedByGlobalTopR = 0;
        uint64_t droppedByRerankTopK = 0;
        uint64_t missingPostingNotVisited = 0;
        uint64_t missingNotInPosting = 0;

        for (int truthVID : truth)
        {
            const bool observedInScannedPosting =
                p_exWorkSpace->m_truthBestScannedRank.find(static_cast<SizeType>(truthVID)) !=
                p_exWorkSpace->m_truthBestScannedRank.end();
            bool inCoarse = coarseVectorIDs.count(truthVID) > 0;
            bool inDeduped = dedupedVectorIDs.count(truthVID) > 0;
            bool inRerank = rerankVectorIDs.count(truthVID) > 0;
            bool inFinal = finalVectorIDs.count(truthVID) > 0;

            if (inFinal)
                finalRecall++;

            if (inDeduped)
                dedupedRecall++;

            if (inRerank)
                rerankRecall++;

            if (inCoarse)
                coarseRecall++;

            if (inFinal)
            {
                if (!inRerank)
                {
                    recoveredByHeadResult++;
                }
                continue;
            }

            if (!observedInScannedPosting)
            {
                missingPostingNotVisited++;
            }
            else if (!inCoarse)
            {
                droppedByPerPostingTopR++;
            }
            else if (!inRerank)
            {
                droppedByGlobalTopR++;
            }
            else if (!inFinal)
            {
                droppedByRerankTopK++;
            }
        }

        p_stats->m_coarseRecall = coarseRecall;
        p_stats->m_coarseRecallAfterDedupe = dedupedRecall;
        p_stats->m_rerankRecall = rerankRecall;
        p_stats->m_finalRecall = finalRecall;
        p_stats->m_truthRecoveredByHeadResult = recoveredByHeadResult;
        p_stats->m_truthDroppedByPerPostingTopR = droppedByPerPostingTopR;
        p_stats->m_truthDroppedByGlobalTopR = droppedByGlobalTopR;
        p_stats->m_truthDroppedByRerankTopK = droppedByRerankTopK;
        p_stats->m_truthMissingPostingNotVisited = missingPostingNotVisited;
        p_stats->m_truthMissingNotInPosting = missingNotInPosting;
    }

    std::string GetPostingListFullData(int postingListId, size_t p_postingListSize, Selection &p_selections,
                                       std::shared_ptr<VectorSet> p_fullVectors, bool p_enableDeltaEncoding = false,
                                       bool p_enablePostingListRearrange = false, const ValueType *headVector = nullptr)
    {
        std::string postingListFullData("");
        std::string vectors("");
        std::string vectorIDs("");
        size_t selectIdx = p_selections.lower_bound(postingListId);
        // iterate over all the vectors in the posting list
        for (int i = 0; i < p_postingListSize; ++i)
        {
            if (p_selections[selectIdx].node != postingListId)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Selection ID NOT MATCH! node:%d offset:%zu\n", postingListId,
                             selectIdx);
                throw std::runtime_error("Selection ID mismatch");
            }
            std::string vectorID("");
            std::string vector("");

            int vid = p_selections[selectIdx++].tonode;
            vectorID.append(reinterpret_cast<char *>(&vid), sizeof(int));

            ValueType *p_vector = reinterpret_cast<ValueType *>(p_fullVectors->GetVector(vid));
            if (p_enableDeltaEncoding)
            {
                DimensionType n = p_fullVectors->Dimension();
                std::vector<ValueType> p_vector_delta(n);
                for (auto j = 0; j < n; j++)
                {
                    p_vector_delta[j] = p_vector[j] - headVector[j];
                }
                vector.append(reinterpret_cast<char *>(&p_vector_delta[0]), p_fullVectors->PerVectorDataSize());
            }
            else
            {
                vector.append(reinterpret_cast<char *>(p_vector), p_fullVectors->PerVectorDataSize());
            }

            if (p_enablePostingListRearrange)
            {
                vectorIDs += vectorID;
                vectors += vector;
            }
            else
            {
                postingListFullData += (vectorID + vector);
            }
        }
        if (p_enablePostingListRearrange)
        {
            return vectors + vectorIDs;
        }
        return postingListFullData;
    }

    bool BuildIndex(std::shared_ptr<Helper::VectorSetReader> &p_reader, std::shared_ptr<VectorIndex> p_headIndex,
                    Options &p_opt, COMMON::VersionLabel &p_versionMap,
                    COMMON::Dataset<std::uint64_t> &p_vectorTranslateMap, SizeType upperBound = -1)
    {
        std::string outputFile = p_opt.m_indexDirectory + FolderSep + p_opt.m_ssdIndex;
        if (outputFile.empty())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Output file can't be empty!\n");
            return false;
        }

        m_opt = &p_opt;
        if (p_opt.m_ssdPostingFormatVersion != 0 || p_opt.m_enableTwoStagePosting)
        {
            m_twoStageQuantizer = LoadQuantizerFromFile(p_opt.m_quantizerFilePath);
            if (m_twoStageQuantizer == nullptr)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Two-stage posting requires a quantizer file. path=%s\n",
                             p_opt.m_quantizerFilePath.c_str());
                return false;
            }
        }
        int numThreads = p_opt.m_iSSDNumberOfThreads;
        int candidateNum = p_opt.m_internalResultNum;
        std::unordered_map<SizeType, SizeType> headVectorIDS;
        if (p_opt.m_headIDFile.empty())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Not found VectorIDTranslate!\n");
            return false;
        }

        for (int i = 0; i < p_vectorTranslateMap.R(); i++)
        {
            headVectorIDS[static_cast<SizeType>(*(p_vectorTranslateMap[i]))] = i;
        }
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Loaded %u Vector IDs\n", static_cast<uint32_t>(headVectorIDS.size()));

        SizeType fullCount = 0;
        size_t vectorInfoSize = 0;
        {
            auto fullVectors = p_reader->GetVectorSet();
            fullCount = fullVectors->Count();
            vectorInfoSize = fullVectors->PerVectorDataSize() + sizeof(int);
        }
        if (upperBound > 0)
            fullCount = upperBound;

        p_versionMap.Initialize(fullCount, p_headIndex->m_iDataBlockSize, p_headIndex->m_iDataCapacity);

        Selection selections(static_cast<size_t>(fullCount) * p_opt.m_replicaCount, p_opt.m_tmpdir);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                     "Full vector count:%d Edge bytes:%llu selection size:%zu, capacity size:%zu\n", fullCount,
                     sizeof(Edge), selections.m_selections.size(), selections.m_selections.capacity());
        std::vector<std::atomic_int> replicaCount(fullCount);
        std::vector<std::atomic_int> postingListSize(p_headIndex->GetNumSamples());
        for (auto &pls : postingListSize)
            pls = 0;
        std::unordered_set<SizeType> emptySet;
        SizeType batchSize = (fullCount + p_opt.m_batches - 1) / p_opt.m_batches;

        auto t1 = std::chrono::high_resolution_clock::now();
        if (p_opt.m_batches > 1)
        {
            if (selections.SaveBatch() != ErrorCode::Success)
            {
                return false;
            }
        }
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Preparation done, start candidate searching.\n");
            SizeType sampleSize = p_opt.m_samples;
            std::vector<SizeType> samples(sampleSize, 0);
            for (int i = 0; i < p_opt.m_batches; i++)
            {
                SizeType start = i * batchSize;
                SizeType end = min(start + batchSize, fullCount);
                auto fullVectors = p_reader->GetVectorSet(start, end);
                if (p_opt.m_distCalcMethod == DistCalcMethod::Cosine && !p_reader->IsNormalized() &&
                    !p_headIndex->m_pQuantizer)
                    fullVectors->Normalize(p_opt.m_iSSDNumberOfThreads);

                if (p_opt.m_batches > 1)
                {
                    if (selections.LoadBatch(static_cast<size_t>(start) * p_opt.m_replicaCount,
                                             static_cast<size_t>(end) * p_opt.m_replicaCount) != ErrorCode::Success)
                    {
                        return false;
                    }
                    emptySet.clear();
                    for (auto &pair : headVectorIDS)
                    {
                        if (pair.first >= start && pair.first < end)
                            emptySet.insert(pair.first - start);
                    }
                }
                else
                {
                    for (auto &pair : headVectorIDS)
                    {
                        emptySet.insert(pair.first);
                    }
                }

                int sampleNum = 0;
                for (int j = start; j < end && sampleNum < sampleSize; j++)
                {
                    if (headVectorIDS.count(j) == 0)
                        samples[sampleNum++] = j - start;
                }

                float acc = 0;
                for (int j = 0; j < sampleNum; j++)
                {
                    COMMON::Utils::atomic_float_add(
                        &acc, COMMON::TruthSet::CalculateRecall(p_headIndex.get(), fullVectors->GetVector(samples[j]),
                                                                candidateNum));
                }
                acc = acc / sampleNum;
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                             "Batch %d vector(%d,%d) loaded with %d vectors (%zu) HeadIndex acc @%d:%f.\n", i, start,
                             end, fullVectors->Count(), selections.m_selections.size(), candidateNum, acc);

                p_headIndex->ApproximateRNG(fullVectors, emptySet, candidateNum, selections.m_selections.data(),
                                            p_opt.m_replicaCount, numThreads, p_opt.m_gpuSSDNumTrees,
                                            p_opt.m_gpuSSDLeafSize, p_opt.m_rngFactor, p_opt.m_numGPUs);
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Batch %d finished!\n", i);

                for (SizeType j = start; j < end; j++)
                {
                    replicaCount[j] = 0;
                    size_t vecOffset = j * (size_t)p_opt.m_replicaCount;
                    if (headVectorIDS.count(j) == 0)
                    {
                        for (int resNum = 0;
                             resNum < p_opt.m_replicaCount && selections[vecOffset + resNum].node != INT_MAX; resNum++)
                        {
                            ++postingListSize[selections[vecOffset + resNum].node];
                            selections[vecOffset + resNum].tonode = j;
                            ++replicaCount[j];
                        }
                    }
                    else if (!p_opt.m_excludehead)
                    {
                        selections[vecOffset].node = headVectorIDS[j];
                        selections[vecOffset].tonode = j;
                        ++postingListSize[selections[vecOffset].node];
                        ++replicaCount[j];
                    }
                }

                if (p_opt.m_batches > 1)
                {
                    if (selections.SaveBatch() != ErrorCode::Success)
                    {
                        return false;
                    }
                }
            }
        }
        auto t2 = std::chrono::high_resolution_clock::now();
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Searching replicas ended. Search Time: %.2lf mins\n",
                     ((double)std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count()) / 60.0);

        if (p_opt.m_batches > 1)
        {
            if (selections.LoadBatch(0, static_cast<size_t>(fullCount) * p_opt.m_replicaCount) != ErrorCode::Success)
            {
                return false;
            }
        }

        // Sort results either in CPU or GPU
        VectorIndex::SortSelections(&selections.m_selections);

        auto t3 = std::chrono::high_resolution_clock::now();
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Time to sort selections:%.2lf sec.\n",
                     ((double)std::chrono::duration_cast<std::chrono::seconds>(t3 - t2).count()) +
                         ((double)std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()) / 1000);

        int postingSizeLimit = INT_MAX;
        if (p_opt.m_postingPageLimit > 0)
        {
            p_opt.m_postingPageLimit =
                max(p_opt.m_postingPageLimit,
                    static_cast<int>((p_opt.m_postingVectorLimit * vectorInfoSize + PageSize - 1) / PageSize));
            p_opt.m_searchPostingPageLimit = p_opt.m_postingPageLimit;
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Build index with posting page limit:%d\n",
                         p_opt.m_postingPageLimit);
            postingSizeLimit = static_cast<int>(p_opt.m_postingPageLimit * PageSize / vectorInfoSize);
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Posting size limit: %d\n", postingSizeLimit);

        {
            std::vector<int> replicaCountDist(p_opt.m_replicaCount + 1, 0);
            for (int i = 0; i < replicaCount.size(); ++i)
            {
                if (headVectorIDS.count(i) > 0)
                    continue;
                ++replicaCountDist[replicaCount[i]];
            }

            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Before Posting Cut:\n");
            for (int i = 0; i < replicaCountDist.size(); ++i)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Replica Count Dist: %d, %d\n", i, replicaCountDist[i]);
            }
        }
        {
            std::vector<std::thread> mythreads;
            mythreads.reserve(m_opt->m_iSSDNumberOfThreads);
            std::atomic_size_t sent(0);
            for (int tid = 0; tid < m_opt->m_iSSDNumberOfThreads; tid++)
            {
                mythreads.emplace_back([&, tid]() {
                    size_t i = 0;
                    while (true)
                    {
                        i = sent.fetch_add(1);
                        if (i < postingListSize.size())
                        {
                            if (postingListSize[i] <= postingSizeLimit)
                                continue;

                            std::size_t selectIdx =
                                std::lower_bound(selections.m_selections.begin(), selections.m_selections.end(), i,
                                                 Selection::g_edgeComparer) -
                                selections.m_selections.begin();

                            for (size_t dropID = postingSizeLimit; dropID < postingListSize[i]; ++dropID)
                            {
                                int tonode = selections.m_selections[selectIdx + dropID].tonode;
                                --replicaCount[tonode];
                            }
                            postingListSize[i] = postingSizeLimit;
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
        }
        if (p_opt.m_outputEmptyReplicaID)
        {
            std::vector<int> replicaCountDist(p_opt.m_replicaCount + 1, 0);
            auto ptr = SPTAG::f_createIO();
            if (ptr == nullptr || !ptr->Initialize("EmptyReplicaID.bin", std::ios::binary | std::ios::out))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Fail to create EmptyReplicaID.bin!\n");
                return false;
            }
            for (int i = 0; i < replicaCount.size(); ++i)
            {
                if (headVectorIDS.count(i) > 0)
                    continue;

                ++replicaCountDist[replicaCount[i]];

                if (replicaCount[i] < 2)
                {
                    long long vid = i;
                    if (ptr->WriteBinary(sizeof(vid), reinterpret_cast<char *>(&vid)) != sizeof(vid))
                    {
                        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failt to write EmptyReplicaID.bin!");
                        return false;
                    }
                }
            }

            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "After Posting Cut:\n");
            for (int i = 0; i < replicaCountDist.size(); ++i)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Replica Count Dist: %d, %d\n", i, replicaCountDist[i]);
            }
        }

        auto t4 = std::chrono::high_resolution_clock::now();
        SPTAGLIB_LOG(SPTAG::Helper::LogLevel::LL_Info, "Time to perform posting cut:%.2lf sec.\n",
                     ((double)std::chrono::duration_cast<std::chrono::seconds>(t4 - t3).count()) +
                         ((double)std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3).count()) / 1000);

        // number of posting lists per file
        size_t postingFileSize = (postingListSize.size() + p_opt.m_ssdIndexFileNum - 1) / p_opt.m_ssdIndexFileNum;
        std::vector<size_t> selectionsBatchOffset(p_opt.m_ssdIndexFileNum + 1, 0);
        for (int i = 0; i < p_opt.m_ssdIndexFileNum; i++)
        {
            size_t curPostingListEnd = min(postingListSize.size(), (i + 1) * postingFileSize);
            selectionsBatchOffset[i + 1] =
                std::lower_bound(selections.m_selections.begin(), selections.m_selections.end(),
                                 (SizeType)curPostingListEnd, Selection::g_edgeComparer) -
                selections.m_selections.begin();
        }

        if (p_opt.m_ssdIndexFileNum > 1)
        {
            if (selections.SaveBatch() != ErrorCode::Success)
            {
                return false;
            }
        }

        auto fullVectors = p_reader->GetVectorSet();
        if (p_opt.m_distCalcMethod == DistCalcMethod::Cosine && !p_reader->IsNormalized() && !p_headIndex->m_pQuantizer)
            fullVectors->Normalize(p_opt.m_iSSDNumberOfThreads);

        // iterate over files
        for (int i = 0; i < p_opt.m_ssdIndexFileNum; i++)
        {
            size_t curPostingListOffSet = i * postingFileSize;
            size_t curPostingListEnd = min(postingListSize.size(), (i + 1) * postingFileSize);
            // postingListSize: number of vectors in the posting list, type vector<int>
            std::vector<int> curPostingListSizes(postingListSize.begin() + curPostingListOffSet,
                                                 postingListSize.begin() + curPostingListEnd);

            std::vector<size_t> curPostingListBytes(curPostingListSizes.size());

            if (p_opt.m_ssdIndexFileNum > 1)
            {
                if (selections.LoadBatch(selectionsBatchOffset[i], selectionsBatchOffset[i + 1]) != ErrorCode::Success)
                {
                    return false;
                }
            }
            // create compressor
            if (p_opt.m_enableDataCompression && i == 0)
            {
                m_pCompressor = std::make_unique<Compressor>(p_opt.m_zstdCompressLevel, p_opt.m_dictBufferCapacity);
                // train dict
                if (p_opt.m_enableDictTraining)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Training dictionary...\n");
                    std::string samplesBuffer("");
                    std::vector<size_t> samplesSizes;
                    for (int j = 0; j < curPostingListSizes.size(); j++)
                    {
                        if (curPostingListSizes[j] == 0)
                        {
                            continue;
                        }
                        ValueType *headVector = nullptr;
                        if (p_opt.m_enableDeltaEncoding)
                        {
                            headVector = (ValueType *)p_headIndex->GetSample(j);
                        }
                        std::string postingListFullData = GetPostingListFullData(
                            j, curPostingListSizes[j], selections, fullVectors, p_opt.m_enableDeltaEncoding,
                            p_opt.m_enablePostingListRearrange, headVector);

                        samplesBuffer += postingListFullData;
                        samplesSizes.push_back(postingListFullData.size());
                        if (samplesBuffer.size() > p_opt.m_minDictTraingBufferSize)
                            break;
                    }
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                                 "Using the first %zu postingLists to train dictionary... \n", samplesSizes.size());
                    std::size_t dictSize =
                        m_pCompressor->TrainDict(samplesBuffer, &samplesSizes[0], (unsigned int)samplesSizes.size());
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Dictionary trained, dictionary size: %zu \n", dictSize);
                }
            }

            if (p_opt.m_enableDataCompression)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Getting compressed size of each posting list...\n");
                std::vector<std::thread> mythreads;
                mythreads.reserve(m_opt->m_iSSDNumberOfThreads);
                std::atomic_size_t sent(0);
                for (int tid = 0; tid < m_opt->m_iSSDNumberOfThreads; tid++)
                {
                    mythreads.emplace_back([&, tid]() {
                        size_t j = 0;
                        while (true)
                        {
                            j = sent.fetch_add(1);
                            if (j < curPostingListSizes.size())
                            {
                                SizeType postingListId = j + (SizeType)curPostingListOffSet;
                                // do not compress if no data
                                if (postingListSize[postingListId] == 0)
                                {
                                    curPostingListBytes[j] = 0;
                                    continue;
                                }
                                ValueType *headVector = nullptr;
                                if (p_opt.m_enableDeltaEncoding)
                                {
                                    headVector = (ValueType *)p_headIndex->GetSample(postingListId);
                                }
                                std::string postingListFullData = GetPostingListFullData(
                                    postingListId, postingListSize[postingListId], selections, fullVectors,
                                    p_opt.m_enableDeltaEncoding, p_opt.m_enablePostingListRearrange, headVector);
                                size_t sizeToCompress = postingListSize[postingListId] * vectorInfoSize;
                                if (sizeToCompress != postingListFullData.size())
                                {
                                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                                                 "Size to compress NOT MATCH! PostingListFullData size: %zu "
                                                 "sizeToCompress: %zu \n",
                                                 postingListFullData.size(), sizeToCompress);
                                }
                                curPostingListBytes[j] =
                                    m_pCompressor->GetCompressedSize(postingListFullData, p_opt.m_enableDictTraining);
                                if (postingListId % 10000 == 0 ||
                                    curPostingListBytes[j] > static_cast<uint64_t>(p_opt.m_postingPageLimit) * PageSize)
                                {
                                    SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                                                 "Posting list %d/%d, compressed size: %d, compression ratio: %.4f\n",
                                                 postingListId, postingListSize.size(), curPostingListBytes[j],
                                                 curPostingListBytes[j] / float(sizeToCompress));
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

                SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                             "Getted compressed size for all the %d posting lists in SSD Index file %d.\n",
                             curPostingListBytes.size(), i);
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Mean compressed size: %.4f \n",
                             std::accumulate(curPostingListBytes.begin(), curPostingListBytes.end(), 0.0) /
                                 curPostingListBytes.size());
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Mean compression ratio: %.4f \n",
                             std::accumulate(curPostingListBytes.begin(), curPostingListBytes.end(), 0.0) /
                                 (std::accumulate(curPostingListSizes.begin(), curPostingListSizes.end(), 0.0) *
                                  vectorInfoSize));
            }
            else
            {
                auto quantizer = GetPostingQuantizer(p_headIndex);
                if (quantizer == nullptr)
                {
                    throw std::runtime_error("Two-stage posting build requires a posting quantizer");
                }

                if (m_opt != nullptr && m_opt->m_enableChunkedPosting)
                {
                    std::vector<std::thread> mythreads;
                    mythreads.reserve(m_opt->m_iSSDNumberOfThreads);
                    std::atomic_size_t sent(0);
                    for (int tid = 0; tid < m_opt->m_iSSDNumberOfThreads; ++tid)
                    {
                        mythreads.emplace_back([&, quantizer]() {
                            while (true)
                            {
                                size_t j = sent.fetch_add(1);
                                if (j >= curPostingListSizes.size())
                                    return;
                                if (curPostingListSizes[j] == 0)
                                {
                                    curPostingListBytes[j] = 0;
                                    continue;
                                }

                                const SizeType postingListId =
                                    static_cast<SizeType>(j) + static_cast<SizeType>(curPostingListOffSet);
                                std::string postingListFullData =
                                    GetPostingListFullData(static_cast<int>(postingListId), curPostingListSizes[j],
                                                           selections, fullVectors, false, false, nullptr);
                                curPostingListBytes[j] =
                                    BuildChunkedPostingBlob(postingListId, postingListFullData, curPostingListSizes[j],
                                                            vectorInfoSize, quantizer, fullVectors->Dimension(),
                                                            GetPostingPayloadLayoutKind())
                                        .size();
                            }
                        });
                    }
                    for (auto &t : mythreads)
                    {
                        t.join();
                    }
                }
                else if (m_opt != nullptr && m_opt->m_enableTwoStagePosting)
                {
                    std::vector<std::thread> mythreads;
                    mythreads.reserve(m_opt->m_iSSDNumberOfThreads);
                    std::atomic_size_t sent(0);
                    for (int tid = 0; tid < m_opt->m_iSSDNumberOfThreads; ++tid)
                    {
                        mythreads.emplace_back([&, quantizer]() {
                            while (true)
                            {
                                size_t j = sent.fetch_add(1);
                                if (j >= curPostingListSizes.size())
                                    return;
                                if (curPostingListSizes[j] == 0)
                                {
                                    curPostingListBytes[j] = 0;
                                    continue;
                                }

                                const SizeType postingListId =
                                    static_cast<SizeType>(j) + static_cast<SizeType>(curPostingListOffSet);
                                std::string postingListFullData =
                                    GetPostingListFullData(static_cast<int>(postingListId), curPostingListSizes[j],
                                                           selections, fullVectors, false, false, nullptr);
                                curPostingListBytes[j] = BuildSingleChunkTwoStagePostingBlob(
                                                             postingListId, postingListFullData, curPostingListSizes[j],
                                                             vectorInfoSize, quantizer, GetPostingPayloadLayoutKind())
                                                             .size();
                            }
                        });
                    }
                    for (auto &t : mythreads)
                    {
                        t.join();
                    }
                }
                else
                {
                    for (int j = 0; j < curPostingListSizes.size(); j++)
                    {
                        curPostingListBytes[j] = curPostingListSizes[j] * vectorInfoSize;
                    }
                }
            }

            std::unique_ptr<int[]> postPageNum;
            std::unique_ptr<std::uint16_t[]> postPageOffset;
            std::vector<int> postingOrderInIndex;
            SelectPostingOffset(curPostingListBytes, postPageNum, postPageOffset, postingOrderInIndex);

            OutputSSDIndexFile((i == 0) ? outputFile : outputFile + "_" + std::to_string(i),
                               p_opt.m_enableDeltaEncoding, p_opt.m_enablePostingListRearrange,
                               p_opt.m_enableDataCompression, p_opt.m_enableDictTraining, vectorInfoSize,
                               curPostingListSizes, curPostingListBytes, p_headIndex, selections, postPageNum,
                               postPageOffset, postingOrderInIndex, fullVectors, curPostingListOffSet);
        }

        SavePostingFormatMetadata(outputFile);
        if (p_opt.m_ssdIndexFileNum > 1)
        {
            for (int i = 1; i < p_opt.m_ssdIndexFileNum; ++i)
            {
                SavePostingFormatMetadata(outputFile + "_" + std::to_string(i));
            }
        }
        p_versionMap.Save(p_opt.m_indexDirectory + FolderSep + p_opt.m_deleteIDFile);

        auto t5 = std::chrono::high_resolution_clock::now();
        auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(t5 - t1).count();
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Total used time: %.2lf minutes (about %.2lf hours).\n",
                     elapsedSeconds / 60.0, elapsedSeconds / 3600.0);
        return true;
    }

    virtual bool CheckValidPosting(SizeType postingID)
    {
        return m_listInfos[postingID].listEleCount != 0;
    }

    virtual ErrorCode CheckPosting(SizeType postingID, std::vector<std::uint8_t> *visited = nullptr,
                                   ExtraWorkSpace *p_exWorkSpace = nullptr) override
    {
        if (postingID < 0 || postingID >= m_totalListCount)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "[CheckPosting]: Error postingID %d (should be 0 ~ %d)\n",
                         postingID, m_totalListCount);
            return ErrorCode::Key_OverFlow;
        }
        if (m_listInfos[postingID].listEleCount < 0)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "[CheckPosting]: postingID %d has wrong size:%d\n", postingID,
                         m_listInfos[postingID].listEleCount);
            return ErrorCode::Posting_SizeError;
        }
        return ErrorCode::Success;
    }

    virtual ErrorCode GetPostingDebug(ExtraWorkSpace *p_exWorkSpace, std::shared_ptr<VectorIndex> p_index, SizeType vid,
                                      std::vector<SizeType> &VIDs, std::shared_ptr<VectorSet> &vecs)
    {
        VIDs.clear();

        SizeType curPostingID = vid;
        ListInfo *listInfo = &(m_listInfos[curPostingID]);
        VIDs.resize(listInfo->listEleCount);
        ByteArray vector_array = ByteArray::Alloc(sizeof(ValueType) * listInfo->listEleCount * m_iDataDimension);
        vecs.reset(
            new BasicVectorSet(vector_array, GetEnumValueType<ValueType>(), m_iDataDimension, listInfo->listEleCount));

        int fileid = m_oneContext ? 0 : curPostingID / m_listPerFile;

#ifndef BATCH_READ
        Helper::DiskIO *indexFile = m_indexFiles[fileid].get();
#endif

        size_t totalBytes = (static_cast<size_t>(listInfo->listPageCount) << PageSizeEx);

#ifdef ASYNC_READ
        auto &request = p_exWorkSpace->m_diskRequests[0];
        request.m_offset = listInfo->listOffset;
        request.m_readSize = totalBytes;
        request.m_status = (fileid << 16) | (request.m_status & 0xffff);
        request.m_payload = (void *)listInfo;
        request.m_success = false;

#ifdef BATCH_READ // async batch read
        request.m_callback = [&p_exWorkSpace, &vecs, &VIDs, &p_index, &request, this](bool success) {
            char *buffer = request.m_buffer;
            ListInfo *listInfo = (ListInfo *)(request.m_payload);

            // decompress posting list
            char *p_postingListFullData = buffer + listInfo->pageOffset;
            if (m_enableDataCompression)
            {
                DecompressPosting();
            }

            for (int i = 0; i < listInfo->listEleCount; i++)
            {
                uint64_t offsetVectorID, offsetVector;
                (this->*m_parsePosting)(offsetVectorID, offsetVector, i, listInfo->listEleCount);
                int vectorID = *(reinterpret_cast<int *>(p_postingListFullData + offsetVectorID));
                (this->*m_parseEncoding)(p_index, listInfo, (ValueType *)(p_postingListFullData + offsetVector));
                VIDs[i] = vectorID;
                auto outVec = vecs->GetVector(i);
                memcpy(outVec, (void *)(p_postingListFullData + offsetVector), sizeof(ValueType) * m_iDataDimension);
            }
        };
#else // async read
        request.m_callback = [&p_exWorkSpace, &request](bool success) { p_exWorkSpace->m_processIocp.push(&request); };

        ++unprocessed;
        if (!(indexFile->ReadFileAsync(request)))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read file!\n");
            unprocessed--;
        }
#endif
#else // sync read
        char *buffer = (char *)((p_exWorkSpace->m_pageBuffers[0]).GetBuffer());
        auto numRead = indexFile->ReadBinary(totalBytes, buffer, listInfo->listOffset);
        if (numRead != totalBytes)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, acutal: %llu.\n",
                         m_extraFullGraphFile.c_str(), totalBytes, numRead);
            throw std::runtime_error("File read mismatch");
        }
        // decompress posting list
        char *p_postingListFullData = buffer + listInfo->pageOffset;
        if (m_enableDataCompression)
        {
            DecompressPosting();
        }

        for (int i = 0; i < listInfo->listEleCount; i++)
        {
            uint64_t offsetVectorID, offsetVector;
            (this->*m_parsePosting)(offsetVectorID, offsetVector, i, listInfo->listEleCount);
            int vectorID = *(reinterpret_cast<int *>(p_postingListFullData + offsetVectorID));
            (this->*m_parseEncoding)(p_index, listInfo, (ValueType *)(p_postingListFullData + offsetVector));
            VIDs[i] = vectorID;
            auto outVec = vecs->GetVector(i);
            memcpy(outVec, (void *)(p_postingListFullData + offsetVector), sizeof(ValueType) * m_iDataDimension);
        }
#endif
        return ErrorCode::Success;
    }

  private:
    struct ListInfo
    {
        std::size_t listTotalBytes = 0;

        int listEleCount = 0;

        std::uint16_t listPageCount = 0;

        std::uint64_t listOffset = 0;

        std::uint16_t pageOffset = 0;
    };

    struct PostingRuntimeMetadataCache
    {
        bool m_valid = false;
        bool m_isChunked = false;
        NewPostingHeader m_header;
        size_t m_codeBaseOffset = 0;
        NewPostingChunkDirectoryEntry m_singleChunkEntry;
        std::vector<NewPostingChunkDirectoryEntryV2> m_chunkEntries;
        std::vector<ValueType> m_centroids;
        std::vector<uint8_t> m_codeCache;
    };

    static size_t AlignToPageSize(size_t bytes)
    {
        return ((bytes + PageSize - 1) >> PageSizeEx) << PageSizeEx;
    }

    ErrorCode LoadPostingRuntimeCodeCache(Helper::DiskIO *indexFile, const ListInfo &listInfo, int postingID,
                                          PostingRuntimeMetadataCache &cache)
    {
        if (listInfo.listEleCount == 0 || listInfo.listPageCount == 0)
        {
            cache.m_codeCache.clear();
            return ErrorCode::Success;
        }
        if (!cache.m_valid)
        {
            return ErrorCode::Fail;
        }
        if (cache.m_header.m_codeBytes == 0)
        {
            cache.m_codeCache.clear();
            return ErrorCode::Success;
        }
        if (indexFile == nullptr)
        {
            return ErrorCode::DiskIOFail;
        }

        const size_t codeBytes = static_cast<size_t>(cache.m_header.m_codeBytes);
        if (!cache.m_isChunked)
        {
            cache.m_codeBaseOffset = cache.m_singleChunkEntry.m_codeOffset;
        }
        else
        {
            if (cache.m_chunkEntries.empty())
            {
                return ErrorCode::Fail;
            }
            cache.m_codeBaseOffset = cache.m_chunkEntries.front().m_codeOffset;
        }

        const uint64_t codeFileOffset = listInfo.listOffset + static_cast<uint64_t>(listInfo.pageOffset) +
                                        static_cast<uint64_t>(cache.m_codeBaseOffset);
        cache.m_codeCache.resize(codeBytes);

        // Prefer an exact subrange read for load-time cache population. The steady-state query path
        // never touches this loader, so there is no benefit in forcing page-aligned inflation here.
        auto numRead =
            indexFile->ReadBinary(codeBytes, reinterpret_cast<char *>(cache.m_codeCache.data()), codeFileOffset);
        if (numRead == codeBytes)
        {
            return ErrorCode::Success;
        }

        const uint64_t alignedCodeOffset = codeFileOffset & ~static_cast<uint64_t>(PageSize - 1);
        const size_t codePrefix = static_cast<size_t>(codeFileOffset - alignedCodeOffset);
        const size_t alignedReadBytes = AlignToPageSize(codePrefix + codeBytes);
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
                     "Posting %d exact code cache read fell back to aligned read. expected=%zu actual=%llu "
                     "fileOffset=%llu alignedOffset=%llu prefix=%zu readBytes=%zu isChunked=%d\n",
                     postingID, codeBytes, static_cast<unsigned long long>(numRead),
                     static_cast<unsigned long long>(codeFileOffset),
                     static_cast<unsigned long long>(alignedCodeOffset), codePrefix, alignedReadBytes,
                     cache.m_isChunked ? 1 : 0);
        Helper::PageBuffer<std::uint8_t> codeBuffer;
        codeBuffer.ReservePageBuffer(alignedReadBytes);
        char *codeReadBuffer = reinterpret_cast<char *>(codeBuffer.GetBuffer());

        numRead = indexFile->ReadBinary(alignedReadBytes, codeReadBuffer, alignedCodeOffset);
        if (numRead != alignedReadBytes)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Posting %d code cache read mismatch. expected=%zu actual=%llu offset=%llu prefix=%zu "
                         "base=%zu codeBytes=%zu totalBytes=%zu\n",
                         postingID, alignedReadBytes, static_cast<unsigned long long>(numRead),
                         static_cast<unsigned long long>(alignedCodeOffset), codePrefix, cache.m_codeBaseOffset,
                         codeBytes, static_cast<size_t>(listInfo.listPageCount) << PageSizeEx);
            return ErrorCode::DiskIOFail;
        }

        std::memcpy(cache.m_codeCache.data(), codeReadBuffer + codePrefix, codeBytes);
        return ErrorCode::Success;
    }

    ErrorCode LoadPostingRuntimeMetadata(Helper::DiskIO *indexFile, const ListInfo &listInfo, int postingID,
                                         PostingRuntimeMetadataCache &cache)
    {
        cache = PostingRuntimeMetadataCache();
        if (listInfo.listEleCount == 0 || listInfo.listPageCount == 0)
        {
            return ErrorCode::Success;
        }
        if (indexFile == nullptr)
        {
            return ErrorCode::DiskIOFail;
        }

        const size_t totalBytes = static_cast<size_t>(listInfo.listPageCount) << PageSizeEx;
        if (totalBytes <= listInfo.pageOffset)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Posting %d has invalid total bytes/page offset while caching metadata. total=%zu "
                         "pageOffset=%u\n",
                         postingID, totalBytes, listInfo.pageOffset);
            return ErrorCode::Fail;
        }

        const size_t firstReadBytes = GetInitialPostingMetadataReadBytes(listInfo.pageOffset, totalBytes);
        Helper::PageBuffer<std::uint8_t> metadataBuffer;
        metadataBuffer.ReservePageBuffer(firstReadBytes);
        char *postingMetadataBuffer = reinterpret_cast<char *>(metadataBuffer.GetBuffer());
        auto numRead = indexFile->ReadBinary(firstReadBytes, postingMetadataBuffer, listInfo.listOffset);
        if (numRead != firstReadBytes)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, actual: %llu.\n",
                         m_extraFullGraphFile.c_str(), firstReadBytes, numRead);
            return ErrorCode::DiskIOFail;
        }

        const char *postingBase = postingMetadataBuffer + listInfo.pageOffset;
        const size_t availableBytes = firstReadBytes - listInfo.pageOffset;
        if (availableBytes < sizeof(NewPostingHeader) + sizeof(NewPostingChunkDirectoryEntry))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Posting %d too small for new two-stage header.\n", postingID);
            return ErrorCode::Fail;
        }

        NewPostingHeader header;
        std::memcpy(&header, postingBase, sizeof(NewPostingHeader));
        if (header.m_magic != 0x53504732 || header.m_version != 2 || header.m_chunkCount == 0)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Posting %d has invalid new-format header. magic=%u version=%u chunks=%u\n", postingID,
                         header.m_magic, header.m_version, header.m_chunkCount);
            return ErrorCode::Fail;
        }

        const size_t directoryBytes =
            IsChunkedTwoStagePostingFormat()
                ? static_cast<size_t>(header.m_chunkCount) * sizeof(NewPostingChunkDirectoryEntryV2)
                : static_cast<size_t>(header.m_chunkCount) * sizeof(NewPostingChunkDirectoryEntry);
        const size_t centroidBytes =
            IsChunkedTwoStagePostingFormat()
                ? static_cast<size_t>(header.m_chunkCount) * static_cast<size_t>(m_iDataDimension) * sizeof(ValueType)
                : 0;
        const size_t metadataBytes =
            static_cast<size_t>(listInfo.pageOffset) + sizeof(NewPostingHeader) + directoryBytes + centroidBytes;
        if (metadataBytes > totalBytes)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Posting %d metadata bytes exceed posting bytes while caching. metadata=%zu total=%zu\n",
                         postingID, metadataBytes, totalBytes);
            return ErrorCode::Fail;
        }

        if (metadataBytes > firstReadBytes)
        {
            const size_t alignedMetadataReadBytes = std::min(totalBytes, AlignToPageSize(metadataBytes));
            if (alignedMetadataReadBytes < metadataBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d metadata alignment underflow while caching. metadata=%zu aligned=%zu "
                             "total=%zu\n",
                             postingID, metadataBytes, alignedMetadataReadBytes, totalBytes);
                return ErrorCode::Fail;
            }

            metadataBuffer.ReservePageBuffer(alignedMetadataReadBytes);
            postingMetadataBuffer = reinterpret_cast<char *>(metadataBuffer.GetBuffer());
            numRead = indexFile->ReadBinary(alignedMetadataReadBytes, postingMetadataBuffer, listInfo.listOffset);
            if (numRead != alignedMetadataReadBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, actual: %llu.\n",
                             m_extraFullGraphFile.c_str(), alignedMetadataReadBytes, numRead);
                return ErrorCode::DiskIOFail;
            }
        }

        postingBase = postingMetadataBuffer + listInfo.pageOffset;
        cache.m_valid = true;
        cache.m_isChunked = IsChunkedTwoStagePostingFormat();
        cache.m_header = header;

        if (!cache.m_isChunked)
        {
            NewPostingChunkDirectoryEntry entry;
            std::memcpy(&entry, postingBase + sizeof(NewPostingHeader), sizeof(NewPostingChunkDirectoryEntry));
            cache.m_singleChunkEntry = entry;
        }
        else
        {
            cache.m_chunkEntries.resize(header.m_chunkCount);
            cache.m_centroids.resize(static_cast<size_t>(header.m_chunkCount) * static_cast<size_t>(m_iDataDimension));
            const char *directoryBase = postingBase + sizeof(NewPostingHeader);
            const char *centroidBase = directoryBase + directoryBytes;
            for (uint32_t chunkID = 0; chunkID < header.m_chunkCount; ++chunkID)
            {
                std::memcpy(&cache.m_chunkEntries[chunkID],
                            directoryBase + static_cast<size_t>(chunkID) * sizeof(NewPostingChunkDirectoryEntryV2),
                            sizeof(NewPostingChunkDirectoryEntryV2));
                std::memcpy(cache.m_centroids.data() + static_cast<size_t>(chunkID) * m_iDataDimension,
                            centroidBase + static_cast<size_t>(chunkID) * m_iDataDimension * sizeof(ValueType),
                            static_cast<size_t>(m_iDataDimension) * sizeof(ValueType));
            }
        }

        return ErrorCode::Success;
    }

    ErrorCode AppendPostingBlocksFromCache(const PostingRuntimeMetadataCache &cache, int postingID,
                                           const ListInfo &listInfo, COMMON::QueryResultSet<ValueType> &queryResults,
                                           bool useHardChunkPrune, ExtraWorkSpace *p_exWorkSpace, SearchStats *p_stats,
                                           uint32_t &totalChunkRecords, uint32_t &totalChunkCodeBytes,
                                           uint32_t &totalChunkPayloadBytes)
    {
        const NewPostingHeader &header = cache.m_header;
        if (!cache.m_valid || header.m_chunkCount == 0)
        {
            return ErrorCode::Fail;
        }

        if (!cache.m_isChunked)
        {
            if (header.m_chunkCount != 1)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Posting %d uses %u chunks with legacy two-stage layout.\n",
                             postingID, header.m_chunkCount);
                return ErrorCode::Fail;
            }

            const NewPostingChunkDirectoryEntry &entry = cache.m_singleChunkEntry;
            if (header.m_recordCount != entry.m_recordCount ||
                header.m_payloadRecordBytes != entry.m_payloadRecordBytes || header.m_codeBytes != entry.m_codeBytes ||
                header.m_payloadBytes != entry.m_payloadBytes ||
                entry.m_codeOffset + entry.m_codeBytes != entry.m_payloadOffset ||
                entry.m_payloadOffset > listInfo.listTotalBytes ||
                entry.m_payloadOffset + entry.m_payloadBytes > listInfo.listTotalBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Posting %d has invalid single-chunk metadata.\n", postingID);
                return ErrorCode::Fail;
            }

            PostingBlockInfo block;
            block.m_postingID = postingID;
            block.m_chunkID = 0;
            block.m_cachedCode =
                cache.m_codeCache.empty() ? nullptr : reinterpret_cast<const char *>(cache.m_codeCache.data());
            block.m_codeOffset = entry.m_codeOffset;
            block.m_codeBytes = entry.m_codeBytes;
            block.m_codeRecordBytes = header.m_codeRecordBytes;
            block.m_payloadOffset = entry.m_payloadOffset;
            block.m_payloadBytes = entry.m_payloadBytes;
            block.m_payloadRecordBytes = entry.m_payloadRecordBytes;
            block.m_recordCount = entry.m_recordCount;
            p_exWorkSpace->m_postingBlocks.emplace_back(block);

            totalChunkRecords = entry.m_recordCount;
            totalChunkCodeBytes = entry.m_codeBytes;
            totalChunkPayloadBytes = entry.m_payloadBytes;
            return ErrorCode::Success;
        }

        if (cache.m_chunkEntries.size() != header.m_chunkCount ||
            cache.m_centroids.size() < static_cast<size_t>(header.m_chunkCount) * static_cast<size_t>(m_iDataDimension))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Posting %d has incomplete cached chunk metadata.\n", postingID);
            return ErrorCode::Fail;
        }

        for (uint32_t chunkID = 1; chunkID < header.m_chunkCount; ++chunkID)
        {
            const auto &prev = cache.m_chunkEntries[chunkID - 1];
            const auto &curr = cache.m_chunkEntries[chunkID];
            if (curr.m_codeOffset != prev.m_codeOffset + prev.m_codeBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d chunk %u code blocks are not contiguous. prevEnd=%u currStart=%u\n", postingID,
                             chunkID, prev.m_codeOffset + prev.m_codeBytes, curr.m_codeOffset);
                return ErrorCode::Fail;
            }
        }

        for (uint32_t chunkID = 0; chunkID < header.m_chunkCount; ++chunkID)
        {
            const NewPostingChunkDirectoryEntryV2 &entry = cache.m_chunkEntries[chunkID];
            if (entry.m_centroidBytes != static_cast<uint32_t>(m_iDataDimension * sizeof(ValueType)) ||
                entry.m_recordCount == 0 || entry.m_codeOffset + entry.m_codeBytes > listInfo.listTotalBytes ||
                entry.m_payloadOffset + entry.m_payloadBytes > listInfo.listTotalBytes ||
                entry.m_codeOffset <
                    sizeof(NewPostingHeader) +
                        static_cast<size_t>(header.m_chunkCount) * sizeof(NewPostingChunkDirectoryEntryV2) +
                        static_cast<size_t>(header.m_chunkCount) * m_iDataDimension * sizeof(ValueType) ||
                entry.m_payloadOffset <
                    sizeof(NewPostingHeader) +
                        static_cast<size_t>(header.m_chunkCount) * sizeof(NewPostingChunkDirectoryEntryV2) +
                        static_cast<size_t>(header.m_chunkCount) * m_iDataDimension * sizeof(ValueType) +
                        header.m_codeBytes ||
                entry.m_centroidOffset + entry.m_centroidBytes > listInfo.listTotalBytes)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "Posting %d chunk %u has invalid cached chunk metadata. centroidOffset=%u "
                             "centroidBytes=%u codeOffset=%u codeBytes=%u payloadOffset=%u payloadBytes=%u\n",
                             postingID, chunkID, entry.m_centroidOffset, entry.m_centroidBytes, entry.m_codeOffset,
                             entry.m_codeBytes, entry.m_payloadOffset, entry.m_payloadBytes);
                return ErrorCode::Fail;
            }

            PostingBlockInfo block;
            block.m_postingID = postingID;
            block.m_chunkID = static_cast<int>(chunkID);
            block.m_centroidOffset = entry.m_centroidOffset;
            block.m_centroidBytes = entry.m_centroidBytes;
            block.m_cachedCode =
                cache.m_codeCache.empty()
                    ? nullptr
                    : reinterpret_cast<const char *>(cache.m_codeCache.data()) +
                          static_cast<size_t>(entry.m_codeOffset - cache.m_chunkEntries.front().m_codeOffset);
            block.m_codeOffset = entry.m_codeOffset;
            block.m_codeBytes = entry.m_codeBytes;
            block.m_codeRecordBytes = header.m_codeRecordBytes;
            block.m_payloadOffset = entry.m_payloadOffset;
            block.m_payloadBytes = entry.m_payloadBytes;
            block.m_payloadRecordBytes = entry.m_payloadRecordBytes;
            block.m_recordCount = entry.m_recordCount;
            block.m_radius = entry.m_radius;

            totalChunkRecords += entry.m_recordCount;
            totalChunkCodeBytes += entry.m_codeBytes;
            totalChunkPayloadBytes += entry.m_payloadBytes;

            if (useHardChunkPrune)
            {
                const ValueType *centroid =
                    cache.m_centroids.data() + static_cast<size_t>(chunkID) * static_cast<size_t>(m_iDataDimension);
                block.m_lowerBound = ComputeChunkL2LowerBound(queryResults, centroid, entry.m_radius);
                if (block.m_lowerBound >= queryResults.worstDist())
                {
                    if (p_stats)
                    {
                        p_stats->m_chunksPruned += 1;
                    }
                    continue;
                }
            }

            p_exWorkSpace->m_postingBlocks.emplace_back(block);
        }

        return ErrorCode::Success;
    }

    std::string GetPostingMetadataFilePath(const std::string &p_ssdIndexFile) const
    {
        return p_ssdIndexFile + ".meta";
    }

    PostingFormatMetadata LoadPostingFormatMetadata(const std::string &p_ssdIndexFile)
    {
        PostingFormatMetadata metadata;
        std::string metaFile = GetPostingMetadataFilePath(p_ssdIndexFile);
        if (!fileexists(metaFile.c_str()))
        {
            return metadata;
        }

        Helper::IniReader reader;
        if (reader.LoadIniFile(metaFile) != ErrorCode::Success)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to load posting metadata file: %s\n", metaFile.c_str());
            throw std::runtime_error("Failed to load posting metadata file");
        }

        metadata.m_exists = true;
        auto magic = reader.GetParameter<std::string>("Meta", "Magic", std::string());
        if (!Helper::StrUtils::StrEqualIgnoreCase(magic.c_str(), "SPTAG_SSD_POSTING_META_V1"))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Unknown posting metadata magic in %s\n", metaFile.c_str());
            throw std::runtime_error("Unknown posting metadata magic");
        }

        metadata.m_formatVersion = reader.GetParameter<int>("Meta", "FormatVersion", 0);
        metadata.m_layoutType = reader.GetParameter<std::string>("Meta", "LayoutType", std::string("legacy"));
        metadata.m_codeType = reader.GetParameter<std::string>("Meta", "CodeType", std::string("None"));
        metadata.m_chunkPruneMode = reader.GetParameter<std::string>("Meta", "ChunkPruneMode", std::string("None"));
        metadata.m_payloadLayout =
            reader.GetParameter<std::string>("Meta", "PayloadLayout", std::string("legacy_full_vector"));
        return metadata;
    }

    std::shared_ptr<SPTAG::COMMON::IQuantizer> LoadQuantizerFromFile(const std::string &quantizerFilePath)
    {
        if (quantizerFilePath.empty())
        {
            return nullptr;
        }

        auto ptr = SPTAG::f_createIO();
        if (ptr == nullptr || !ptr->Initialize(quantizerFilePath.c_str(), std::ios::binary | std::ios::in))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open quantizer file %s\n", quantizerFilePath.c_str());
            return nullptr;
        }

        auto quantizer = SPTAG::COMMON::IQuantizer::LoadIQuantizer(ptr);
        if (quantizer == nullptr)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to load quantizer from file %s\n",
                         quantizerFilePath.c_str());
        }
        return quantizer;
    }

    std::shared_ptr<SPTAG::COMMON::IQuantizer> GetPostingQuantizer(const std::shared_ptr<VectorIndex> &p_index) const
    {
        if (m_twoStageQuantizer != nullptr)
        {
            return m_twoStageQuantizer;
        }
        return p_index ? p_index->GetQuantizer() : nullptr;
    }

    void SavePostingFormatMetadata(const std::string &p_ssdIndexFile)
    {
        std::string metaFile = GetPostingMetadataFilePath(p_ssdIndexFile);
        auto ptr = SPTAG::f_createIO();
        if (ptr == nullptr || !ptr->Initialize(metaFile.c_str(), std::ios::binary | std::ios::out))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open posting metadata file %s\n", metaFile.c_str());
            throw std::runtime_error("Failed to open posting metadata file");
        }

        auto writeLine = [&](const std::string &line) {
            if (ptr->WriteString(line.c_str()) == 0)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write posting metadata file %s\n",
                             metaFile.c_str());
                throw std::runtime_error("Failed to write posting metadata file");
            }
        };

        writeLine("[Meta]\n");
        writeLine("Magic=SPTAG_SSD_POSTING_META_V1\n");
        if (m_opt != nullptr && m_opt->m_ssdPostingFormatVersion >= 2)
        {
            writeLine("FormatVersion=2\n");
            writeLine(std::string("LayoutType=") +
                      (m_opt->m_enableChunkedPosting ? "chunked_twostage_v1" : "twostage_v1") + "\n");
            writeLine((std::string("CodeType=") +
                       (m_opt->m_postingCodeType.empty() ? std::string("FullPrecision") : m_opt->m_postingCodeType) +
                       "\n"));
            writeLine((std::string("ChunkPruneMode=") + NormalizeChunkPruneMode() + "\n"));
            writeLine(std::string("PayloadLayout=") + GetPostingPayloadLayoutName() + "\n");
        }
        else
        {
            writeLine("FormatVersion=0\n");
            writeLine("LayoutType=legacy\n");
            writeLine("CodeType=None\n");
            writeLine("ChunkPruneMode=None\n");
            writeLine("PayloadLayout=legacy_full_vector\n");
        }
    }

    int LoadingHeadInfo(const std::string &p_file, int p_postingPageLimit, std::vector<ListInfo> &p_listInfos)
    {
        auto ptr = SPTAG::f_createIO();
        if (ptr == nullptr || !ptr->Initialize(p_file.c_str(), std::ios::binary | std::ios::in))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open file: %s\n", p_file.c_str());
            throw std::runtime_error("Failed open file in LoadingHeadInfo");
        }
        m_pCompressor = std::make_unique<Compressor>(); // no need compress level to decompress

        int m_listCount;
        int m_totalDocumentCount;
        int m_listPageOffset;

        if (ptr->ReadBinary(sizeof(m_listCount), reinterpret_cast<char *>(&m_listCount)) != sizeof(m_listCount))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
            throw std::runtime_error("Failed read file in LoadingHeadInfo");
        }
        if (ptr->ReadBinary(sizeof(m_totalDocumentCount), reinterpret_cast<char *>(&m_totalDocumentCount)) !=
            sizeof(m_totalDocumentCount))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
            throw std::runtime_error("Failed read file in LoadingHeadInfo");
        }
        if (ptr->ReadBinary(sizeof(m_iDataDimension), reinterpret_cast<char *>(&m_iDataDimension)) !=
            sizeof(m_iDataDimension))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
            throw std::runtime_error("Failed read file in LoadingHeadInfo");
        }
        if (ptr->ReadBinary(sizeof(m_listPageOffset), reinterpret_cast<char *>(&m_listPageOffset)) !=
            sizeof(m_listPageOffset))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
            throw std::runtime_error("Failed read file in LoadingHeadInfo");
        }

        if (m_vectorInfoSize == 0)
            m_vectorInfoSize = m_iDataDimension * sizeof(ValueType) + sizeof(int);
        else if (m_vectorInfoSize != m_iDataDimension * sizeof(ValueType) + sizeof(int))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                         "Failed to read head info file! DataDimension and ValueType are not match!\n");
            throw std::runtime_error("DataDimension and ValueType don't match in LoadingHeadInfo");
        }

        size_t totalListCount = p_listInfos.size();
        p_listInfos.resize(totalListCount + m_listCount);
        if (IsTwoStagePostingFormat())
        {
            m_postingRuntimeMetadata.resize(totalListCount + m_listCount);
        }

        size_t totalListElementCount = 0;

        std::map<int, int> pageCountDist;

        size_t biglistCount = 0;
        size_t biglistElementCount = 0;
        int pageNum;
        for (int i = 0; i < m_listCount; ++i)
        {
            ListInfo *listInfo = &(p_listInfos[totalListCount + i]);

            if (m_enableDataCompression)
            {
                if (ptr->ReadBinary(sizeof(listInfo->listTotalBytes),
                                    reinterpret_cast<char *>(&(listInfo->listTotalBytes))) !=
                    sizeof(listInfo->listTotalBytes))
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
                    throw std::runtime_error("Failed read file in LoadingHeadInfo");
                }
            }
            if (ptr->ReadBinary(sizeof(pageNum), reinterpret_cast<char *>(&(pageNum))) != sizeof(pageNum))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
                throw std::runtime_error("Failed read file in LoadingHeadInfo");
            }
            if (ptr->ReadBinary(sizeof(listInfo->pageOffset), reinterpret_cast<char *>(&(listInfo->pageOffset))) !=
                sizeof(listInfo->pageOffset))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
                throw std::runtime_error("Failed read file in LoadingHeadInfo");
            }
            if (ptr->ReadBinary(sizeof(listInfo->listEleCount), reinterpret_cast<char *>(&(listInfo->listEleCount))) !=
                sizeof(listInfo->listEleCount))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
                throw std::runtime_error("Failed read file in LoadingHeadInfo");
            }
            if (ptr->ReadBinary(sizeof(listInfo->listPageCount),
                                reinterpret_cast<char *>(&(listInfo->listPageCount))) !=
                sizeof(listInfo->listPageCount))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
                throw std::runtime_error("Failed read file in LoadingHeadInfo");
            }
            listInfo->listOffset = (static_cast<uint64_t>(m_listPageOffset + pageNum) << PageSizeEx);
            if (IsTwoStagePostingFormat())
            {
                listInfo->listTotalBytes =
                    (static_cast<size_t>(listInfo->listPageCount) << PageSizeEx) - listInfo->pageOffset;
            }
            else if (!m_enableDataCompression)
            {
                listInfo->listTotalBytes = listInfo->listEleCount * m_vectorInfoSize;
                listInfo->listEleCount =
                    min(listInfo->listEleCount,
                        (min(static_cast<int>(listInfo->listPageCount), p_postingPageLimit) << PageSizeEx) /
                            m_vectorInfoSize);
                listInfo->listPageCount = static_cast<std::uint16_t>(
                    ceil((m_vectorInfoSize * listInfo->listEleCount + listInfo->pageOffset) * 1.0 / (1 << PageSizeEx)));
            }
            totalListElementCount += listInfo->listEleCount;
            int pageCount = listInfo->listPageCount;

            if (pageCount > 1)
            {
                ++biglistCount;
                biglistElementCount += listInfo->listEleCount;
            }

            if (pageCountDist.count(pageCount) == 0)
            {
                pageCountDist[pageCount] = 1;
            }
            else
            {
                pageCountDist[pageCount] += 1;
            }
        }

        if (IsTwoStagePostingFormat())
        {
            auto metadataPtr = SPTAG::f_createIO();
            if (metadataPtr == nullptr || !metadataPtr->Initialize(p_file.c_str(), std::ios::binary | std::ios::in))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to reopen file for posting metadata cache: %s\n",
                             p_file.c_str());
                throw std::runtime_error("Failed reopen file in LoadingHeadInfo");
            }

            for (int i = 0; i < m_listCount; ++i)
            {
                auto cacheRet = LoadPostingRuntimeMetadata(metadataPtr.get(), p_listInfos[totalListCount + i],
                                                           static_cast<int>(totalListCount) + i,
                                                           m_postingRuntimeMetadata[totalListCount + i]);
                if (cacheRet != ErrorCode::Success)
                {
                    throw std::runtime_error("Failed to cache new-format posting metadata");
                }
                cacheRet = LoadPostingRuntimeCodeCache(metadataPtr.get(), p_listInfos[totalListCount + i],
                                                       static_cast<int>(totalListCount) + i,
                                                       m_postingRuntimeMetadata[totalListCount + i]);
                if (cacheRet != ErrorCode::Success)
                {
                    throw std::runtime_error("Failed to cache new-format posting code");
                }
            }
        }

        if (m_enableDataCompression && m_enableDictTraining)
        {
            size_t dictBufferSize;
            if (ptr->ReadBinary(sizeof(size_t), reinterpret_cast<char *>(&dictBufferSize)) != sizeof(dictBufferSize))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
                throw std::runtime_error("Failed read file in LoadingHeadInfo");
            }
            char *dictBuffer = new char[dictBufferSize];
            if (ptr->ReadBinary(dictBufferSize, dictBuffer) != dictBufferSize)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file!\n");
                throw std::runtime_error("Failed read file in LoadingHeadInfo");
            }
            try
            {
                m_pCompressor->SetDictBuffer(std::string(dictBuffer, dictBufferSize));
            }
            catch (std::runtime_error &err)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read head info file: %s \n", err.what());
                throw std::runtime_error("Failed read file in LoadingHeadInfo");
            }
            delete[] dictBuffer;
        }

        SPTAGLIB_LOG(
            Helper::LogLevel::LL_Info,
            "Finish reading header info, list count %d, total doc count %d, dimension %d, list page offset %d.\n",
            m_listCount, m_totalDocumentCount, m_iDataDimension, m_listPageOffset);

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Big page (>4K): list count %zu, total element count %zu.\n",
                     biglistCount, biglistElementCount);

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Total Element Count: %llu\n", totalListElementCount);

        for (auto &ele : pageCountDist)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Page Count Dist: %d %d\n", ele.first, ele.second);
        }

        return m_listCount;
    }

    inline void ParsePostingListRearrange(uint64_t &offsetVectorID, uint64_t &offsetVector, int i, int eleCount)
    {
        offsetVectorID = (m_vectorInfoSize - sizeof(int)) * eleCount + sizeof(int) * i;
        offsetVector = (m_vectorInfoSize - sizeof(int)) * i;
    }

    inline void ParsePostingList(uint64_t &offsetVectorID, uint64_t &offsetVector, int i, int eleCount)
    {
        offsetVectorID = m_vectorInfoSize * i;
        offsetVector = offsetVectorID + sizeof(int);
    }

    inline void ParseDeltaEncoding(std::shared_ptr<VectorIndex> &p_index, ListInfo *p_info, ValueType *vector)
    {
        ValueType *headVector = (ValueType *)p_index->GetSample((SizeType)(p_info - m_listInfos.data()));
        COMMON::SIMDUtils::ComputeSum(vector, headVector, m_iDataDimension);
    }

    inline void ParseEncoding(std::shared_ptr<VectorIndex> &p_index, ListInfo *p_info, ValueType *vector)
    {
    }

    float ComputeL2DistanceToCentroid(const ValueType *vector, const std::vector<float> &centroid,
                                      DimensionType dataDimension) const
    {
        float dist = 0.0f;
        for (DimensionType dim = 0; dim < dataDimension; ++dim)
        {
            const float diff = static_cast<float>(vector[dim]) - centroid[dim];
            dist += diff * diff;
        }
        return dist;
    }

    std::vector<std::vector<uint32_t>> ClusterPostingIntoChunks(const std::string &postingListFullData, int recordCount,
                                                                size_t p_spacePerVector,
                                                                DimensionType dataDimension) const
    {
        std::vector<std::vector<uint32_t>> chunkMembers;
        if (recordCount <= 0)
            return chunkMembers;

        const int targetChunkSize = std::max(1, m_opt->m_postingChunkTargetSize);
        const int requestedChunkCount =
            std::max(1, std::min(recordCount, (recordCount + targetChunkSize - 1) / targetChunkSize));
        chunkMembers.resize(requestedChunkCount);
        if (requestedChunkCount == 1)
        {
            chunkMembers[0].reserve(recordCount);
            for (uint32_t i = 0; i < static_cast<uint32_t>(recordCount); ++i)
            {
                chunkMembers[0].push_back(i);
            }
            return chunkMembers;
        }

        auto getVector = [&](uint32_t recordIndex) -> const ValueType * {
            const char *recordPtr = postingListFullData.data() + static_cast<size_t>(recordIndex) * p_spacePerVector;
            return reinterpret_cast<const ValueType *>(recordPtr + sizeof(int));
        };

        std::vector<uint32_t> seedIndices;
        seedIndices.reserve(requestedChunkCount);
        seedIndices.push_back(0);
        std::vector<float> minSeedDistance(recordCount, MaxDist);
        minSeedDistance[0] = 0.0f;

        for (int seedId = 1; seedId < requestedChunkCount; ++seedId)
        {
            uint32_t nextSeed = 0;
            float bestDistance = -1.0f;
            const ValueType *lastSeed = getVector(seedIndices.back());
            for (uint32_t i = 0; i < static_cast<uint32_t>(recordCount); ++i)
            {
                const float dist = COMMON::DistanceUtils::ComputeL2Distance(getVector(i), lastSeed, dataDimension);
                if (dist < minSeedDistance[i])
                {
                    minSeedDistance[i] = dist;
                }
                if (minSeedDistance[i] > bestDistance)
                {
                    bestDistance = minSeedDistance[i];
                    nextSeed = i;
                }
            }
            if (std::find(seedIndices.begin(), seedIndices.end(), nextSeed) != seedIndices.end())
            {
                break;
            }
            seedIndices.push_back(nextSeed);
        }

        const int effectiveChunkCount = static_cast<int>(seedIndices.size());
        std::vector<int> assignments(recordCount, 0);
        std::vector<std::vector<float>> centroids(effectiveChunkCount, std::vector<float>(dataDimension, 0.0f));
        for (int chunkID = 0; chunkID < effectiveChunkCount; ++chunkID)
        {
            const ValueType *seedVector = getVector(seedIndices[chunkID]);
            for (DimensionType dim = 0; dim < dataDimension; ++dim)
            {
                centroids[chunkID][dim] = static_cast<float>(seedVector[dim]);
            }
        }

        for (int iter = 0; iter < 2; ++iter)
        {
            std::vector<std::vector<float>> nextCentroids(effectiveChunkCount, std::vector<float>(dataDimension, 0.0f));
            std::vector<uint32_t> counts(effectiveChunkCount, 0);

            for (int i = 0; i < recordCount; ++i)
            {
                const ValueType *vector = getVector(static_cast<uint32_t>(i));
                float bestDistance = MaxDist;
                int bestChunk = 0;
                for (int chunkID = 0; chunkID < effectiveChunkCount; ++chunkID)
                {
                    const float dist = ComputeL2DistanceToCentroid(vector, centroids[chunkID], dataDimension);
                    if (dist < bestDistance)
                    {
                        bestDistance = dist;
                        bestChunk = chunkID;
                    }
                }
                assignments[i] = bestChunk;
                counts[bestChunk] += 1;
                for (DimensionType dim = 0; dim < dataDimension; ++dim)
                {
                    nextCentroids[bestChunk][dim] += static_cast<float>(vector[dim]);
                }
            }

            for (int chunkID = 0; chunkID < effectiveChunkCount; ++chunkID)
            {
                if (counts[chunkID] == 0)
                    continue;
                const float invCount = 1.0f / counts[chunkID];
                for (DimensionType dim = 0; dim < dataDimension; ++dim)
                {
                    nextCentroids[chunkID][dim] *= invCount;
                }
            }
            centroids.swap(nextCentroids);
        }

        chunkMembers.assign(effectiveChunkCount, {});
        for (int chunkID = 0; chunkID < effectiveChunkCount; ++chunkID)
        {
            chunkMembers[chunkID].reserve(targetChunkSize);
        }
        for (uint32_t i = 0; i < static_cast<uint32_t>(recordCount); ++i)
        {
            chunkMembers[assignments[i]].push_back(i);
        }

        chunkMembers.erase(std::remove_if(chunkMembers.begin(), chunkMembers.end(),
                                          [](const std::vector<uint32_t> &members) { return members.empty(); }),
                           chunkMembers.end());
        return chunkMembers;
    }

    std::vector<uint32_t> BuildPQCodeSortedOrder(const std::string &postingListFullData, int recordCount,
                                                 size_t p_spacePerVector,
                                                 const std::shared_ptr<SPTAG::COMMON::IQuantizer> &quantizer) const
    {
        const size_t quantizeSize = quantizer->QuantizeSize();
        std::vector<std::pair<std::vector<uint8_t>, uint32_t>> codeWithIndex;
        codeWithIndex.reserve(recordCount);

        std::vector<uint8_t> codeBuffer(quantizeSize);
        for (uint32_t i = 0; i < static_cast<uint32_t>(recordCount); ++i)
        {
            const char *legacyRecord = postingListFullData.data() + static_cast<size_t>(i) * p_spacePerVector;
            const char *vectorPtr = legacyRecord + sizeof(int);
            quantizer->QuantizeVector(vectorPtr, codeBuffer.data(), false);
            codeWithIndex.emplace_back(std::vector<uint8_t>(codeBuffer.begin(), codeBuffer.end()), i);
        }

        std::sort(codeWithIndex.begin(), codeWithIndex.end(), [](const auto &a, const auto &b) {
            if (a.first != b.first)
            {
                return a.first < b.first;
            }
            return a.second < b.second;
        });

        std::vector<uint32_t> order;
        order.reserve(recordCount);
        for (const auto &entry : codeWithIndex)
        {
            order.push_back(entry.second);
        }
        return order;
    }

    bool EnsureCohitPayloadOrderLoaded() const
    {
        std::lock_guard<std::mutex> lock(m_cohitOrderMutex);
        if (m_cohitOrderLoaded)
        {
            return m_cohitOrderLoadSucceeded;
        }

        m_cohitOrderLoaded = true;
        m_cohitOrderLoadSucceeded = false;
        m_cohitPayloadOrder.clear();

        if (m_opt == nullptr || m_opt->m_postingCohitOrderFile.empty())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "PostingPayloadLayout=CoHit requires PostingCohitOrderFile.\n");
            return false;
        }

        std::ifstream input(m_opt->m_postingCohitOrderFile.c_str());
        if (!input.is_open())
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to open PostingCohitOrderFile: %s\n",
                         m_opt->m_postingCohitOrderFile.c_str());
            return false;
        }

        std::string line;
        uint64_t rowCount = 0;
        while (std::getline(input, line))
        {
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            for (char &ch : line)
            {
                if (ch == ',' || ch == '\t')
                {
                    ch = ' ';
                }
            }

            std::istringstream iss(line);
            int postingID = -1;
            int vectorID = -1;
            uint32_t rank = 0;
            if (!(iss >> postingID >> vectorID >> rank))
            {
                // Header lines are allowed.
                continue;
            }
            if (postingID < 0 || vectorID < 0)
            {
                continue;
            }

            m_cohitPayloadOrder[postingID][vectorID] = rank;
            ++rowCount;
        }

        m_cohitOrderLoadSucceeded = rowCount > 0;
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Loaded co-hit payload order from %s, rows=%llu, postings=%zu.\n",
                     m_opt->m_postingCohitOrderFile.c_str(), static_cast<unsigned long long>(rowCount),
                     m_cohitPayloadOrder.size());
        return m_cohitOrderLoadSucceeded;
    }

    std::vector<uint32_t> BuildCoHitSortedOrder(int postingID, const std::string &postingListFullData, int recordCount,
                                                size_t p_spacePerVector) const
    {
        std::vector<uint32_t> identityOrder;
        identityOrder.reserve(recordCount);
        for (uint32_t i = 0; i < static_cast<uint32_t>(recordCount); ++i)
        {
            identityOrder.push_back(i);
        }

        if (!EnsureCohitPayloadOrderLoaded())
        {
            return identityOrder;
        }

        auto postingOrderIter = m_cohitPayloadOrder.find(postingID);
        if (postingOrderIter == m_cohitPayloadOrder.end())
        {
            return identityOrder;
        }

        const auto &rankByVector = postingOrderIter->second;
        std::vector<std::pair<uint64_t, uint32_t>> rankWithIndex;
        rankWithIndex.reserve(recordCount);
        for (uint32_t i = 0; i < static_cast<uint32_t>(recordCount); ++i)
        {
            const char *legacyRecord = postingListFullData.data() + static_cast<size_t>(i) * p_spacePerVector;
            const int vectorID = *(reinterpret_cast<const int *>(legacyRecord));
            auto rankIter = rankByVector.find(vectorID);
            const uint64_t rank =
                rankIter == rankByVector.end() ? std::numeric_limits<uint32_t>::max() : rankIter->second;
            rankWithIndex.emplace_back((rank << 32) | i, i);
        }

        std::sort(rankWithIndex.begin(), rankWithIndex.end());
        std::vector<uint32_t> order;
        order.reserve(recordCount);
        for (const auto &entry : rankWithIndex)
        {
            order.push_back(entry.second);
        }
        return order;
    }

    void WriteTwoStageRecordsByOrder(const std::string &postingListFullData, const std::vector<uint32_t> &recordOrder,
                                     size_t p_spacePerVector,
                                     const std::shared_ptr<SPTAG::COMMON::IQuantizer> &quantizer, char *codePtr,
                                     char *payloadPtr, uint32_t codeBytesPerVector,
                                     uint32_t payloadBytesPerVector) const
    {
        for (uint32_t outputIdx = 0; outputIdx < static_cast<uint32_t>(recordOrder.size()); ++outputIdx)
        {
            const uint32_t origIdx = recordOrder[outputIdx];
            const char *legacyRecord = postingListFullData.data() + static_cast<size_t>(origIdx) * p_spacePerVector;
            const int vectorID = *(reinterpret_cast<const int *>(legacyRecord));
            const char *vectorPtr = legacyRecord + sizeof(int);

            NewPostingCodeRecordPrefix prefix;
            prefix.m_vectorID = vectorID;
            prefix.m_payloadOffset = outputIdx * payloadBytesPerVector;
            std::memcpy(codePtr + static_cast<size_t>(outputIdx) * codeBytesPerVector, &prefix,
                        sizeof(NewPostingCodeRecordPrefix));
            quantizer->QuantizeVector(vectorPtr,
                                      reinterpret_cast<uint8_t *>(codePtr +
                                                                  static_cast<size_t>(outputIdx) * codeBytesPerVector +
                                                                  sizeof(NewPostingCodeRecordPrefix)),
                                      false);
            std::memcpy(payloadPtr + static_cast<size_t>(outputIdx) * payloadBytesPerVector, vectorPtr,
                        payloadBytesPerVector);
        }
    }

    std::string BuildSingleChunkTwoStagePostingBlob(int postingID, const std::string &postingListFullData,
                                                    int recordCount, size_t p_spacePerVector,
                                                    const std::shared_ptr<SPTAG::COMMON::IQuantizer> &quantizer,
                                                    PostingPayloadLayoutKind payloadLayout) const
    {
        if (recordCount <= 0)
            return std::string();
        if (quantizer == nullptr)
        {
            throw std::runtime_error("Two-stage posting build requires quantizer");
        }

        const uint32_t codeBytesPerVector =
            static_cast<uint32_t>(sizeof(NewPostingCodeRecordPrefix) + quantizer->QuantizeSize());
        const uint32_t payloadBytesPerVector = static_cast<uint32_t>(p_spacePerVector - sizeof(int));
        NewPostingHeader header;
        header.m_recordCount = static_cast<uint32_t>(recordCount);
        header.m_codeRecordBytes = codeBytesPerVector;
        header.m_codeBytes = codeBytesPerVector * header.m_recordCount;
        header.m_payloadRecordBytes = payloadBytesPerVector;
        header.m_payloadBytes = payloadBytesPerVector * header.m_recordCount;

        NewPostingChunkDirectoryEntry entry;
        entry.m_codeOffset = sizeof(NewPostingHeader) + sizeof(NewPostingChunkDirectoryEntry);
        entry.m_codeBytes = header.m_codeBytes;
        entry.m_payloadOffset = entry.m_codeOffset + entry.m_codeBytes;
        entry.m_payloadBytes = header.m_payloadBytes;
        entry.m_payloadRecordBytes = header.m_payloadRecordBytes;
        entry.m_recordCount = header.m_recordCount;

        std::string postingBlob(sizeof(NewPostingHeader) + sizeof(NewPostingChunkDirectoryEntry) + header.m_codeBytes +
                                    header.m_payloadBytes,
                                '\0');
        std::memcpy(postingBlob.data(), &header, sizeof(NewPostingHeader));
        std::memcpy(postingBlob.data() + sizeof(NewPostingHeader), &entry, sizeof(NewPostingChunkDirectoryEntry));

        char *codePtr = postingBlob.data() + entry.m_codeOffset;
        char *payloadPtr = postingBlob.data() + entry.m_payloadOffset;

        std::vector<uint32_t> recordOrder;
        if (payloadLayout == PostingPayloadLayoutKind::PQCode && recordCount > 1)
        {
            recordOrder = BuildPQCodeSortedOrder(postingListFullData, recordCount, p_spacePerVector, quantizer);
        }
        else if (payloadLayout == PostingPayloadLayoutKind::CoHit && recordCount > 1)
        {
            recordOrder = BuildCoHitSortedOrder(postingID, postingListFullData, recordCount, p_spacePerVector);
        }
        else
        {
            recordOrder.reserve(recordCount);
            for (uint32_t i = 0; i < static_cast<uint32_t>(recordCount); ++i)
            {
                recordOrder.push_back(i);
            }
        }
        WriteTwoStageRecordsByOrder(postingListFullData, recordOrder, p_spacePerVector, quantizer, codePtr, payloadPtr,
                                    codeBytesPerVector, payloadBytesPerVector);

        return postingBlob;
    }

    std::string BuildChunkedPostingBlob(int postingID, const std::string &postingListFullData, int recordCount,
                                        size_t p_spacePerVector,
                                        const std::shared_ptr<SPTAG::COMMON::IQuantizer> &quantizer,
                                        DimensionType dataDimension, PostingPayloadLayoutKind payloadLayout) const
    {
        if (recordCount <= 0)
            return std::string();
        if (quantizer == nullptr)
        {
            throw std::runtime_error("Chunked posting build requires quantizer");
        }

        const uint32_t codeBytesPerVector =
            static_cast<uint32_t>(sizeof(NewPostingCodeRecordPrefix) + quantizer->QuantizeSize());
        const uint32_t payloadBytesPerVector = static_cast<uint32_t>(p_spacePerVector - sizeof(int));
        const uint32_t centroidBytesPerChunk = static_cast<uint32_t>(dataDimension * sizeof(ValueType));
        auto chunkMembers = ClusterPostingIntoChunks(postingListFullData, recordCount, p_spacePerVector, dataDimension);
        if (payloadLayout == PostingPayloadLayoutKind::PQCode)
        {
            auto sortedOrder = BuildPQCodeSortedOrder(postingListFullData, recordCount, p_spacePerVector, quantizer);
            chunkMembers.clear();
            chunkMembers.emplace_back(std::move(sortedOrder));
        }
        else if (payloadLayout == PostingPayloadLayoutKind::CoHit)
        {
            auto sortedOrder = BuildCoHitSortedOrder(postingID, postingListFullData, recordCount, p_spacePerVector);
            chunkMembers.clear();
            chunkMembers.emplace_back(std::move(sortedOrder));
        }
        const uint32_t chunkCount = static_cast<uint32_t>(chunkMembers.size());
        if (chunkCount == 0)
            return std::string();

        std::vector<std::vector<float>> chunkCentroids(chunkCount, std::vector<float>(dataDimension, 0.0f));
        std::vector<float> chunkRadii(chunkCount, 0.0f);
        std::vector<NewPostingChunkDirectoryEntryV2> entries(chunkCount);

        auto getRecordPtr = [&](uint32_t recordIndex) -> const char * {
            return postingListFullData.data() + static_cast<size_t>(recordIndex) * p_spacePerVector;
        };
        auto getVector = [&](uint32_t recordIndex) -> const ValueType * {
            return reinterpret_cast<const ValueType *>(getRecordPtr(recordIndex) + sizeof(int));
        };

        const uint32_t directoryBaseOffset = sizeof(NewPostingHeader);
        const uint32_t centroidBaseOffset = directoryBaseOffset + chunkCount * sizeof(NewPostingChunkDirectoryEntryV2);
        const uint32_t codeBaseOffset = centroidBaseOffset + chunkCount * centroidBytesPerChunk;
        uint32_t runningCodeOffset = codeBaseOffset;
        for (uint32_t chunkID = 0; chunkID < chunkCount; ++chunkID)
        {
            const auto &members = chunkMembers[chunkID];
            for (uint32_t memberIndex : members)
            {
                const ValueType *vector = getVector(memberIndex);
                for (DimensionType dim = 0; dim < dataDimension; ++dim)
                {
                    chunkCentroids[chunkID][dim] += static_cast<float>(vector[dim]);
                }
            }

            const float invCount = 1.0f / static_cast<float>(members.size());
            for (DimensionType dim = 0; dim < dataDimension; ++dim)
            {
                chunkCentroids[chunkID][dim] *= invCount;
            }

            for (uint32_t memberIndex : members)
            {
                chunkRadii[chunkID] =
                    std::max(chunkRadii[chunkID], std::sqrt(ComputeL2DistanceToCentroid(
                                                      getVector(memberIndex), chunkCentroids[chunkID], dataDimension)));
            }

            auto &entry = entries[chunkID];
            entry.m_centroidOffset = centroidBaseOffset + chunkID * centroidBytesPerChunk;
            entry.m_centroidBytes = centroidBytesPerChunk;
            entry.m_radius = chunkRadii[chunkID];
            entry.m_codeOffset = runningCodeOffset;
            entry.m_codeBytes = static_cast<uint32_t>(members.size()) * codeBytesPerVector;
            entry.m_payloadRecordBytes = payloadBytesPerVector;
            entry.m_recordCount = static_cast<uint32_t>(members.size());
            runningCodeOffset += entry.m_codeBytes;
        }

        uint32_t runningPayloadOffset = runningCodeOffset;
        for (uint32_t chunkID = 0; chunkID < chunkCount; ++chunkID)
        {
            auto &entry = entries[chunkID];
            entry.m_payloadOffset = runningPayloadOffset;
            entry.m_payloadBytes = entry.m_recordCount * payloadBytesPerVector;
            runningPayloadOffset += entry.m_payloadBytes;
        }

        NewPostingHeader header;
        header.m_chunkCount = chunkCount;
        header.m_recordCount = static_cast<uint32_t>(recordCount);
        header.m_codeRecordBytes = codeBytesPerVector;
        header.m_codeBytes = runningCodeOffset - codeBaseOffset;
        header.m_payloadRecordBytes = payloadBytesPerVector;
        header.m_payloadBytes = runningPayloadOffset - runningCodeOffset;

        std::string postingBlob(runningPayloadOffset, '\0');
        std::memcpy(postingBlob.data(), &header, sizeof(NewPostingHeader));
        std::memcpy(postingBlob.data() + directoryBaseOffset, entries.data(),
                    chunkCount * sizeof(NewPostingChunkDirectoryEntryV2));

        for (uint32_t chunkID = 0; chunkID < chunkCount; ++chunkID)
        {
            std::vector<ValueType> storedCentroid(dataDimension);
            for (DimensionType dim = 0; dim < dataDimension; ++dim)
            {
                storedCentroid[dim] = static_cast<ValueType>(chunkCentroids[chunkID][dim]);
            }
            std::memcpy(postingBlob.data() + entries[chunkID].m_centroidOffset, storedCentroid.data(),
                        centroidBytesPerChunk);

            char *codeWritePtr = postingBlob.data() + entries[chunkID].m_codeOffset;
            char *payloadWritePtr = postingBlob.data() + entries[chunkID].m_payloadOffset;
            uint32_t localRecordIndex = 0;
            for (uint32_t memberIndex : chunkMembers[chunkID])
            {
                const char *recordPtr = getRecordPtr(memberIndex);
                const int vectorID = *(reinterpret_cast<const int *>(recordPtr));
                const char *vectorPtr = recordPtr + sizeof(int);

                NewPostingCodeRecordPrefix prefix;
                prefix.m_vectorID = vectorID;
                prefix.m_payloadOffset = localRecordIndex * payloadBytesPerVector;
                std::memcpy(codeWritePtr + static_cast<size_t>(localRecordIndex) * codeBytesPerVector, &prefix,
                            sizeof(NewPostingCodeRecordPrefix));
                quantizer->QuantizeVector(
                    vectorPtr,
                    reinterpret_cast<uint8_t *>(codeWritePtr +
                                                static_cast<size_t>(localRecordIndex) * codeBytesPerVector +
                                                sizeof(NewPostingCodeRecordPrefix)),
                    false);
                std::memcpy(payloadWritePtr + static_cast<size_t>(localRecordIndex) * payloadBytesPerVector, vectorPtr,
                            payloadBytesPerVector);
                ++localRecordIndex;
            }
        }

        return postingBlob;
    }

    void SelectPostingOffset(const std::vector<size_t> &p_postingListBytes, std::unique_ptr<int[]> &p_postPageNum,
                             std::unique_ptr<std::uint16_t[]> &p_postPageOffset,
                             std::vector<int> &p_postingOrderInIndex)
    {
        p_postPageNum.reset(new int[p_postingListBytes.size()]);
        p_postPageOffset.reset(new std::uint16_t[p_postingListBytes.size()]);

        struct PageModWithID
        {
            int id;

            std::uint16_t rest;
        };

        struct PageModeWithIDCmp
        {
            bool operator()(const PageModWithID &a, const PageModWithID &b) const
            {
                return a.rest == b.rest ? a.id < b.id : a.rest > b.rest;
            }
        };

        std::set<PageModWithID, PageModeWithIDCmp> listRestSize;

        p_postingOrderInIndex.clear();
        p_postingOrderInIndex.reserve(p_postingListBytes.size());

        PageModWithID listInfo;
        for (size_t i = 0; i < p_postingListBytes.size(); ++i)
        {
            if (p_postingListBytes[i] == 0)
            {
                continue;
            }

            listInfo.id = static_cast<int>(i);
            listInfo.rest = static_cast<std::uint16_t>(p_postingListBytes[i] % PageSize);

            listRestSize.insert(listInfo);
        }

        listInfo.id = -1;

        int currPageNum = 0;
        std::uint16_t currOffset = 0;

        while (!listRestSize.empty())
        {
            listInfo.rest = PageSize - currOffset;
            auto iter = listRestSize.lower_bound(listInfo); // avoid page-crossing
            if (iter == listRestSize.end() || (listInfo.rest != PageSize && iter->rest == 0))
            {
                ++currPageNum;
                currOffset = 0;
            }
            else
            {
                p_postPageNum[iter->id] = currPageNum;
                p_postPageOffset[iter->id] = currOffset;

                p_postingOrderInIndex.push_back(iter->id);

                currOffset += iter->rest;
                if (currOffset > PageSize)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Crossing extra pages\n");
                    throw std::runtime_error("Read too many pages");
                }

                if (currOffset == PageSize)
                {
                    ++currPageNum;
                    currOffset = 0;
                }

                currPageNum += static_cast<int>(p_postingListBytes[iter->id] / PageSize);

                listRestSize.erase(iter);
            }
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "TotalPageNumbers: %d, IndexSize: %llu\n", currPageNum,
                     static_cast<uint64_t>(currPageNum) * PageSize + currOffset);
    }

    void OutputSSDIndexFile(const std::string &p_outputFile, bool p_enableDeltaEncoding,
                            bool p_enablePostingListRearrange, bool p_enableDataCompression, bool p_enableDictTraining,
                            size_t p_spacePerVector, const std::vector<int> &p_postingListSizes,
                            const std::vector<size_t> &p_postingListBytes, std::shared_ptr<VectorIndex> p_headIndex,
                            Selection &p_postingSelections, const std::unique_ptr<int[]> &p_postPageNum,
                            const std::unique_ptr<std::uint16_t[]> &p_postPageOffset,
                            const std::vector<int> &p_postingOrderInIndex, std::shared_ptr<VectorSet> p_fullVectors,
                            size_t p_postingListOffset)
    {
        if (m_opt != nullptr && m_opt->m_ssdPostingFormatVersion >= 2)
        {
            OutputSSDIndexFileTwoStage(p_outputFile, p_enableDeltaEncoding, p_enablePostingListRearrange,
                                       p_enableDataCompression, p_enableDictTraining, p_spacePerVector,
                                       p_postingListSizes, p_postingListBytes, p_headIndex, p_postingSelections,
                                       p_postPageNum, p_postPageOffset, p_postingOrderInIndex, p_fullVectors,
                                       p_postingListOffset);
            return;
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start output...\n");

        auto t1 = std::chrono::high_resolution_clock::now();

        auto ptr = SPTAG::f_createIO();
        int retry = 3;
        // open file
        while (retry > 0 &&
               (ptr == nullptr || !ptr->Initialize(p_outputFile.c_str(), std::ios::binary | std::ios::out)))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed open file %s, retrying...\n", p_outputFile.c_str());
            retry--;
        }

        if (ptr == nullptr || !ptr->Initialize(p_outputFile.c_str(), std::ios::binary | std::ios::out))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed open file %s\n", p_outputFile.c_str());
            throw std::runtime_error("Failed to open file for SSD index save");
        }
        // meta size of global info
        std::uint64_t listOffset = sizeof(int) * 4;
        // meta size of the posting lists
        listOffset +=
            (sizeof(int) + sizeof(std::uint16_t) + sizeof(int) + sizeof(std::uint16_t)) * p_postingListSizes.size();
        // write listTotalBytes only when enabled data compression
        if (p_enableDataCompression)
        {
            listOffset += sizeof(size_t) * p_postingListSizes.size();
        }

        // compression dict
        if (p_enableDataCompression && p_enableDictTraining)
        {
            listOffset += sizeof(size_t);
            listOffset += m_pCompressor->GetDictBuffer().size();
        }

        std::unique_ptr<char[]> paddingVals(new char[PageSize]);
        memset(paddingVals.get(), 0, sizeof(char) * PageSize);
        // paddingSize: bytes left in the last page
        std::uint64_t paddingSize = PageSize - (listOffset % PageSize);
        if (paddingSize == PageSize)
        {
            paddingSize = 0;
        }
        else
        {
            listOffset += paddingSize;
        }

        // Number of posting lists
        int i32Val = static_cast<int>(p_postingListSizes.size());
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
            throw std::runtime_error("Failed to write SSDIndex File");
        }

        // Number of vectors
        i32Val = static_cast<int>(p_fullVectors->Count());
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
            throw std::runtime_error("Failed to write SSDIndex File");
        }

        // Vector dimension
        i32Val = static_cast<int>(p_fullVectors->Dimension());
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
            throw std::runtime_error("Failed to write SSDIndex File");
        }

        // Page offset of list content section
        i32Val = static_cast<int>(listOffset / PageSize);
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
            throw std::runtime_error("Failed to write SSDIndex File");
        }

        // Meta of each posting list
        for (int i = 0; i < p_postingListSizes.size(); ++i)
        {
            size_t postingListByte = 0;
            int pageNum = 0; // starting page number
            std::uint16_t pageOffset = 0;
            int listEleCount = 0;
            std::uint16_t listPageCount = 0;

            if (p_postingListSizes[i] > 0)
            {
                pageNum = p_postPageNum[i];
                pageOffset = static_cast<std::uint16_t>(p_postPageOffset[i]);
                listEleCount = static_cast<int>(p_postingListSizes[i]);
                postingListByte = p_postingListBytes[i];
                listPageCount = static_cast<std::uint16_t>(postingListByte / PageSize);
                if (0 != (postingListByte % PageSize))
                {
                    ++listPageCount;
                }
            }
            // Total bytes of the posting list, write only when enabled data compression
            if (p_enableDataCompression &&
                ptr->WriteBinary(sizeof(postingListByte), reinterpret_cast<char *>(&postingListByte)) !=
                    sizeof(postingListByte))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
            // Page number of the posting list
            if (ptr->WriteBinary(sizeof(pageNum), reinterpret_cast<char *>(&pageNum)) != sizeof(pageNum))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
            // Page offset
            if (ptr->WriteBinary(sizeof(pageOffset), reinterpret_cast<char *>(&pageOffset)) != sizeof(pageOffset))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
            // Number of vectors in the posting list
            if (ptr->WriteBinary(sizeof(listEleCount), reinterpret_cast<char *>(&listEleCount)) != sizeof(listEleCount))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
            // Page count of the posting list
            if (ptr->WriteBinary(sizeof(listPageCount), reinterpret_cast<char *>(&listPageCount)) !=
                sizeof(listPageCount))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
        }
        // compression dict
        if (p_enableDataCompression && p_enableDictTraining)
        {
            std::string dictBuffer = m_pCompressor->GetDictBuffer();
            // dict size
            size_t dictBufferSize = dictBuffer.size();
            if (ptr->WriteBinary(sizeof(size_t), reinterpret_cast<char *>(&dictBufferSize)) != sizeof(dictBufferSize))
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
            // dict
            if (ptr->WriteBinary(dictBuffer.size(), const_cast<char *>(dictBuffer.data())) != dictBuffer.size())
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
        }

        // Write padding vals
        if (paddingSize > 0)
        {
            if (ptr->WriteBinary(paddingSize, reinterpret_cast<char *>(paddingVals.get())) != paddingSize)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
        }

        if (static_cast<uint64_t>(ptr->TellP()) != listOffset)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "List offset not match!\n");
            throw std::runtime_error("List offset mismatch");
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "SubIndex Size: %llu bytes, %llu MBytes\n", listOffset,
                     listOffset >> 20);

        listOffset = 0;

        std::uint64_t paddedSize = 0;
        // iterate over all the posting lists
        for (auto id : p_postingOrderInIndex)
        {
            std::uint64_t targetOffset = static_cast<uint64_t>(p_postPageNum[id]) * PageSize + p_postPageOffset[id];
            if (targetOffset < listOffset)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "List offset not match, targetOffset < listOffset!\n");
                throw std::runtime_error("List offset mismatch");
            }
            // write padding vals before the posting list
            if (targetOffset > listOffset)
            {
                if (targetOffset - listOffset > PageSize)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Padding size greater than page size!\n");
                    throw std::runtime_error("Padding size mismatch with page size");
                }

                if (ptr->WriteBinary(targetOffset - listOffset, reinterpret_cast<char *>(paddingVals.get())) !=
                    targetOffset - listOffset)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                    throw std::runtime_error("Failed to write SSDIndex File");
                }

                paddedSize += targetOffset - listOffset;

                listOffset = targetOffset;
            }

            if (p_postingListSizes[id] == 0)
            {
                continue;
            }
            int postingListId = id + (int)p_postingListOffset;
            // get posting list full content and write it at once
            ValueType *headVector = nullptr;
            if (p_enableDeltaEncoding)
            {
                headVector = (ValueType *)p_headIndex->GetSample(postingListId);
            }
            std::string postingListFullData =
                GetPostingListFullData(postingListId, p_postingListSizes[id], p_postingSelections, p_fullVectors,
                                       p_enableDeltaEncoding, p_enablePostingListRearrange, headVector);
            size_t postingListFullSize = p_postingListSizes[id] * p_spacePerVector;
            if (postingListFullSize != postingListFullData.size())
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                             "posting list full data size NOT MATCH! postingListFullData.size(): %zu "
                             "postingListFullSize: %zu \n",
                             postingListFullData.size(), postingListFullSize);
                throw std::runtime_error("Posting list full size mismatch");
            }
            if (p_enableDataCompression)
            {
                std::string compressedData = m_pCompressor->Compress(postingListFullData, p_enableDictTraining);
                size_t compressedSize = compressedData.size();
                if (compressedSize != p_postingListBytes[id])
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error,
                                 "Compressed size NOT MATCH! compressed size:%zu, pre-calculated compressed size:%zu\n",
                                 compressedSize, p_postingListBytes[id]);
                    throw std::runtime_error("Compression size mismatch");
                }
                if (ptr->WriteBinary(compressedSize, compressedData.data()) != compressedSize)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                    throw std::runtime_error("Failed to write SSDIndex File");
                }
                listOffset += compressedSize;
            }
            else
            {
                if (ptr->WriteBinary(postingListFullSize, postingListFullData.data()) != postingListFullSize)
                {
                    SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                    throw std::runtime_error("Failed to write SSDIndex File");
                }
                listOffset += postingListFullSize;
            }
        }

        paddingSize = PageSize - (listOffset % PageSize);
        if (paddingSize == PageSize)
        {
            paddingSize = 0;
        }
        else
        {
            listOffset += paddingSize;
            paddedSize += paddingSize;
        }

        if (paddingSize > 0)
        {
            if (ptr->WriteBinary(paddingSize, reinterpret_cast<char *>(paddingVals.get())) != paddingSize)
            {
                SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to write SSDIndex File!");
                throw std::runtime_error("Failed to write SSDIndex File");
            }
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Padded Size: %llu, final total size: %llu.\n", paddedSize, listOffset);

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Output done...\n");
        auto t2 = std::chrono::high_resolution_clock::now();
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Time to write results:%.2lf sec.\n",
                     ((double)std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count()) +
                         ((double)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()) / 1000);
    }

    void OutputSSDIndexFileTwoStage(const std::string &p_outputFile, bool p_enableDeltaEncoding,
                                    bool p_enablePostingListRearrange, bool p_enableDataCompression,
                                    bool p_enableDictTraining, size_t p_spacePerVector,
                                    const std::vector<int> &p_postingListSizes,
                                    const std::vector<size_t> &p_postingListBytes,
                                    std::shared_ptr<VectorIndex> p_headIndex, Selection &p_postingSelections,
                                    const std::unique_ptr<int[]> &p_postPageNum,
                                    const std::unique_ptr<std::uint16_t[]> &p_postPageOffset,
                                    const std::vector<int> &p_postingOrderInIndex,
                                    std::shared_ptr<VectorSet> p_fullVectors, size_t p_postingListOffset)
    {
        auto quantizer = GetPostingQuantizer(p_headIndex);
        if (quantizer == nullptr)
        {
            throw std::runtime_error("Two-stage posting build requires quantizer");
        }
        const bool enableChunkedPosting = (m_opt != nullptr && m_opt->m_enableChunkedPosting);
        if (p_enableDeltaEncoding || p_enablePostingListRearrange || p_enableDataCompression)
        {
            throw std::runtime_error("Two-stage posting Phase 1 does not support delta/rearrange/compression");
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Start two-stage output...\n");
        auto t1 = std::chrono::high_resolution_clock::now();

        auto ptr = SPTAG::f_createIO();
        if (ptr == nullptr || !ptr->Initialize(p_outputFile.c_str(), std::ios::binary | std::ios::out))
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed open file %s\n", p_outputFile.c_str());
            throw std::runtime_error("Failed to open file for two-stage SSD index save");
        }

        const uint32_t codeBytesPerVector =
            static_cast<uint32_t>(sizeof(NewPostingCodeRecordPrefix) + quantizer->QuantizeSize());
        const uint32_t payloadBytesPerVector = static_cast<uint32_t>(p_spacePerVector - sizeof(int));
        const size_t listCount = p_postingListSizes.size();
        std::vector<size_t> actualPostingBytes(listCount, 0);

        std::uint64_t listOffset = sizeof(int) * 4;
        listOffset += (sizeof(int) + sizeof(std::uint16_t) + sizeof(int) + sizeof(std::uint16_t)) * listCount;

        std::unique_ptr<char[]> paddingVals(new char[PageSize]);
        memset(paddingVals.get(), 0, sizeof(char) * PageSize);
        std::uint64_t paddingSize = PageSize - (listOffset % PageSize);
        if (paddingSize == PageSize)
        {
            paddingSize = 0;
        }
        else
        {
            listOffset += paddingSize;
        }

        int i32Val = static_cast<int>(listCount);
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            throw std::runtime_error("Failed to write SSDIndex File");
        }
        i32Val = static_cast<int>(p_fullVectors->Count());
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            throw std::runtime_error("Failed to write SSDIndex File");
        }
        i32Val = static_cast<int>(p_fullVectors->Dimension());
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            throw std::runtime_error("Failed to write SSDIndex File");
        }
        i32Val = static_cast<int>(listOffset / PageSize);
        if (ptr->WriteBinary(sizeof(i32Val), reinterpret_cast<char *>(&i32Val)) != sizeof(i32Val))
        {
            throw std::runtime_error("Failed to write SSDIndex File");
        }

        for (size_t i = 0; i < listCount; ++i)
        {
            size_t postingListByte = 0;
            int pageNum = 0;
            std::uint16_t pageOffset = 0;
            int listEleCount = 0;
            std::uint16_t listPageCount = 0;

            if (p_postingListSizes[i] > 0)
            {
                pageNum = p_postPageNum[i];
                pageOffset = static_cast<std::uint16_t>(p_postPageOffset[i]);
                listEleCount = static_cast<int>(p_postingListSizes[i]);
                postingListByte =
                    enableChunkedPosting
                        ? p_postingListBytes[i]
                        : sizeof(NewPostingHeader) + sizeof(NewPostingChunkDirectoryEntry) +
                              static_cast<size_t>(listEleCount) * (codeBytesPerVector + payloadBytesPerVector);
                actualPostingBytes[i] = postingListByte;
                listPageCount = static_cast<std::uint16_t>((postingListByte + pageOffset + PageSize - 1) / PageSize);
            }

            if (ptr->WriteBinary(sizeof(pageNum), reinterpret_cast<char *>(&pageNum)) != sizeof(pageNum) ||
                ptr->WriteBinary(sizeof(pageOffset), reinterpret_cast<char *>(&pageOffset)) != sizeof(pageOffset) ||
                ptr->WriteBinary(sizeof(listEleCount), reinterpret_cast<char *>(&listEleCount)) !=
                    sizeof(listEleCount) ||
                ptr->WriteBinary(sizeof(listPageCount), reinterpret_cast<char *>(&listPageCount)) !=
                    sizeof(listPageCount))
            {
                throw std::runtime_error("Failed to write SSDIndex File");
            }
        }

        if (paddingSize > 0 &&
            ptr->WriteBinary(paddingSize, reinterpret_cast<char *>(paddingVals.get())) != paddingSize)
        {
            throw std::runtime_error("Failed to write SSDIndex File");
        }
        if (static_cast<uint64_t>(ptr->TellP()) != listOffset)
        {
            throw std::runtime_error("List offset mismatch");
        }

        const std::uint64_t postingSectionBaseOffset = listOffset;
        listOffset = postingSectionBaseOffset;
        std::uint64_t paddedSize = 0;
        for (auto id : p_postingOrderInIndex)
        {
            std::uint64_t targetOffset =
                postingSectionBaseOffset + static_cast<uint64_t>(p_postPageNum[id]) * PageSize + p_postPageOffset[id];
            if (targetOffset > listOffset)
            {
                if (ptr->WriteBinary(targetOffset - listOffset, reinterpret_cast<char *>(paddingVals.get())) !=
                    targetOffset - listOffset)
                {
                    throw std::runtime_error("Failed to write SSDIndex File");
                }
                paddedSize += targetOffset - listOffset;
                listOffset = targetOffset;
            }
            if (p_postingListSizes[id] == 0)
            {
                continue;
            }

            int postingListId = id + static_cast<int>(p_postingListOffset);
            ValueType *headVector = nullptr;
            if (p_enableDeltaEncoding)
            {
                headVector = (ValueType *)p_headIndex->GetSample(postingListId);
            }
            std::string postingListFullData =
                GetPostingListFullData(postingListId, p_postingListSizes[id], p_postingSelections, p_fullVectors,
                                       p_enableDeltaEncoding, p_enablePostingListRearrange, headVector);
            size_t postingListFullSize = static_cast<size_t>(p_postingListSizes[id]) * p_spacePerVector;
            if (postingListFullData.size() != postingListFullSize)
            {
                throw std::runtime_error("Posting list full size mismatch");
            }

            std::string postingBlob;
            if (enableChunkedPosting)
            {
                postingBlob = BuildChunkedPostingBlob(postingListId, postingListFullData, p_postingListSizes[id],
                                                      p_spacePerVector, quantizer, p_fullVectors->Dimension(),
                                                      GetPostingPayloadLayoutKind());
            }
            else
            {
                postingBlob =
                    BuildSingleChunkTwoStagePostingBlob(postingListId, postingListFullData, p_postingListSizes[id],
                                                        p_spacePerVector, quantizer, GetPostingPayloadLayoutKind());
            }

            if (postingBlob.size() != actualPostingBytes[id])
            {
                throw std::runtime_error("Two-stage posting byte size mismatch");
            }
            if (ptr->WriteBinary(postingBlob.size(), postingBlob.data()) != postingBlob.size())
            {
                throw std::runtime_error("Failed to write SSDIndex File");
            }
            listOffset += postingBlob.size();
        }

        paddingSize = PageSize - (listOffset % PageSize);
        if (paddingSize == PageSize)
        {
            paddingSize = 0;
        }
        else
        {
            listOffset += paddingSize;
            paddedSize += paddingSize;
        }
        if (paddingSize > 0 &&
            ptr->WriteBinary(paddingSize, reinterpret_cast<char *>(paddingVals.get())) != paddingSize)
        {
            throw std::runtime_error("Failed to write SSDIndex File");
        }

        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Two-stage output done. padded size: %llu final total size: %llu\n",
                     paddedSize, listOffset);
        auto t2 = std::chrono::high_resolution_clock::now();
        SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "Time to write two-stage results: %.2lf sec.\n",
                     ((double)std::chrono::duration_cast<std::chrono::seconds>(t2 - t1).count()) +
                         ((double)std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()) / 1000);
    }

    ErrorCode GetWritePosting(ExtraWorkSpace *p_exWorkSpace, SizeType pid, std::string &posting,
                              bool write = false) override
    {
        if (write)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Unsupport write\n");
            return ErrorCode::Undefined;
        }
        ListInfo *listInfo = &(m_listInfos[pid]);
        size_t totalBytes = (static_cast<size_t>(listInfo->listPageCount) << PageSizeEx);
        size_t realBytes = IsTwoStagePostingFormat() ? listInfo->listTotalBytes
                                                     : static_cast<size_t>(listInfo->listEleCount) * m_vectorInfoSize;
        posting.resize(totalBytes);
        int fileid = m_oneContext ? 0 : pid / m_listPerFile;
        Helper::DiskIO *indexFile = m_indexFiles[fileid].get();
        auto numRead = indexFile->ReadBinary(totalBytes, (char *)posting.data(), listInfo->listOffset);
        if (numRead != totalBytes)
        {
            SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "File %s read bytes, expected: %zu, acutal: %llu.\n",
                         m_extraFullGraphFile.c_str(), totalBytes, numRead);
            return ErrorCode::DiskIOFail;
        }
        char *ptr = (char *)(posting.c_str());
        memcpy(ptr, posting.c_str() + listInfo->pageOffset, realBytes);
        posting.resize(realBytes);
        return ErrorCode::Success;
    }

  private:
    bool m_available = false;

    std::shared_ptr<Helper::Concurrent::ConcurrentQueue<int>> m_freeWorkSpaceIds;
    std::atomic<int> m_workspaceCount = 0;

    std::string m_extraFullGraphFile;
    PostingFormatMetadata m_postingFormatMetadata;

    std::vector<ListInfo> m_listInfos;
    std::vector<PostingRuntimeMetadataCache> m_postingRuntimeMetadata;
    mutable std::mutex m_cohitOrderMutex;
    mutable bool m_cohitOrderLoaded = false;
    mutable bool m_cohitOrderLoadSucceeded = false;
    mutable std::unordered_map<int, std::unordered_map<int, uint32_t>> m_cohitPayloadOrder;
    bool m_oneContext;
    Options *m_opt;

    std::vector<std::shared_ptr<Helper::DiskIO>> m_indexFiles;
    std::shared_ptr<SPTAG::COMMON::IQuantizer> m_twoStageQuantizer;
    std::unique_ptr<Compressor> m_pCompressor;
    bool m_enableDeltaEncoding;
    bool m_enablePostingListRearrange;
    bool m_enableDataCompression;
    bool m_enableDictTraining;

    void (ExtraStaticSearcher<ValueType>::*m_parsePosting)(uint64_t &, uint64_t &, int, int);
    void (ExtraStaticSearcher<ValueType>::*m_parseEncoding)(std::shared_ptr<VectorIndex> &, ListInfo *, ValueType *);

    int m_vectorInfoSize = 0;
    int m_iDataDimension = 0;

    int m_totalListCount = 0;

    int m_listPerFile = 0;
};
} // namespace SPANN
} // namespace SPTAG

#endif // _SPTAG_SPANN_EXTRASTATICSEARCHER_H_
