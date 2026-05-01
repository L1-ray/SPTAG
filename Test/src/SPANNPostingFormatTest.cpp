// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Core/Common/PQQuantizer.h"
#include "inc/Core/SPANN/Index.h"
#include "inc/Core/VectorIndex.h"
#include "inc/Quantizer/Training.h"
#include "inc/Test.h"

#include <cstring>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace SPTAG;

struct ScopedDirCleanup
{
    explicit ScopedDirCleanup(std::filesystem::path path) : m_path(std::move(path))
    {
    }

    ~ScopedDirCleanup()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    std::filesystem::path m_path;
};

struct SearchSnapshot
{
    std::vector<SizeType> m_vids;
    uint64_t m_coarseCandidateHash = 0;
    uint64_t m_payloadPageHash = 0;
    uint64_t m_finalResultHash = 0;
    uint64_t m_metadataBytesRead = 0;
    uint64_t m_codePhysicalBytesRead = 0;
};

std::shared_ptr<VectorSet> BuildVectorSet(SizeType count, DimensionType dim)
{
    ByteArray bytes = ByteArray::Alloc(sizeof(float) * static_cast<size_t>(count) * dim);
    auto *data = reinterpret_cast<float *>(bytes.Data());
    for (SizeType i = 0; i < count; ++i)
    {
        const float base = (i < (count / 2)) ? 0.0f : 100.0f;
        for (DimensionType d = 0; d < dim; ++d)
        {
            data[static_cast<size_t>(i) * dim + d] = base + static_cast<float>((i % 7) * 0.1f + d * 0.01f);
        }
    }

    return std::make_shared<BasicVectorSet>(bytes, VectorValueType::Float, dim, count);
}

std::shared_ptr<MetadataSet> BuildMetadataSet(SizeType count)
{
    std::vector<std::uint8_t> metaBuffer;
    std::vector<std::uint64_t> offsetBuffer;
    offsetBuffer.reserve(static_cast<size_t>(count) + 1);
    for (SizeType i = 0; i < count; ++i)
    {
        offsetBuffer.push_back(static_cast<std::uint64_t>(metaBuffer.size()));
        const std::string label = std::to_string(i);
        metaBuffer.insert(metaBuffer.end(), label.begin(), label.end());
    }
    offsetBuffer.push_back(static_cast<std::uint64_t>(metaBuffer.size()));

    ByteArray meta = ByteArray::Alloc(metaBuffer.size());
    if (!metaBuffer.empty())
    {
        std::memcpy(meta.Data(), metaBuffer.data(), metaBuffer.size());
    }
    ByteArray offsets = ByteArray::Alloc(offsetBuffer.size() * sizeof(std::uint64_t));
    if (!offsetBuffer.empty())
    {
        std::memcpy(offsets.Data(), offsetBuffer.data(), offsetBuffer.size() * sizeof(std::uint64_t));
    }

    return std::make_shared<MemMetadataSet>(meta, offsets, count);
}

std::shared_ptr<SPTAG::COMMON::IQuantizer> BuildPQQuantizer(const std::shared_ptr<VectorSet> &trainVectors,
                                                            const std::filesystem::path &quantizerPath)
{
    constexpr DimensionType quantizedDim = 2;
    auto options = std::make_shared<QuantizerOptions>(trainVectors->Count(), false, 0.0f, QuantizerType::PQQuantizer,
                                                      DistCalcMethod::L2, quantizerPath.string(), quantizedDim, "", "");
    options->m_dimension = trainVectors->Dimension();
    options->m_threadNum = 2;
    options->m_inputValueType = VectorValueType::Float;
    options->m_trainingSamples = trainVectors->Count();

    ByteArray pqBytes =
        ByteArray::Alloc(sizeof(std::uint8_t) * static_cast<size_t>(quantizedDim) * trainVectors->Count());
    auto pqVectors =
        std::make_shared<BasicVectorSet>(pqBytes, VectorValueType::UInt8, quantizedDim, trainVectors->Count());

    auto codebooks = TrainPQQuantizer<float>(options, trainVectors, pqVectors);
    BOOST_REQUIRE(codebooks != nullptr);

    auto quantizer = std::make_shared<COMMON::PQQuantizer<float>>(
        quantizedDim, 256, static_cast<DimensionType>(trainVectors->Dimension() / quantizedDim), false,
        std::move(codebooks), DistCalcMethod::L2);

    auto fp = SPTAG::f_createIO();
    BOOST_REQUIRE(fp != nullptr);
    BOOST_REQUIRE(fp->Initialize(quantizerPath.string().c_str(), std::ios::binary | std::ios::out));
    BOOST_REQUIRE(quantizer->SaveQuantizer(fp) == ErrorCode::Success);
    return quantizer;
}

