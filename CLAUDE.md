# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

SPTAG (Space Partition Tree And Graph) 是由 Microsoft Research 和 Microsoft Bing 开发的大规模向量近似最近邻 (ANN) 搜索 C++ 库。包含用于增量就地更新的 SPFresh 系统 (SOSP 2023)。

## 构建命令

### Linux
```bash
mkdir build
cd build && cmake -DSPDK=OFF -DROCKSDB=OFF .. && make
```
编译产物输出到 `Release/` 目录。

### Windows
```bash
mkdir build
cd build && cmake -A x64 -DSPDK=OFF -DROCKSDB=OFF ..
```
然后在 Visual Studio 2019+ 中编译 SPTAGLib.sln。

### 构建选项 (CMake)
- `GPU` - 启用 GPU 支持 (默认 OFF)
- `ROCKSDB` - 启用 RocksDB 存储后端 (OFF)
- `SPDK` - 启用 SPDK 存储后端 (OFF)
- `TBB` - 启用 Intel TBB (默认 ON)
- `URING` - 启用 liburing/io_uring 支持 (OFF)
- `USE_ASAN` - 启用 AddressSanitizer (OFF)

### 依赖要求
- CMake >= 3.12
- GCC >= 5.0 (Linux) 或 MSVC 14+ (Windows)
- Boost >= 1.67
- SWIG >= 4.0.2 (用于 Python 绑定)
- OpenMP (必需)

## 测试

测试框架: Boost.Test

```bash
# 运行所有测试
./Release/SPTAGTest

# 运行特定测试套件
./Release/SPTAGTest --run_test=TestSuiteName

# 详细输出模式
./Release/SPTAGTest --log_level=test_suite
```

测试文件位于 `Test/src/` (AlgoTest.cpp, DistanceTest.cpp, SIMDTest.cpp, SPFreshTest.cpp 等)

## 代码格式化

使用 clang-format，基于 Microsoft 风格：
- 列宽限制: 120
- 缩进宽度: 4
- C++17 标准
- 自定义花括号换行 (函数、类、命名空间后换行)

```bash
clang-format -i <file>
```

## 架构

### 索引算法
- **BKT** (Balanced K-Means Tree) - 高维数据精度更高
- **KDT** (KD-Tree) - 构建成本更低
- **SPANN** - 面向十亿级向量的磁盘索引

### 核心组件 (`AnnService/inc/Core/`)
- `BKT/` - BKT 索引实现
- `KDT/` - KDT 索引实现
- `SPANN/` - SPANN 磁盘索引，用于十亿级向量
  - `PageCache.h/cpp` - M1 全局页面缓存实现
  - `ExtraStaticSearcher.h` - Legacy posting 搜索路径
  - `IExtraSearcher.h` - 搜索接口定义
  - `Options.h` - SPANN 配置参数
- `Common/` - 公共工具: SIMD (AVX/AVX2/AVX512)、距离计算 (L2, Cosine)、量化 (PQ/OPQ)

### 可执行程序 (`AnnService/src/`)
- `IndexBuilder/` - 构建内存索引
- `IndexSearcher/` - 搜索内存索引
- `Server/` - 基于 Socket 的搜索服务
- `Client/` - 远程搜索客户端
- `Aggregator/` - 分布式搜索聚合器
- `SSDServing/` - SSD 索引构建与搜索
- `SPFresh/` - 增量就地更新系统
- `Quantizer/` - 训练 PQ/OPQ 量化器

### 关键数据结构
- `VectorSet` - 向量存储
- `MetadataSet` - 向量关联的元数据
- `NeighborhoodGraph` - ANN 搜索的图结构
- `WorkSpace` - 搜索工作空间管理
- `GlobalPageCache` - M1 全局页面缓存

### 支持的距离度量
- L2 (欧氏距离)
- Cosine (余弦距离)

### 支持的向量类型
- Float, Int8, Int16, UInt8

### 文件格式
- DEFAULT (二进制): `<向量数><维度><原始数据>`
- TXT: `<元数据>\t<v1>|<v2>|...|`
- XVEC: .fvecs/.ivecs 格式

## Python 绑定

Python 封装通过 SWIG 构建。使用示例见 `docs/Tutorial.ipynb` 和 `docs/GettingStart.md`。

```python
import SPTAG
index = SPTAG.AnnIndex('BKT', 'Float', dimension)
index.SetBuildParam("DistCalcMethod", "L2", "Index")
index.Build(vectors, num_vectors, False)
index.Save('output_folder')
```

## 配置文件

索引构建和搜索使用 INI 格式配置文件。关键参数：
- `DistCalcMethod` - 距离度量 (L2 或 Cosine)
- `MaxCheck` - 每次查询访问的节点数 (影响召回率/延迟)
- `NumberOfThreads` - 构建时的线程数
- `NeighborhoodSize` - 图中每个节点的邻居数

