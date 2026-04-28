# SPANN 索引性能测试指南

本文档记录了 SPTAG 项目编译完成后，对 SPANN 索引算法进行性能测试的完整过程。

## 目录

1. [环境准备](#1-环境准备)
2. [数据集准备](#2-数据集准备)
3. [索引构建](#3-索引构建)
4. [搜索性能测试](#4-搜索性能测试)
5. [测试结果分析](#5-测试结果分析)
6. [硬件监控](#6-硬件监控)
7. [测试脚本与配置文件](#7-测试脚本与配置文件)

---

## 1. 环境准备

### 1.1 项目编译

首先需要初始化 git 子模块并编译项目：

```bash
# 初始化 zstd 子模块（必需）
git submodule update --init ThirdParty/zstd

# 创建构建目录并配置 CMake
mkdir -p build
cd build
cmake -DSPDK=OFF -DROCKSDB=OFF ..

# 编译
make -j$(nproc)
```

编译完成后，可执行文件输出到 `Release/` 目录：

| 可执行文件 | 用途 |
|-----------|------|
| `ssdserving` | SPANN 索引构建与搜索（主要工具） |
| `indexbuilder` | 构建内存索引 |
| `indexsearcher` | 搜索内存索引 |
| `server` | Socket 搜索服务 |
| `quantizer` | 训练 PQ/OPQ 量化器 |
| `SPTAGTest` | 测试程序 |

### 1.2 依赖检查

确保系统满足以下依赖：
- CMake >= 3.12
- GCC >= 5.0
- Boost >= 1.67
- OpenMP
- TBB（默认启用）

---

## 2. 数据集准备

### 2.1 数据集格式

本次测试使用 SIFT1M 数据集，位于 `/media/ray/1tb/sift1m/` 目录。

**文件结构：**
```
sift1m/
├── sift_base.fvecs       # 基础向量库 (1,000,000 × 128)
├── sift_query.fvecs      # 查询向量 (10,000 × 128)
├── sift_groundtruth.ivecs # 真值 (10,000 × 100)
└── sift_learn.fvecs      # 学习集（可选）
```

**文件格式说明：**

**fvecs 格式（浮点向量）：**
```
<4字节维度><维度×4字节浮点数据>
<4字节维度><维度×4字节浮点数据>
...
```

**ivecs 格式（整型向量）：**
```
<4字节维度><维度×4字节整型数据>
<4字节维度><维度×4字节整型数据>
...
```

### 2.2 数据集验证

使用 Python 验证数据集：

```python
import struct
import numpy as np

def load_fvecs(filepath):
    with open(filepath, 'rb') as f:
        dim = struct.unpack('i', f.read(4))[0]
        f.seek(0, 2)
        size = f.tell()
        n = size // (4 + dim * 4)
        f.seek(0)
        data = np.frombuffer(f.read(), dtype=np.float32)
        return data.reshape(n, dim + 1)[:, 1:].copy()

# 验证数据集
base = load_fvecs('sift_base.fvecs')
query = load_fvecs('sift_query.fvecs')
print(f"基础向量: {base.shape}")  # (1000000, 128)
print(f"查询向量: {query.shape}")  # (10000, 128)
```

---

## 3. 索引构建

### 3.1 配置文件结构

SPANN 索引构建使用 INI 格式配置文件，包含以下几个部分：

```ini
[Base]           # 基础配置（数据路径、向量类型等）
[SelectHead]     # 头节点选择配置
[BuildHead]      # 头索引构建配置
[BuildSSDIndex]  # SSD 索引构建配置
[SearchSSDIndex] # 搜索配置
```

### 3.2 完整构建配置

创建文件 `spann_sift1m_config.ini`：

```ini
[Base]
ValueType=Float
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/media/ray/1tb/sift1m/sift_base.fvecs
VectorType=XVEC
QueryPath=/media/ray/1tb/sift1m/sift_query.fvecs
QueryType=XVEC
WarmupPath=/media/ray/1tb/sift1m/sift_query.fvecs
WarmupType=XVEC
TruthPath=/media/ray/1tb/sift1m/sift_groundtruth.ivecs
TruthType=XVEC
IndexDirectory=/media/ray/1tb/sift1m/spann_index

[SelectHead]
isExecute=true
TreeNumber=1
BKTKmeansK=32
BKTLeafSize=8
SamplesNumber=1000
SaveBKT=false
SelectThreshold=50
SplitFactor=6
SplitThreshold=100
Ratio=0.16
NumberOfThreads=16

[BuildHead]
isExecute=true
NeighborhoodSize=32
TPTNumber=32
TPTLeafSize=2000
MaxCheck=8192
MaxCheckForRefineGraph=8192
RefineIterations=3
NumberOfThreads=16

[BuildSSDIndex]
isExecute=true
BuildSsdIndex=true
InternalResultNum=64
ReplicaCount=8
PostingPageLimit=12
NumberOfThreads=16
MaxCheck=8192
TmpDir=/tmp/

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=32
NumberOfThreads=1
HashTableExponent=4
ResultNum=10
MaxCheck=2048
MaxDistRatio=8.0
SearchPostingPageLimit=12
```

### 3.3 配置参数详解

#### 3.3.1 [Base] 基础配置

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `ValueType` | string | Float | 向量数据类型：Float / Int8 / Int16 / UInt8 |
| `DistCalcMethod` | string | Cosine | 距离度量：L2（欧氏距离）/ Cosine（余弦距离） |
| `IndexAlgoType` | string | KDT | 索引算法：BKT / KDT |
| `Dim` | int | -1 | 向量维度（必需） |
| `VectorPath` | string | "" | 基础向量文件路径 |
| `VectorType` | string | DEFAULT | 文件格式：XVEC（fvecs/ivecs）/ DEFAULT / TXT |
| `QueryPath` | string | "" | 查询向量文件路径 |
| `QueryType` | string | DEFAULT | 查询文件格式 |
| `WarmupPath` | string | "" | 预热向量文件路径（搜索前预加载） |
| `TruthPath` | string | "" | 真值文件路径（用于计算召回率） |
| `TruthType` | string | DEFAULT | 真值文件格式 |
| `IndexDirectory` | string | "SPANN" | 索引存储目录 |

#### 3.3.2 [SelectHead] 头节点选择配置

此阶段从所有向量中选择一部分作为"头节点"，用于构建内存索引。

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行此阶段 |
| `TreeNumber` | int | 1 | BKT 树数量 |
| `BKTKmeansK` | int | 32 | K-means 聚类的 K 值，每个节点的子节点数 |
| `BKTLeafSize` | int | 8 | BKT 叶子节点大小 |
| `SamplesNumber` | int | 1000 | 用于树节点分裂的采样点数 |
| `BKTLambdaFactor` | float | -1.0 | 平衡因子（-1 表示自动计算） |
| `NumberOfThreads` | int | 4 | **构建线程数**（⚠️ 注意：存在 bug，见下文说明） |
| `SaveBKT` | bool | false | 是否保存 BKT 树结构 |
| `SelectThreshold` | int | 6 | 选择阈值 |
| `SplitFactor` | int | 5 | 分裂因子 |
| `SplitThreshold` | int | 25 | 分裂阈值 |
| `Ratio` | double | 0.2 | 头节点比例（如 0.16 表示 16% 的向量作为头节点） |
| `Count` | int | 0 | 头节点数量（优先于 Ratio） |
| `SelectHeadType` | string | "BKT" | 选择方式：BKT / Random / Clustering |

**参数影响：**
- `Ratio` 越大，头节点越多，内存索引越大，搜索速度越快
- `BKTKmeansK` 影响树的结构和搜索效率
- `NumberOfThreads` 影响构建速度

#### 3.3.3 [BuildHead] 头索引构建配置

此阶段为选中的头节点构建内存索引（BKT + RNG 图）。

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行此阶段 |

> 注：BuildHead 阶段的线程数等参数继承自头索引的配置，详见 `Index` 部分。

**头索引参数**（通过 `[BuildHead]` 部分或内部配置）：

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `NeighborhoodSize` | int | 32 | 图中每个节点的邻居数 |
| `TPTNumber` | int | 32 | TPT 树数量（加速图构建） |
| `TPTLeafSize` | int | 2000 | TPT 树叶子大小 |
| `MaxCheck` | int | 8192 | 图构建时的搜索深度 |
| `MaxCheckForRefineGraph` | int | 10000 | 图精炼时的搜索深度 |
| `RefineIterations` | int | 3 | 图精炼迭代次数 |
| `NumberOfThreads` | int | 1 | 构建线程数 |

**参数影响：**
- `NeighborhoodSize` 越大，图连通性越好，但内存占用增加
- `RefineIterations` 越多，图质量越高，但构建时间增加
- `MaxCheck` 影响图构建质量

#### 3.3.4 [BuildSSDIndex] SSD 索引构建配置

此阶段为所有向量构建 posting 列表，存储到 SSD。

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行此阶段 |
| `BuildSsdIndex` | bool | false | 是否构建 SSD 索引（true 时执行构建） |
| `NumberOfThreads` | int | 16 | **构建线程数**（⚠️ 实测值为 1 时效率最高） |
| `InternalResultNum` | int | 64 | 内部搜索结果数 |
| `ReplicaCount` | int | 8 | 每个向量的副本数量（存储到多个头节点） |
| `PostingPageLimit` | int | 3 | Posting 页面大小限制（页数） |
| `MaxCheck` | int | 4096 | 构建时搜索深度 |
| `HashTableExponent` | int | 4 | 哈希表指数（影响搜索精度） |
| `TmpDir` | string | "." | 临时文件目录 |
| `RNGFactor` | float | 1.0 | RNG 因子 |
| `ExcludeHead` | bool | true | 是否排除头节点 |

**参数影响：**
- `ReplicaCount` 越大，召回率越高，但存储空间增加
- `PostingPageLimit` 影响 posting 存储效率
- `HashTableExponent` 影响哈希表大小，值越大搜索越精确但内存占用增加

#### 3.3.5 [SearchSSDIndex] 搜索配置

此阶段执行搜索测试。

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行此阶段 |
| `BuildSsdIndex` | bool | false | 是否构建索引（false 时仅搜索） |
| `SearchThreadNum` | int | 2 | **搜索线程数**（关键参数） |
| `NumberOfThreads` | int | 16 | ⚠️ 搜索阶段此参数不生效，使用 SearchThreadNum |
| `ResultNum` | int | 5 | 返回结果数（Top-K） |
| `MaxCheck` | int | 4096 | 最大检查节点数（影响召回率和延迟） |
| `HashTableExponent` | int | 4 | 哈希表指数（推荐 12 以提高搜索性能） |
| `SearchInternalResultNum` | int | 64 | 搜索内部结果数 |
| `SearchPostingPageLimit` | int | 3 | 搜索时 posting 页面限制 |
| `MaxDistRatio` | float | 10000 | 最大距离比率阈值 |
| `IOThreadsPerHandler` | int | 4 | 每个 handler 的 I/O 线程数 |

**参数影响：**
- `SearchThreadNum` 直接影响 QPS，推荐设置为 CPU 核心数的一半
- `MaxCheck` 越大召回率越高，但延迟增加
- `HashTableExponent=12` 可显著提升搜索性能（但会使构建变慢）

---

### 3.4 关键参数说明

| 参数 | 说明 | 影响 |
|------|------|------|
| `ValueType` | 向量数据类型 | Float/Int8/Int16/UInt8 |
| `DistCalcMethod` | 距离度量方法 | L2（欧氏距离）/ Cosine（余弦距离） |
| `VectorType` | 文件格式 | XVEC（fvecs/ivecs）/ DEFAULT / TXT |
| `Ratio` | 头节点比例 | 影响索引大小和搜索速度 |
| `NeighborhoodSize` | 图邻居数 | 影响召回率和索引大小 |
| `ReplicaCount` | 副本数量 | 影响召回率和存储空间 |
| `MaxCheck` | 最大检查节点数 | 影响召回率和搜索延迟 |
| `PostingPageLimit` | Posting 页面限制 | 影响每个 posting 的大小 |

### 3.4 执行索引构建

```bash
./Release/ssdserving spann_sift1m_config.ini
```

### 3.5 构建过程详解

索引构建分为三个阶段：

**阶段 1: SelectHead（头节点选择）**
- 使用 BKT（Balanced K-Means Tree）对向量进行聚类
- 动态选择约 16% 的向量作为头节点
- 输出：头节点 ID 列表

**阶段 2: BuildHead（头索引构建）**
- 为头节点构建内存索引（BKT + RNG 图）
- 使用 TPT 树加速图构建
- 进行多次图精炼迭代
- 输出：HeadIndex/ 目录

**阶段 3: BuildSSDIndex（SSD 索引构建）**
- 为每个向量找到最近的头节点（副本）
- 构建 posting 列表
- 按页面组织数据以优化磁盘 I/O
- 输出：SPTAGFullList.bin

### 3.6 构建输出

```
spann_index/
├── HeadIndex/              # 头索引目录
│   ├── tree.bin           # BKT 树结构
│   ├── graph.bin          # RNG 图结构
│   ├── vectors.bin        # 头向量数据
│   ├── deletes.bin        # 删除标记
│   └── indexloader.ini    # 索引加载配置
├── SPTAGFullList.bin      # SSD posting 列表（主要存储）
├── SPTAGHeadVectorIDs.bin # 头节点 ID 映射
├── SPTAGHeadVectors.bin   # 头向量备份
└── DeletedIDs.bin         # 版本控制信息
```

---

## 4. 搜索性能测试

### 4.1 搜索配置文件

创建单独的搜索配置文件 `configs/spann_search_only.ini`：

```ini
[Base]
ValueType=Float
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/media/ray/1tb/sift1m/sift_base.fvecs
VectorType=XVEC
QueryPath=/media/ray/1tb/sift1m/sift_query.fvecs
QueryType=XVEC
TruthPath=/media/ray/1tb/sift1m/sift_groundtruth.ivecs
TruthType=XVEC
IndexDirectory=/media/ray/1tb/sift1m/spann_index
SearchResult=/media/ray/1tb/sift1m/search_results.bin

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[BuildSSDIndex]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=32
NumberOfThreads=4
HashTableExponent=12
ResultNum=10
MaxCheck=2048
MaxDistRatio=8.0
SearchPostingPageLimit=15
```

**关键设置：**
- `isExecute=false` 跳过构建阶段
- `isExecute=true` 启用搜索阶段
- `NumberOfThreads=4` 并行搜索线程数

### 4.2 执行搜索测试

```bash
./Release/ssdserving configs/spann_search_only.ini
```

### 4.3 搜索流程

1. **加载索引**
   - 加载头索引（内存）
   - 打开 SSD 索引文件
   - 构建 posting 映射

2. **执行搜索**
   - 对每个查询向量：
     a. 在头索引中搜索最近的头节点
     b. 访问对应头节点的 posting 列表
     c. 计算精确距离并排序
     d. 返回 Top-K 结果

3. **计算指标**
   - 召回率（Recall）
   - 平均倒数排名（MRR）
   - 查询吞吐量（QPS）
   - 延迟分布

---

## 5. 测试结果分析

### 5.1 构建性能

| 阶段 | 耗时 | 说明 |
|------|------|------|
| SelectHead | 188 秒 | 选择约 16 万个头节点 |
| BuildHead | 279 秒 | 构建头索引图结构 |
| BuildSSDIndex | 226 秒 | 构建 posting 列表 |
| **总计** | **693 秒 (~11.5 分钟)** | |

### 5.2 搜索性能

| 指标 | 值 | 说明 |
|------|-----|------|
| **Recall@10** | **93.89%** | Top-10 召回率 |
| MRR@10 | 100% | 平均倒数排名 |
| **QPS** | **73.65** | 每秒查询数 |
| 平均延迟 | 27.15 ms | 端到端查询延迟 |

### 5.3 延迟分布

| 百分位 | Head 延迟 (ms) | Ex 延迟 (ms) | 总延迟 (ms) |
|--------|----------------|--------------|-------------|
| P50 | 16.15 | 9.01 | 25.69 |
| P90 | 23.56 | 11.62 | 33.00 |
| P95 | 25.00 | 12.80 | 34.92 |
| P99 | 29.74 | 14.86 | 40.83 |
| P99.9 | 36.23 | 17.15 | 47.73 |

**延迟组成：**
- **Head 延迟**：在内存头索引中搜索的时间
- **Ex 延迟**：访问磁盘 posting 列表的时间

### 5.4 磁盘访问统计

| 指标 | 平均值 | 说明 |
|------|--------|------|
| 磁盘页访问 | 182.4 | 每次查询访问的 4KB 页数 |
| 磁盘 I/O 次数 | 31.7 | 异步 I/O 操作次数 |

### 5.5 索引大小

| 文件 | 大小 | 说明 |
|------|------|------|
| SPTAGFullList.bin | 2.9 GB | 主要存储（posting 列表） |
| HeadIndex/ | ~100 MB | 头索引（内存加载） |
| **总计** | **~3 GB** | 约为原始数据的 1.5 倍 |

### 5.6 线程数对性能和内存的影响

通过测试不同 `SearchThreadNum` 配置，发现线程数对搜索性能和内存占用有显著影响。

以下测试结果均使用 `-C` 选项清除系统缓存，确保数据从磁盘读取，反映真实的生产环境性能。

#### 测试结果

| 线程数 | 实际线程数 | 搜索耗时 | QPS | Recall@10 | 内存占用 |
|--------|-----------|---------|-----|-----------|---------|
| 2 | 4 | 142.1s | 70.35 | 93.89% | ~1.4 GB |
| 4 | 6 | 117.4s | 85.20 | 93.89% | ~2.7 GB |
| 8 | 10 | 116.7s | **85.72** | 93.89% | ~5.3 GB |
| 16 | 18 | 125.5s | 79.67 | 93.89% | ~10.4 GB |

> **注**：实际线程数 = SearchThreadNum + 2（辅助线程）

#### 最优配置

**推荐使用 8 线程**：QPS 最高（85.72），内存占用适中（~5.3 GB）。

4 线程与 8 线程性能接近（85.20 vs 85.72），但内存占用仅为一半，适合内存受限的场景。

#### 内存占用与线程数的关系

内存占用随线程数线性增长，原因如下：

每个搜索线程需要分配独立的 **WorkSpace** 结构，包含：

```
ExtraWorkSpace {
    m_postingIDs           // posting ID 列表
    m_deduper              // 去重哈希表
    m_pageBuffers          // 页面缓冲区 (~2 MB/线程)
    m_diskRequests         // 异步 I/O 请求缓冲区
    m_decompressBuffer     // 解压缓冲区
}
```

每个 WorkSpace 约 2-3 MB，加上页面缓存和异步 I/O 缓冲区，实际每线程占用约 **0.6 GB**。

#### 重要发现：配置参数区别

| 参数名 | 所属部分 | 用途 | 默认值 |
|--------|----------|------|--------|
| `NumberOfThreads` | `[BuildSSDIndex]` | SSD 索引构建线程数 | 16 |
| `SearchThreadNum` | `[SearchSSDIndex]` | **搜索线程数** | **2** |

⚠️ **常见错误**：在 `[SearchSSDIndex]` 部分配置 `NumberOfThreads`，但搜索线程数实际由 `SearchThreadNum` 控制。

正确的配置示例：
```ini
[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=32
SearchThreadNum=8        # 搜索线程数（关键参数）
NumberOfThreads=16       # 此参数在搜索阶段不生效
MaxCheck=2048
ResultNum=10
```

#### 瓶颈分析

- **2-4 线程**：QPS 随线程数显著提升（70.35 → 85.20），磁盘 I/O 带宽未饱和
- **4-8 线程**：QPS 基本持平（85.20 → 85.72），磁盘 I/O 带宽接近饱和
- **8-16 线程**：QPS 反而下降（85.72 → 79.67），线程竞争和上下文切换开销增加

**结论**：磁盘 I/O 是 SPANN 搜索的主要瓶颈，升级到更快的存储（如 NVMe SSD）可显著提升性能。在本测试环境（SATA SSD）下，4-8 线程为最优配置。

---

## 6. 硬件监控

### 6.1 监控脚本功能

`spann_monitor.sh` 是专为 SPANN 性能测试设计的硬件监控脚本，提供以下功能：

- **实时监控**：CPU 使用率、内存占用、磁盘 I/O
- **阶段识别**：自动识别 SelectHead、BuildHead、BuildSSDIndex、SearchSSDIndex 阶段
- **统计摘要**：自动生成各阶段耗时和资源使用统计

### 6.2 使用方法

**方式一：指定配置文件**
```bash
./spann_monitor.sh -c spann_sift1m_config.ini
```

**方式二：自定义输出路径**
```bash
./spann_monitor.sh -c configs/spann_search_only.ini -o metrics.csv -l experiment.log
```

**方式三：完整命令行**
```bash
./spann_monitor.sh -d /media/ray/1tb -o metrics.csv -l experiment.log -- ./Release/ssdserving config.ini
```

### 6.3 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-d, --disk` | 监控的磁盘挂载点 | `/media/ray/1tb` |
| `-o, --output` | CSV 输出文件 | `spann_monitor.csv` |
| `-l, --log` | LOG 输出文件 | `spann_monitor.log` |
| `-i, --interval` | 采样间隔（秒） | `1` |
| `-c, --config` | SPANN 配置文件 | - |
| `-C, --clear-cache` | 清除系统缓存（需要 sudo） | `false` |
| `-h, --help` | 显示帮助信息 | - |

### 6.4 清除缓存功能

使用 `-C` 或 `--clear-cache` 选项可以在测试前清除系统缓存，确保测试结果不受之前运行的影响：

```bash
# 清除缓存后运行测试
./spann_monitor.sh -c configs/spann_search_only.ini -C

# 完整示例（结果保存到 results 目录）
./spann_monitor.sh -c configs/spann_search_only.ini -C -o results/search_nocache.csv -l results/search_nocache.log
```

**清除缓存功能说明：**

该选项会执行以下操作：
1. 同步文件系统缓冲区 (`sync`)
2. 清除页面缓存 (Page Cache)
3. 清除目录项缓存 (Dentries)
4. 清除 Inode 缓存 (Inodes)

**注意事项：**
- 需要 `sudo` 权限，脚本会提示输入密码
- 清除缓存后首次运行会从磁盘读取所有数据，性能会较低
- 适用于需要测量"冷启动"性能的场景

**缓存对性能的影响示例：**

| 测试条件 | 搜索耗时 | QPS | 说明 |
|----------|---------|-----|------|
| 有缓存 | 130.9s | 76.41 | 数据已在内存中 |
| 无缓存 (-C) | 146.4s | 68.29 | 从磁盘读取 |

### 6.5 输出文件格式

**CSV 文件列：**
| 列名 | 类型 | 说明 |
|------|------|------|
| `timestamp` | string | ISO 8601 时间戳 |
| `elapsed_time` | float | 经过时间（秒） |
| `stage` | string | 当前阶段 |
| `cpu_percent` | float | CPU 使用率（%） |
| `mem_rss_mb` | float | 物理内存（MB） |
| `mem_vsz_mb` | float | 虚拟内存（MB） |
| `process_status` | string | 进程状态 |
| `threads` | int | 线程数 |
| `disk_read_bytes` | int | 累计读取字节 |
| `disk_write_bytes` | int | 累计写入字节 |
| `disk_read_rate_mbs` | float | 读取速率（MB/s） |
| `disk_write_rate_mbs` | float | 写入速率（MB/s） |

### 6.6 监控示例输出

```
[   136.4s] 阶段: SearchSSDIndex  CPU: 168.0%  内存: 1425.6 MB  磁盘读取:  47.87 MB/s  写入:   0.00 MB/s

========================================
实验统计摘要
========================================
总运行时间: 138.7 秒
CPU 使用率: 平均 164.4%, 峰值 168.00%
内存占用: 平均 1396.9 MB, 峰值 1425.57 MB
磁盘 I/O: 读取 7125 MB, 写入 0 MB

阶段耗时:
  SearchSSDIndex: 136.0s

CSV 文件: results/spann_search_nocache.csv
LOG 文件: results/spann_search_nocache.log
```

### 6.7 阶段识别关键词

脚本通过监控日志输出自动识别当前阶段：

| 阶段 | 识别关键词 |
|------|-----------|
| SelectHead | `Begin Select Head` |
| BuildHead | `Begin Build Head` |
| BuildSSDIndex | `Begin Build SSDIndex` |
| Warmup | `Start warmup` |
| SearchSSDIndex | `Start ANN Search` |
| finished | `Finish ANN Search` |

---

## 7. 测试脚本与配置文件

### 7.1 完整测试脚本

创建 `run_spann_benchmark.sh`：

```bash
#!/bin/bash
# SPANN 索引性能完整测试脚本

set -e

SPTAG_ROOT="/home/ray/code/SPTAG"
SSDSERVING="${SPTAG_ROOT}/Release/ssdserving"
DATA_DIR="/media/ray/1tb/sift1m"
INDEX_DIR="${DATA_DIR}/spann_index"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="${SPTAG_ROOT}/spann_benchmark_${TIMESTAMP}.log"

echo "=========================================="
echo "SPANN 索引性能测试"
echo "=========================================="
echo "测试时间: $(date)"
echo "日志文件: ${LOG_FILE}"

# 检查数据集
echo "检查数据集文件..."
for f in "${DATA_DIR}/sift_base.fvecs" "${DATA_DIR}/sift_query.fvecs" "${DATA_DIR}/sift_groundtruth.ivecs"; do
    if [ ! -f "$f" ]; then
        echo "错误: 找不到文件 $f"
        exit 1
    fi
done

# 清理旧索引（如需重新构建）
# rm -rf "${INDEX_DIR}"

# 创建搜索配置
SEARCH_CONFIG="${SPTAG_ROOT}/spann_search_test.ini"
cat > "${SEARCH_CONFIG}" << EOF
[Base]
ValueType=Float
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=${DATA_DIR}/sift_base.fvecs
VectorType=XVEC
QueryPath=${DATA_DIR}/sift_query.fvecs
QueryType=XVEC
TruthPath=${DATA_DIR}/sift_groundtruth.ivecs
TruthType=XVEC
IndexDirectory=${INDEX_DIR}
SearchResult=${DATA_DIR}/search_results.bin

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[BuildSSDIndex]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=32
NumberOfThreads=4
HashTableExponent=12
ResultNum=10
MaxCheck=2048
MaxDistRatio=8.0
SearchPostingPageLimit=15
EOF

# 运行测试
echo "开始搜索性能测试..."
cd "${SPTAG_ROOT}"
${SSDSERVING} "${SEARCH_CONFIG}" 2>&1 | tee "${LOG_FILE}"

echo ""
echo "=========================================="
echo "测试完成!"
echo "=========================================="
grep -E "(Recall|QPS|MRR|Latency)" "${LOG_FILE}" | tail -10
```

### 7.2 配置文件模板

**索引构建配置** (`spann_build_config.ini`)：
```ini
[Base]
ValueType=Float
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=<向量数据路径>
VectorType=XVEC
QueryPath=<查询数据路径>
QueryType=XVEC
TruthPath=<真值文件路径>
TruthType=XVEC
IndexDirectory=<索引输出目录>

[SelectHead]
isExecute=true
TreeNumber=1
BKTKmeansK=32
BKTLeafSize=8
SamplesNumber=1000
Ratio=0.16
NumberOfThreads=16

[BuildHead]
isExecute=true
NeighborhoodSize=32
TPTNumber=32
TPTLeafSize=2000
MaxCheck=8192
RefineIterations=3
NumberOfThreads=16

[BuildSSDIndex]
isExecute=true
BuildSsdIndex=true
InternalResultNum=64
ReplicaCount=8
PostingPageLimit=12
NumberOfThreads=16
MaxCheck=8192
TmpDir=/tmp/

[SearchSSDIndex]
isExecute=false
```

**搜索测试配置** (`spann_search_config.ini`)：
```ini
[Base]
ValueType=Float
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=<向量数据路径>
VectorType=XVEC
QueryPath=<查询数据路径>
QueryType=XVEC
TruthPath=<真值文件路径>
TruthType=XVEC
IndexDirectory=<索引目录>

[SelectHead]
isExecute=false

[BuildHead]
isExecute=false

[BuildSSDIndex]
isExecute=false

[SearchSSDIndex]
isExecute=true
BuildSsdIndex=false
InternalResultNum=32
NumberOfThreads=4
HashTableExponent=12
ResultNum=10
MaxCheck=2048
MaxDistRatio=8.0
SearchPostingPageLimit=15
```

---

## 附录 A：完整配置参数参考

以下参数列表来源于源码 `AnnService/inc/Core/SPANN/ParameterDefinitionList.h`。

### A.1 [Base] 部分

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `ValueType` | string | Undefined | 向量数据类型：Float / Int8 / Int16 / UInt8 |
| `DistCalcMethod` | string | Undefined | 距离度量：L2 / Cosine |
| `IndexAlgoType` | string | KDT | 索引算法：BKT / KDT |
| `Dim` | int | -1 | 向量维度 |
| `VectorPath` | string | "" | 向量文件路径 |
| `VectorType` | string | DEFAULT | 文件格式：DEFAULT / XVEC / TXT |
| `VectorSize` | int | -1 | 向量数量（可选） |
| `VectorDelimiter` | string | "\|" | TXT 格式分隔符 |
| `QueryPath` | string | "" | 查询文件路径 |
| `QueryType` | string | Undefined | 查询文件格式 |
| `QuerySize` | int | -1 | 查询数量 |
| `QueryDelimiter` | string | "\|" | 查询文件分隔符 |
| `WarmupPath` | string | "" | 预热文件路径 |
| `WarmupType` | string | Undefined | 预热文件格式 |
| `TruthPath` | string | "" | 真值文件路径 |
| `TruthType` | string | Undefined | 真值文件格式 |
| `GenerateTruth` | bool | false | 是否生成真值 |
| `IndexDirectory` | string | "SPANN" | 索引目录 |
| `HeadVectorIDs` | string | "SPTAGHeadVectorIDs.bin" | 头节点 ID 文件 |
| `DeletedIDs` | string | "DeletedIDs.bin" | 删除 ID 文件 |
| `HeadVectors` | string | "SPTAGHeadVectors.bin" | 头向量文件 |
| `HeadIndexFolder` | string | "HeadIndex" | 头索引目录 |
| `SSDIndex` | string | "SPTAGFullList.bin" | SSD 索引文件 |
| `DeleteHeadVectors` | bool | false | 是否删除头向量 |
| `SSDIndexFileNum` | int | 1 | SSD 索引文件数 |
| `QuantizerFilePath` | string | "" | 量化器文件路径 |
| `DataBlockSize` | int | 1048576 | 数据块大小 |
| `DataCapacity` | int | MaxSize | 数据容量 |

### A.2 [SelectHead] 部分

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行 |
| `TreeNumber` | int | 1 | 树数量 |
| `BKTKmeansK` | int | 32 | K-means K 值 |
| `BKTLeafSize` | int | 8 | 叶子大小 |
| `SamplesNumber` | int | 1000 | 采样数 |
| `BKTLambdaFactor` | float | -1.0 | 平衡因子 |
| `NumberOfThreads` | int | 4 | 线程数 |
| `SaveBKT` | bool | false | 保存 BKT |
| `AnalyzeOnly` | bool | false | 仅分析 |
| `CalcStd` | bool | false | 计算标准差 |
| `SelectDynamically` | bool | true | 动态选择 |
| `NoOutput` | bool | false | 不输出 |
| `SelectThreshold` | int | 6 | 选择阈值 |
| `SplitFactor` | int | 5 | 分裂因子 |
| `SplitThreshold` | int | 25 | 分裂阈值 |
| `SplitMaxTry` | int | 8 | 最大尝试次数 |
| `Ratio` | double | 0.2 | 头节点比例 |
| `Count` | int | 0 | 头节点数量 |
| `RecursiveCheckSmallCluster` | bool | true | 递归检查小簇 |
| `PrintSizeCount` | bool | true | 打印大小计数 |
| `SelectHeadType` | string | "BKT" | 选择类型：BKT / Random / Clustering |

### A.3 [BuildHead] 部分

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行 |

### A.4 [BuildSSDIndex] 部分

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行 |
| `BuildSsdIndex` | bool | false | 构建索引 |
| `NumberOfThreads` | int | 16 | 线程数 |
| `EnableDeltaEncoding` | bool | false | 启用增量编码 |
| `EnablePostingListRearrange` | bool | false | 重排 posting |
| `EnableDataCompression` | bool | false | 数据压缩 |
| `EnableDictTraining` | bool | true | 字典训练 |
| `MinDictTrainingBufferSize` | int | 10240000 | 最小字典缓冲 |
| `DictBufferCapacity` | int | 204800 | 字典缓冲容量 |
| `ZstdCompressLevel` | int | 0 | Zstd 压缩级别 |
| `InternalResultNum` | int | 64 | 内部结果数 |
| `PostingPageLimit` | int | 3 | Posting 页面限制 |
| `ReplicaCount` | int | 8 | 副本数 |
| `OutputEmptyReplicaID` | bool | false | 输出空副本 ID |
| `Batches` | int | 1 | 批次数 |
| `TmpDir` | string | "." | 临时目录 |
| `RNGFactor` | float | 1.0 | RNG 因子 |
| `RecallTestSampleNumber` | int | 100 | 召回测试采样数 |
| `ExcludeHead` | bool | true | 排除头节点 |
| `PostingVectorLimit` | int | 118 | Posting 向量限制 |
| `FullDeletedIDFile` | string | "fulldeleted" | 完整删除 ID 文件 |
| `Storage` | string | STATIC | 存储类型 |
| `SpdkBatchSize` | int | 64 | SPDK 批量大小 |
| `KVFile` | string | "rocksdb" | KV 文件 |
| `SsdMappingFile` | string | "ssdmapping" | SSD 映射文件 |
| `SsdInfoFile` | string | "ssdinfo" | SSD 信息文件 |
| `ChecksumFile` | string | "checksum" | 校验和文件 |
| `UseDirectIO` | bool | false | 直接 I/O |
| `PreReassign` | bool | false | 预重分配 |
| `PreReassignRatio` | float | 0.7 | 预重分配比例 |
| `BufferLength` | int | 3 | 缓冲区长度 |
| `EnableWAL` | bool | false | 启用 WAL |
| `DisableCheckpoint` | bool | false | 禁用检查点 |
| `GPUSSDNumTrees` | int | 100 | GPU SSD 树数 |
| `GPUSSDLeafSize` | int | 200 | GPU SSD 叶子大小 |
| `NumGPUs` | int | 1 | GPU 数量 |

### A.5 [SearchSSDIndex] 部分

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `isExecute` | bool | false | 是否执行 |
| `SearchResult` | string | "" | 搜索结果文件 |
| `LogFile` | string | "" | 日志文件 |
| `QpsLimit` | int | 0 | QPS 限制 |
| `ResultNum` | int | 5 | 返回结果数 |
| `TruthResultNum` | int | -1 | 真值结果数 |
| `MaxCheck` | int | 4096 | 最大检查节点数 |
| `HashTableExponent` | int | 4 | 哈希表指数 |
| `QueryCountLimit` | int | MaxInt | 查询数量限制 |
| `MaxDistRatio` | float | 10000 | 最大距离比率 |
| `IOThreadsPerHandler` | int | 4 | 每个 handler 的 I/O 线程数 |
| `SearchInternalResultNum` | int | 64 | 搜索内部结果数 |
| `SearchPostingPageLimit` | int | 3 | 搜索 posting 页面限制 |
| `Rerank` | int | 0 | 重排序 |
| `EnableADC` | bool | false | 启用 ADC |
| `RecallAnalysis` | bool | false | 召回分析 |
| `DebugBuildInternalResultNum` | int | 64 | 调试内部结果数 |
| `IOTimeout` | int | 30 | I/O 超时（秒） |
| `TruthFilePrefix` | string | "" | 真值文件前缀 |
| `CalTruth` | bool | true | 计算真值 |
| `OnlySearchFinalBatch` | bool | false | 仅搜索最后批次 |
| `SearchTimes` | int | 1 | 搜索次数 |
| `SearchThreadNum` | int | 2 | **搜索线程数** |
| `MinInternalResultNum` | int | -1 | 最小内部结果数 |
| `StepInternalResultNum` | int | -1 | 内部结果数步长 |
| `MaxInternalResultNum` | int | -1 | 最大内部结果数 |

---

## 附录 B：已知问题和注意事项

### B.1 参数共享问题

**问题描述**：SPTAG 的某些参数在所有阶段之间全局共享，后执行的阶段配置可能覆盖前面的设置。

**具体表现**：
- `[SearchSSDIndex]` 中的 `HashTableExponent=12` 会覆盖 `[BuildSSDIndex]` 的设置
- `HashTableExponent=12` 会使 BuildSSDIndex 阶段变慢约 256 倍

**解决方案**：将构建和搜索分成两个独立的配置文件执行：
1. `configs/spann_build_only.ini` - 仅构建，`[SearchSSDIndex].isExecute=false`
2. `configs/spann_search_only.ini` - 仅搜索，其他阶段 `isExecute=false`

### B.2 SelectHead 阶段 NumberOfThreads 不生效

**问题描述**：`[SelectHead]` 部分的 `NumberOfThreads` 配置**实际不生效**。

**原因**：源码 bug。在 `AnnService/inc/SSDServing/SelectHead.h` 第 797 和 802 行，代码使用了 `opts.m_iNumberOfThreads`，但 `SPANN::Options` 类中不存在此成员变量。正确的变量名应为 `opts.m_iSelectHeadNumberOfThreads`。

**影响**：SelectHead 阶段的线程数固定使用默认值（较低），无法通过配置调整。

**临时方案**：此 bug 需要修改源码才能解决。

### B.3 BuildSSDIndex 阶段线程数建议

**实测数据**：

| NumberOfThreads | 实际线程数 | BuildSSDIndex 耗时 |
|-----------------|-----------|-------------------|
| 14 | 16 (14+2辅助) | **94 秒** ✅ |
| 1 | 3 (1+2辅助) | **233 秒** |

**结论**：多线程能显著加速 `BuildSSDIndex` 阶段，`NumberOfThreads=14` 比 `NumberOfThreads=1` 快约 **2.5 倍**。

**建议配置**：
```ini
[BuildSSDIndex]
NumberOfThreads=14  # 根据CPU核心数调整，建议设置为核心数或略少
```

**线程数计算**：实际线程数 = NumberOfThreads + 2（主线程 + I/O 线程）

### B.4 搜索线程数配置

**注意**：搜索线程数由 `SearchThreadNum` 控制，**不是** `NumberOfThreads`。

**正确配置**：
```ini
[SearchSSDIndex]
SearchThreadNum=8        # 搜索线程数（有效）
NumberOfThreads=16       # 此参数在搜索阶段不生效
```

### B.5 HashTableExponent 参数影响

| 值 | 构建阶段 | 搜索阶段 |
|----|----------|----------|
| 4（默认） | 快 | 较慢 |
| 12 | **极慢（256x）** | 快 |

**建议**：
- 构建时使用默认值 4
- 搜索时使用 12 以提高性能

---

## 附录 C：常见问题

### Q1: 编译时找不到 zstd 目录

**原因**：git 子模块未初始化

**解决**：
```bash
git submodule update --init ThirdParty/zstd
```

### Q2: 搜索时段错误 (Segmentation Fault)

**原因**：配置文件中同时启用了构建和搜索，或缺少必要配置

**解决**：确保构建阶段 `isExecute=false`，并移除 `WarmupPath` 配置

### Q3: Recall 为 0

**原因**：Python 绑定不支持 SPANN 索引格式

**解决**：使用 `ssdserving` 工具进行搜索测试

### Q4: 如何调整召回率与速度的权衡

**方法**：
- 增加 `MaxCheck` 提高召回率（降低速度）
- 增加 `InternalResultNum` 提高召回率
- 增加 `ReplicaCount` 提高召回率（增加存储）

---

## 附录 D：参考资料

- [SPTAG GitHub](https://github.com/microsoft/SPTAG)
- [SPFresh 论文 (SOSP 2023)](https://dl.acm.org/doi/10.1145/3600006.3613166)
- [SPANN 论文 (NeurIPS 2021)](https://proceedings.neurips.cc/paper/2021/hash/513f7906d1b8476071935c0c2d4c3c44-Abstract.html)
