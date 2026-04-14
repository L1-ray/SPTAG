// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// SPTAG 在线模式性能测试程序
// 在同一进程中完成索引构建和搜索，无需保存到磁盘

#include "inc/Core/Common.h"
#include "inc/Core/VectorIndex.h"
#include "inc/Core/Common/TruthSet.h"
#include "inc/Helper/SimpleIniReader.h"
#include "inc/Helper/VectorSetReader.h"
#include "inc/Helper/StringConvert.h"

#include <chrono>
#include <iomanip>
#include <atomic>
#include <thread>

using namespace SPTAG;

class OnlineTestOptions : public Helper::ReaderOptions
{
  public:
    OnlineTestOptions() : Helper::ReaderOptions(VectorValueType::Float, 0, VectorFileType::XVEC, "|", 32)
    {
        AddRequiredOption(m_baseFile, "-b", "--base", "Base vector file.");
        AddRequiredOption(m_queryFile, "-q", "--query", "Query vector file.");
        AddOptionalOption(m_truthFile, "-t", "--truth", "Ground truth file.");
        AddOptionalOption(m_outputFolder, "-o", "--output", "Output folder (optional, save index).");
        AddOptionalOption(m_algoStr, "-a", "--algo", "Index Algorithm type.");
        AddOptionalOption(m_maxCheck, "-m", "--maxcheck", "MaxCheck for search.");
        AddOptionalOption(m_k, "-k", "--knn", "Number of nearest neighbors.");
        AddOptionalOption(m_truthK, "-tk", "--truthknn", "Truth K for recall calculation.");
    }

    ~OnlineTestOptions() {}

    std::string m_baseFile;
    std::string m_queryFile;
    std::string m_truthFile;
    std::string m_outputFolder;
    std::string m_maxCheck = "8192";
    std::string m_algoStr = "BKT";
    int m_k = 32;
    int m_truthK = -1;
    SPTAG::IndexAlgoType m_indexAlgoType = SPTAG::IndexAlgoType::BKT;
};

// 在 Parse 之后初始化 m_indexAlgoType
void InitAlgoType(std::shared_ptr<OnlineTestOptions> options)
{
    SPTAG::Helper::Convert::ConvertStringTo<SPTAG::IndexAlgoType>(options->m_algoStr.c_str(), options->m_indexAlgoType);
}

// 加载 ground truth（ivecs 格式）
std::vector<std::vector<int>> LoadGroundTruth(const std::string& truthFile, int numQueries)
{
    std::vector<std::vector<int>> truth(numQueries);

    auto fp = SPTAG::f_createIO();
    if (fp == nullptr || !fp->Initialize(truthFile.c_str(), std::ios::in | std::ios::binary))
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Cannot open truth file: %s\n", truthFile.c_str());
        return truth;
    }

    for (int i = 0; i < numQueries; i++)
    {
        int dim;
        if (fp->ReadBinary(sizeof(int), (char*)&dim) != sizeof(int))
            break;

        truth[i].resize(dim);
        fp->ReadBinary(dim * sizeof(int), (char*)truth[i].data());
    }

    return truth;
}

// 计算 Recall@K
float CalculateRecall(const std::vector<QueryResult>& results,
                      const std::vector<std::vector<int>>& truth,
                      int k)
{
    int correct = 0;
    int total = 0;

    for (size_t i = 0; i < results.size(); i++)
    {
        std::set<int> truthSet(truth[i].begin(), truth[i].begin() + std::min(k, (int)truth[i].size()));
        for (int j = 0; j < std::min(k, results[i].GetResultNum()); j++)
        {
            if (truthSet.count(results[i].GetResult(j)->VID))
                correct++;
        }
        total += k;
    }

    return (float)correct / total;
}

