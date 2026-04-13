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