void ConfigureChunkedTwoStageIndex(SPTAG::VectorIndex *index, const std::filesystem::path &indexDir,
                                   const std::filesystem::path &quantizerPath)
{
    index->SetParameter("IndexAlgoType", "BKT", "Base");
    index->SetParameter("DistCalcMethod", "L2", "Base");
    index->SetParameter("ValueType", "Float", "Base");
    index->SetParameter("Dim", "8", "Base");
    index->SetParameter("IndexDirectory", indexDir.string().c_str(), "Base");
    index->SetParameter("QuantizerFilePath", quantizerPath.string().c_str(), "Base");

    index->SetParameter("isExecute", "true", "SelectHead");
    index->SetParameter("NumberOfThreads", "2", "SelectHead");
    index->SetParameter("Ratio", "0.05", "SelectHead");

    index->SetParameter("isExecute", "true", "BuildHead");
    index->SetParameter("NumberOfThreads", "2", "BuildHead");
    index->SetParameter("RefineIterations", "2", "BuildHead");

    index->SetParameter("isExecute", "true", "BuildSSDIndex");
    index->SetParameter("BuildSsdIndex", "true", "BuildSSDIndex");
    index->SetParameter("Storage", "Static", "BuildSSDIndex");
    index->SetParameter("NumberOfThreads", "2", "BuildSSDIndex");
    index->SetParameter("InternalResultNum", "32", "BuildSSDIndex");
    index->SetParameter("SearchInternalResultNum", "32", "BuildSSDIndex");
    index->SetParameter("PostingPageLimit", "12", "BuildSSDIndex");
    index->SetParameter("SearchPostingPageLimit", "12", "BuildSSDIndex");
    index->SetParameter("SSDPostingFormatVersion", "2", "BuildSSDIndex");
    index->SetParameter("EnableTwoStagePosting", "true", "BuildSSDIndex");
    index->SetParameter("EnableChunkedPosting", "true", "BuildSSDIndex");
    index->SetParameter("PostingCodeType", "PQ", "BuildSSDIndex");
    index->SetParameter("PostingTopRPerPosting", "8", "BuildSSDIndex");
    index->SetParameter("PostingTopRGlobal", "32", "BuildSSDIndex");
    index->SetParameter("PostingChunkTargetSize", "2", "BuildSSDIndex");
    index->SetParameter("PostingChunkPruneMode", "L2", "BuildSSDIndex");
    index->SetParameter("PostingPayloadBatchPages", "4", "BuildSSDIndex");
    index->SetParameter("EnableDeltaEncoding", "false", "BuildSSDIndex");
    index->SetParameter("EnablePostingListRearrange", "false", "BuildSSDIndex");
    index->SetParameter("EnableDataCompression", "false", "BuildSSDIndex");
}