int main(int argc, char* argv[])
{
    // 设置无缓冲输出，确保监控脚本可以实时检测阶段变化
    std::cout.setf(std::ios::unitbuf);

    std::shared_ptr<OnlineTestOptions> options(new OnlineTestOptions);
    if (!options->Parse(argc - 1, argv + 1))
    {
        exit(1);
    }
    InitAlgoType(options);

    std::cout << "==================================================" << std::endl;
    std::cout << "SPTAG 在线模式性能测试 (C++ 版本)" << std::endl;
    std::cout << "==================================================" << std::endl;

    // 1. 加载基础向量
    std::cout << "\n[1] 加载数据..." << std::endl;
    std::cout << "    基础向量: " << options->m_baseFile << std::endl;
    std::cout << "    查询向量: " << options->m_queryFile << std::endl;

    auto startLoad = std::chrono::high_resolution_clock::now();

    // 加载基础向量
    auto baseReader = Helper::VectorSetReader::CreateInstance(options);
    if (ErrorCode::Success != baseReader->LoadFile(options->m_baseFile))
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read base file.\n");
        exit(1);
    }
    auto baseVectors = baseReader->GetVectorSet();

    // 加载查询向量
    auto queryReader = Helper::VectorSetReader::CreateInstance(options);
    if (ErrorCode::Success != queryReader->LoadFile(options->m_queryFile))
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to read query file.\n");
        exit(1);
    }
    auto queryVectors = queryReader->GetVectorSet();

    auto endLoad = std::chrono::high_resolution_clock::now();
    double loadTime = std::chrono::duration<double>(endLoad - startLoad).count();

    std::cout << "    基础向量: (" << baseVectors->Count() << ", " << baseVectors->Dimension() << ")" << std::endl;
    std::cout << "    查询向量: (" << queryVectors->Count() << ", " << queryVectors->Dimension() << ")" << std::endl;
    std::cout << "    加载耗时: " << std::fixed << std::setprecision(2) << loadTime << "s" << std::endl;

    // 加载 ground truth（如果提供）
    std::vector<std::vector<int>> groundTruth;
    if (!options->m_truthFile.empty())
    {
        groundTruth = LoadGroundTruth(options->m_truthFile, queryVectors->Count());
        std::cout << "    Ground truth: " << options->m_truthFile << std::endl;
    }

    // 2. 构建索引（在线模式核心）
    std::cout << "\n[2] 构建索引（在线模式，" << options->m_threadNum << " 线程）..." << std::endl;

    auto startBuild = std::chrono::high_resolution_clock::now();

    auto index = VectorIndex::CreateInstance(options->m_indexAlgoType, options->m_inputValueType);
    if (index == nullptr)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to create index.\n");
        exit(1);
    }

    // 设置构建参数
    index->SetParameter("DistCalcMethod", "L2", "Index");
    index->SetParameter("NumberOfThreads", std::to_string(options->m_threadNum), "Index");
    index->SetParameter("NeighborhoodSize", "32", "Index");
    index->SetParameter("TPTNumber", "8", "Index");
    index->SetParameter("RefineIterations", "1", "Index");

    // 构建索引
    ErrorCode ret = index->BuildIndex(baseVectors->GetData(), baseVectors->Count(),
                                       baseVectors->Dimension(), false, true);
    if (ret != ErrorCode::Success)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "Failed to build index.\n");
        exit(1);
    }

    auto endBuild = std::chrono::high_resolution_clock::now();
    double buildTime = std::chrono::duration<double>(endBuild - startBuild).count();

    std::cout << "    构建耗时: " << std::fixed << std::setprecision(2) << buildTime << "s"
              << " (" << buildTime / 60 << "分钟)" << std::endl;
    std::cout << "    构建速度: " << (int)(baseVectors->Count() / buildTime) << " vectors/s" << std::endl;

    // 3. 设置搜索参数
    std::cout << "\n[3] 设置搜索参数 (MaxCheck=" << options->m_maxCheck << ")..." << std::endl;
    index->SetParameter("MaxCheck", options->m_maxCheck.c_str(), "Index");

    // 4. 搜索测试
    std::cout << "\n[4] 搜索测试 (K=" << options->m_k << ")..." << std::endl;
    std::cout << std::flush;  // 强制刷新输出缓冲区，确保监控脚本可以检测到阶段变化

    int numQueries = queryVectors->Count();
    int k = options->m_k;
    int truthK = (options->m_truthK < 0) ? k : options->m_truthK;

    std::vector<QueryResult> results(numQueries, QueryResult(nullptr, k, false));
    std::vector<float> latencies(numQueries);

    // 设置查询目标
    for (int i = 0; i < numQueries; i++)
    {
        results[i].SetTarget(queryVectors->GetVector(i));
    }

    // 多线程搜索
    std::atomic_size_t queriesSent(0);
    std::vector<std::thread> threads;
    threads.reserve(options->m_threadNum);

    auto startSearch = std::chrono::high_resolution_clock::now();

    for (int tid = 0; tid < options->m_threadNum; tid++)
    {
        threads.emplace_back([&, tid]() {
            size_t qid = 0;
            while (true)
            {
                qid = queriesSent.fetch_add(1);
                if (qid < numQueries)
                {
                    auto t1 = std::chrono::high_resolution_clock::now();
                    index->SearchIndex(results[qid]);
                    auto t2 = std::chrono::high_resolution_clock::now();
                    latencies[qid] = std::chrono::duration<float>(t2 - t1).count();
                }
                else
                {
                    return;
                }
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    auto endSearch = std::chrono::high_resolution_clock::now();
    double searchTime = std::chrono::duration<double>(endSearch - startSearch).count();
    double qps = numQueries / searchTime;

    // 计算延迟统计
    float avgLatency = 0, minLatency = 1e9, maxLatency = 0;
    for (int i = 0; i < numQueries; i++)
    {
        avgLatency += latencies[i];
        if (latencies[i] < minLatency) minLatency = latencies[i];
        if (latencies[i] > maxLatency) maxLatency = latencies[i];
    }
    avgLatency /= numQueries;

    std::sort(latencies.begin(), latencies.end());
    float p95Latency = latencies[(int)(numQueries * 0.95)];
    float p99Latency = latencies[(int)(numQueries * 0.99)];

    // 计算 Recall
    float recall = 0;
    if (!groundTruth.empty())
    {
        recall = CalculateRecall(results, groundTruth, truthK);
    }

    std::cout << "\n[搜索结果]" << std::endl;
    std::cout << "    总查询数: " << numQueries << std::endl;
    std::cout << "    搜索耗时: " << std::fixed << std::setprecision(3) << searchTime << "s" << std::endl;
    std::cout << "    QPS: " << std::fixed << std::setprecision(2) << qps << std::endl;
    std::cout << "    平均延迟: " << std::fixed << std::setprecision(4) << avgLatency * 1000 << "ms" << std::endl;
    std::cout << "    P95 延迟: " << p95Latency * 1000 << "ms" << std::endl;
    std::cout << "    P99 延迟: " << p99Latency * 1000 << "ms" << std::endl;
    if (!groundTruth.empty())
    {
        std::cout << "    Recall@" << truthK << ": " << std::fixed << std::setprecision(2) << recall * 100 << "%" << std::endl;
    }

    // 5. 可选：保存索引
    if (!options->m_outputFolder.empty())
    {
        std::cout << "\n[5] 保存索引到: " << options->m_outputFolder << std::endl;
        index->SaveIndex(options->m_outputFolder);
        std::cout << "    保存完成" << std::endl;
    }

    // 6. 总结
    std::cout << "\n==================================================" << std::endl;
    std::cout << "测试总结" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "模式: 在线模式 (C++)" << std::endl;
    std::cout << "基础向量: " << baseVectors->Count() << std::endl;
    std::cout << "查询向量: " << queryVectors->Count() << std::endl;
    std::cout << "构建线程: " << options->m_threadNum << std::endl;
    std::cout << "搜索线程: " << options->m_threadNum << std::endl;
    std::cout << "构建耗时: " << std::fixed << std::setprecision(2) << buildTime << "s" << std::endl;
    std::cout << "搜索QPS: " << std::fixed << std::setprecision(2) << qps << std::endl;
    if (!groundTruth.empty())
    {
        std::cout << "Recall@" << truthK << ": " << recall * 100 << "%" << std::endl;
    }
    std::cout << "==================================================" << std::endl;

    return 0;
}