完整参数文档见 `docs/Parameters.md`。

### M1 Page Cache 配置参数

在 `[SearchSSDIndex]` section 中配置：
- `EnablePageCache` - 是否启用页面缓存 (默认 false)
- `PageCacheMaxBytes` - 缓存最大字节数 (默认 256MB)

推荐配置：
```ini
[SearchSSDIndex]
EnablePageCache=true
PageCacheMaxBytes=268435456
```

**注意**: 当前实现只缓存单页 posting (32% 的 posting)，256MB 足够。如需缓存 1-2 页 posting，需要先实现分片锁架构。

## 项目文档与记忆文件

以下文档记录了 SPANN 优化工作的进展和状态：

### SPANN_Beyond_Official_Baseline_Plan_20260502.md
**主要规划文档**，定义了在官方 strict `UInt8 + DEFAULT` baseline 上超越官方的目标和路径。
- 目标：st8 QPS >= 5945，Recall@10 >= 0.978319
- M1: Global I/O broker + page cache + in-flight coalescing
- M2-H: Hybrid selective code-first for bad postings
- M4: Primary-secondary payload dedupe
- 包含完整的 S0 诊断结果和 M1 实现完成报告

### SIFT1M_Official_Alignment_Summary.md
官方参数对齐与 strict `UInt8 + DEFAULT` baseline 测试结果总结。
- 包含完整的 `SearchThreadNum`、`InternalResultNum`、`NumberOfThreads` sweep 结果
- 记录了参数与性能指标的关系分析
- 关键结论：st=4~8 是吞吐有效区间，ir=64~96 是 Recall/QPS 平衡区间

### SPANN_M2_M3_Code_Plan_Review_20260501.md
SPANN M2/M3 代码与验证方案审查记录。
- 当前结论：two-stage 方向已证伪，官方 strict baseline 是性能主线
- 记录了各优先级任务的状态和验收条件
- 包含 P0~P5 的详细实验设计

### SPANN_M2_M3_UInt8_Optimization_Memo.md
P0/P1 优化工作记忆文件，记录具体实验结果。
- Two-stage posting 格式尝试与失败原因分析
- Per-phase cost attribution 结果
- 结论：two-stage 在 legacy 口径下无法超越官方 baseline

## 当前进度 (2026-05-03)

| 优先级 | 任务 | 状态 | 结果 |
|--------|------|------|------|
| S0 | Trace-only diagnosis | **已完成** | Cross-query page reuse 92.4%，M1 方向正确 |
| M1 Phase 1 | Single-page posting cache | **已完成** | QPS +3.7% (st8)，p99.9 恶化 |
| M1 Phase 2 | Verification + latency analysis | **已完成** | 256MB plateau，p99.9 恶化 3x |
| M1 Phase 3 | Shard Lock implementation | **已完成** | p99.9 问题解决，QPS +5.0% |
| M1 收尾 | 日志保存 + st sweep + 统计修正 | **已完成** | st1 回退 -6.2%，st4 回退 -4.8% |
| M2-H | Hybrid selective code-first | 暂缓 | 等待 M1 完成后评估 |
| M4 | Primary-secondary dedupe | 暂缓 | 需要更多 duplicate VID 证据 |

### M1 最终状态

**核心实验完成** / **st8 主线方案完成**

| st | Baseline QPS | Sharded QPS | Change | 状态 |
|----|--------------|-------------|--------|------|
| 1 | 925.7 | 868.1 | -6.2% | ⚠ 回退 |
| 4 | 3630.7 | 3457.8 | -4.8% | ⚠ 回退 |
| 8 | 5827.5 | **6120.0** | **+5.0%** | ✓ 达标 |
| 16 | 5835.5 | **6631.3** | **+13.6%** | ✓ 大幅提升 |

### 关键结论

1. **ShardedPageCache 成功**：p99.9 从 10ms 降到 3.7ms
2. **锁竞争降低 87%**：lock wait 从 449ms 降到 75ms
3. **st8 QPS = 6120**：超过官方 baseline 目标 (5945)
4. **st1/st4 回退**：低并发下 cache 开销 > I/O 节省

### 推荐方案

**ShardedPageCache + single-page cache + no admission**

适用场景：st >= 8 的高并发搜索

```ini
[SearchSSDIndex]
EnablePageCache=true
PageCacheMaxBytes=268435456
```

## 常用测试命令

```bash
# 运行 M1 缓存测试
cd /home/ray/code/SPTAG
./Release/ssdserving results/io_analysis/m1_test/m1_test_cache_enabled.ini

# 查看 cache 统计
grep "Page cache stats" results/io_analysis/m1_test/*.log

# 对比 baseline
./Release/ssdserving results/io_analysis/m1_test/m1_test_st8.ini
```