std::shared_ptr<SPTAG::SPANN::Index<float>>
BuildAndLoadChunkedTwoStageIndex(const std::filesystem::path &root, std::shared_ptr<VectorSet> &vectors)
{
    const auto indexDir = root / "index";
    const auto quantizerPath = root / "quantizer.bin";
    std::filesystem::create_directories(indexDir);

    vectors = BuildVectorSet(256, 8);
    auto metadata = BuildMetadataSet(vectors->Count());
    auto quantizer = BuildPQQuantizer(vectors, quantizerPath);

    auto index = SPTAG::VectorIndex::CreateInstance(SPTAG::IndexAlgoType::SPANN, SPTAG::VectorValueType::Float);
    BOOST_REQUIRE(index != nullptr);
    ConfigureChunkedTwoStageIndex(index.get(), indexDir, quantizerPath);
    index->SetQuantizer(quantizer);

    BOOST_REQUIRE(index->BuildIndex(vectors, metadata, true) == SPTAG::ErrorCode::Success);
    BOOST_REQUIRE(index->SaveIndex(indexDir.string()) == SPTAG::ErrorCode::Success);

    std::shared_ptr<SPTAG::VectorIndex> loaded;
    BOOST_REQUIRE(SPTAG::VectorIndex::LoadIndex(indexDir.string(), loaded) == SPTAG::ErrorCode::Success);
    BOOST_REQUIRE(loaded != nullptr);

    auto *spann = dynamic_cast<SPTAG::SPANN::Index<float> *>(loaded.get());
    BOOST_REQUIRE(spann != nullptr);
    BOOST_REQUIRE(spann->GetMemoryIndex() != nullptr);

    return std::static_pointer_cast<SPTAG::SPANN::Index<float>>(loaded);
}

SearchSnapshot RunChunkedTwoStageQuery(SPTAG::SPANN::Index<float> *spann, const float *target, int resultNum)
{
    SPTAG::QueryResult query(target, resultNum, true);
    BOOST_REQUIRE(spann->GetMemoryIndex()->SearchIndex(query) == SPTAG::ErrorCode::Success);

    SPTAG::SPANN::SearchStats stats;
    BOOST_REQUIRE(spann->SearchDiskIndex(query, &stats) == SPTAG::ErrorCode::Success);

    SearchSnapshot snapshot;
    snapshot.m_vids.reserve(static_cast<size_t>(resultNum));
    for (int i = 0; i < resultNum; ++i)
    {
        snapshot.m_vids.push_back(query.GetResult(i)->VID);
    }
    snapshot.m_coarseCandidateHash = stats.m_coarseCandidateHash;
    snapshot.m_payloadPageHash = stats.m_payloadPageHash;
    snapshot.m_finalResultHash = stats.m_finalResultHash;
    snapshot.m_metadataBytesRead = stats.m_metadataBytesRead;
    snapshot.m_codePhysicalBytesRead = stats.m_codePhysicalBytesRead;
    return snapshot;
}
} // namespace

BOOST_AUTO_TEST_SUITE(SPANNPostingFormatTest)

BOOST_AUTO_TEST_CASE(ChunkedTwoStageBuildLoadSearch)
{
    const auto root = std::filesystem::temp_directory_path() / "spann_chunked_twostage_smoke";
    ScopedDirCleanup cleanup(root);
    std::filesystem::create_directories(root);

    std::shared_ptr<VectorSet> vectors;
    auto spann = BuildAndLoadChunkedTwoStageIndex(root, vectors);
    const auto indexDir = root / "index";

    const auto metaFile = indexDir / "SPTAGFullList.bin.meta";
    std::ifstream metaIn(metaFile);
    BOOST_REQUIRE(metaIn.good());
    const std::string metaContent((std::istreambuf_iterator<char>(metaIn)), std::istreambuf_iterator<char>());
    BOOST_CHECK(metaContent.find("LayoutType=chunked_twostage_v1") != std::string::npos);
    BOOST_CHECK(metaContent.find("ChunkPruneMode=HeuristicL2") != std::string::npos);

    SPTAG::QueryResult query(vectors->GetVector(0), 8, true);
    BOOST_REQUIRE(spann->GetMemoryIndex()->SearchIndex(query) == SPTAG::ErrorCode::Success);

    SPTAG::SPANN::SearchStats stats;
    BOOST_REQUIRE(spann->SearchDiskIndex(query, &stats) == SPTAG::ErrorCode::Success);
    BOOST_CHECK_EQUAL(stats.m_metadataBytesRead, 0U);
    BOOST_CHECK_GT(stats.m_codeBytesRead, 0U);
    BOOST_CHECK_EQUAL(stats.m_codePhysicalBytesRead, 0U);
    BOOST_CHECK_GT(stats.m_payloadBytesRead, 0U);
    BOOST_CHECK_GT(stats.m_coarseCandidateCount, 0U);
    BOOST_CHECK_GT(stats.m_coarseCandidateCountAfterDedupe, 0U);
    BOOST_CHECK_GT(stats.m_rerankCandidateCount, 0U);
    BOOST_CHECK_GT(stats.m_chunksConsidered, 0U);
    BOOST_CHECK_GT(stats.m_chunksScanned, 0U);
    BOOST_CHECK_GE(stats.m_chunksConsidered, stats.m_chunksScanned);
    BOOST_CHECK_GE(stats.m_chunksConsidered, stats.m_postingsTouched);
    BOOST_CHECK_EQUAL(stats.m_chunksConsidered, stats.m_chunksScanned + stats.m_chunksPruned);
    BOOST_CHECK_NE(query.GetResult(0)->VID, -1);
}

BOOST_AUTO_TEST_CASE(ChunkedTwoStageConcurrentSearchDeterministic)
{
    const auto root = std::filesystem::temp_directory_path() / "spann_chunked_twostage_concurrent";
    ScopedDirCleanup cleanup(root);
    std::filesystem::create_directories(root);

    std::shared_ptr<VectorSet> vectors;
    auto spann = BuildAndLoadChunkedTwoStageIndex(root, vectors);

    constexpr int resultNum = 8;
    constexpr int queryCount = 32;
    constexpr int workerCount = 8;
    constexpr int rounds = 4;

    std::vector<SearchSnapshot> baseline(static_cast<size_t>(queryCount));
    for (int i = 0; i < queryCount; ++i)
    {
        baseline[static_cast<size_t>(i)] =
            RunChunkedTwoStageQuery(spann.get(), reinterpret_cast<const float *>(vectors->GetVector(i)), resultNum);
        BOOST_CHECK_EQUAL(baseline[static_cast<size_t>(i)].m_metadataBytesRead, 0U);
        BOOST_CHECK_EQUAL(baseline[static_cast<size_t>(i)].m_codePhysicalBytesRead, 0U);
    }

    std::atomic<int> nextTask(0);
    std::mutex errorLock;
    std::vector<std::string> errors;
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (int worker = 0; worker < workerCount; ++worker)
    {
        workers.emplace_back([&, worker]() {
            while (true)
            {
                const int taskId = nextTask.fetch_add(1);
                if (taskId >= queryCount * rounds)
                {
                    return;
                }

                const int queryId = taskId % queryCount;
                const auto actual =
                    RunChunkedTwoStageQuery(spann.get(), reinterpret_cast<const float *>(vectors->GetVector(queryId)),
                                            resultNum);
                const auto &expected = baseline[static_cast<size_t>(queryId)];

                if (actual.m_vids != expected.m_vids || actual.m_coarseCandidateHash != expected.m_coarseCandidateHash ||
                    actual.m_payloadPageHash != expected.m_payloadPageHash ||
                    actual.m_finalResultHash != expected.m_finalResultHash ||
                    actual.m_metadataBytesRead != expected.m_metadataBytesRead ||
                    actual.m_codePhysicalBytesRead != expected.m_codePhysicalBytesRead)
                {
                    std::lock_guard<std::mutex> guard(errorLock);
                    if (errors.size() < 16)
                    {
                        errors.emplace_back("worker=" + std::to_string(worker) + " query=" + std::to_string(queryId) +
                                            " coarse=" + std::to_string(actual.m_coarseCandidateHash) + "/" +
                                            std::to_string(expected.m_coarseCandidateHash) + " payload=" +
                                            std::to_string(actual.m_payloadPageHash) + "/" +
                                            std::to_string(expected.m_payloadPageHash) + " final=" +
                                            std::to_string(actual.m_finalResultHash) + "/" +
                                            std::to_string(expected.m_finalResultHash));
                    }
                }
            }
        });
    }

    for (auto &worker : workers)
    {
        worker.join();
    }

    const std::string errorMessage =
        errors.empty() ? std::string() : ("Concurrent deterministic search mismatch: " + errors.front());
    BOOST_REQUIRE_MESSAGE(errors.empty(), errorMessage);
}

BOOST_AUTO_TEST_SUITE_END()
