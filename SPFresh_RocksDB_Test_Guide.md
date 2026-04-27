# SPFresh + RocksDB 性能测试指南

本文档详细介绍如何使用 SIFT1M 数据集和 RocksDB 存储后端测试 SPFresh 算法性能。

## 一、概述

### 1.1 SPFresh 简介

SPFresh 是 SPTAG 项目中的**增量就地更新系统**，发表于 SOSP 2023。核心特点：
- 支持动态数据更新（插入/删除）
- 支持页分裂（Split）、路由重分配（Reassign）和垃圾回收（GC）
- 在数据动态演化过程中维持优异的检索性能

### 1.2 存储后端对比

| 存储类型 | 说明 | 性能 | 硬件要求 |
|----------|------|------|----------|
| `STATIC` | 静态索引，不可更新 | - | 无 |
| `SPDKIO` | SPDK 直接访问 NVMe | 最高 | 需要 NVMe + SPDK |
| `ROCKSDBIO` | RocksDB KV 存储 | 中等 | 无特殊要求 |
| `FILEIO` | 普通文件 I/O | 较低 | 无 |

### 1.3 SPFresh vs SPANN+ vs SPANN

| 系统 | 页分裂 | 路由重分配 | 垃圾回收 | 说明 |
|------|--------|-----------|---------|------|
| **SPFresh** | ✅ | ✅ | ✅ | 完整实现 |
| **SPANN+** | ❌ | ❌ | ❌ | 仅支持追加，倒排页无限膨胀 |
| **SPANN** | ❌ | ❌ | ❌ | 静态索引，不支持更新 |

---

## 二、环境准备

### 2.1 系统要求

- 操作系统：Linux (Ubuntu 20.04+ 推荐)
- 内存：8GB+ (SIFT1M 测试)
- 磁盘：10GB+ 可用空间
- CPU：多核处理器

### 2.2 安装依赖

```bash
# 基础依赖
sudo apt-get update
sudo apt-get install -y build-essential cmake git

# Boost 库
sudo apt-get install -y libboost-all-dev

# OpenMP (通常已包含在 gcc 中)
sudo apt-get install -y libomp-dev

# TBB (Intel Threading Building Blocks)
sudo apt-get install -y libtbb-dev
```

### 2.3 安装 RocksDB

**方法一：使用包管理器（推荐）**

```bash
sudo apt-get install -y librocksdb-dev
```

**方法二：从源码编译**

```bash
# 克隆 RocksDB
git clone https://github.com/facebook/rocksdb.git
cd rocksdb

# 编译共享库
make shared_lib -j$(nproc)

# 安装
sudo make install

# 更新动态链接库缓存
sudo ldconfig

cd ..
```

### 2.4 编译 SPTAG（启用 RocksDB）

```bash
# 克隆仓库（如果还没有）
git clone https://github.com/microsoft/SPTAG.git
cd SPTAG

# 创建构建目录
mkdir -p build
cd build

# 配置 CMake（关键：启用 RocksDB，禁用 SPDK）
cmake -DROCKSDB=ON -DSPDK=OFF ..

# 编译
make -j$(nproc)

# 编译产物在 ../Release/ 目录
ls ../Release/
# 应该看到: spfresh, ssdserving, usefultool 等可执行文件

cd ..
```

**验证 RocksDB 支持：**

```bash
# 检查 spfresh 是否链接了 RocksDB
ldd Release/spfresh | grep rocksdb
# 应该输出类似: librocksdb.so.8 => /usr/lib/x86_64-linux-gnu/librocksdb.so.8
```

---

## 三、数据准备

### 3.1 下载 SIFT1M 数据集

```bash
# 创建数据目录
DATA_DIR=/media/ray/1tb/sift1m
mkdir -p $DATA_DIR

# 下载 SIFT1M 数据集
cd $DATA_DIR

# 基础向量 (1,000,000 × 128 维)
wget ftp://ftp.irisa.fr/local/texmex/corpus/sift/sift_base.fvecs

# 查询向量 (10,000 × 128 维)
wget ftp://ftp.irisa.fr/local/texmex/corpus/sift/sift_query.fvecs

# Ground truth (100 最近邻)
wget ftp://ftp.irisa.fr/local/texmex/corpus/sift/sift_groundtruth.ivecs

# 验证文件
ls -lh *.fvecs *.ivecs
# sift_base.fvecs:     ~488 MB
# sift_query.fvecs:    ~4.9 MB
# sift_groundtruth.ivecs: ~40 MB
```

### 3.2 数据格式说明

SIFT1M 使用 `.fvecs` 格式（XVEC 格式）：

```
文件结构：
[维度: int32] [向量1: float × 维度]
[维度: int32] [向量2: float × 维度]
...
```

- `sift_base.fvecs`: 1,000,000 个 128 维 float 向量
- `sift_query.fvecs`: 10,000 个 128 维 float 向量
- `sift_groundtruth.ivecs`: 每个查询的 100 个最近邻 ID

### 3.3 创建索引目录

```bash
# 创建 SPFresh 索引存储目录
mkdir -p $DATA_DIR/spfresh_index
```

---

## 四、配置文件

### 4.1 创建索引构建配置

创建文件 `config/build_spann_sift1m_rocksdb.ini`：

```ini
[Index]
IndexAlgoType=SPANN
ValueType=Float

[Base]
ValueType=Float
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/media/ray/1tb/sift1m/sift_base.fvecs
VectorType=XVEC
VectorSize=1000000
IndexDirectory=/media/ray/1tb/sift1m/spfresh_index
HeadIndexFolder=head_index

[SelectHead]
isExecute=true
TreeNumber=1
BKTKmeansK=32
BKTLeafSize=8
SamplesNumber=1000
NumberOfThreads=8
SaveBKT=false
AnalyzeOnly=false
CalcStd=true
SelectDynamically=true
NoOutput=false
SelectThreshold=12
SplitFactor=9
SplitThreshold=18
Ratio=0.15
RecursiveCheckSmallCluster=true
PrintSizeCount=true

[BuildHead]
isExecute=true
TreeFilePath=tree.bin
GraphFilePath=graph.bin
VectorFilePath=vectors.bin
DeleteVectorFilePath=deletes.bin
EnableBfs=0
BKTNumber=1
BKTKmeansK=32
BKTLeafSize=8
Samples=1000
BKTLambdaFactor=100.000000
TPTNumber=32
TPTLeafSize=2000
NumTopDimensionTpTreeSplit=5
NeighborhoodSize=32
GraphNeighborhoodScale=2.000000
GraphCEFScale=2.000000
RefineIterations=2
EnableRebuild=0
CEF=1000
AddCEF=500
MaxCheckForRefineGraph=8192
RNGFactor=1.000000
NumberOfThreads=8
DistCalcMethod=L2
DeletePercentageForRefine=0.400000
AddCountForRebuild=1000
MaxCheck=4096
ThresholdOfNumberOfContinuousNoBetterPropagation=3
NumberOfInitialDynamicPivots=50
NumberOfOtherDynamicPivots=4
HashTableExponent=2
DataBlockSize=1048576
DataCapacity=2147483647
MetaRecordSize=10

[BuildSSDIndex]
isExecute=true
BuildSsdIndex=true
NumberOfThreads=8
InternalResultNum=64
ReplicaCount=8
PostingPageLimit=4
OutputEmptyReplicaID=1
TmpDir=/media/ray/1tb/sift1m/spfresh_index/tmpdir

# ====== RocksDB 配置 ======
UseSPDK=false
Storage=ROCKSDBIO
KVFile=rocksdb
ExcludeHead=false
UseDirectIO=false
SpdkBatchSize=64

# 搜索参数
ResultNum=10
SearchInternalResultNum=64
SearchThreadNum=2
SearchTimes=1

# 更新参数
Update=false
SteadyState=false
Days=0
InsertThreadNum=2
AppendThreadNum=1
ReassignThreadNum=0
FullVectorPath=/media/ray/1tb/sift1m/sift_base.fvecs
DisableReassign=false
ReassignK=64
LatencyLimit=100.0
CalTruth=false
SearchPostingPageLimit=4
MaxDistRatio=1000000
SearchDuringUpdate=false
DeleteQPS=1000
MergeThreshold=10
Sampling=2
ShowUpdateProgress=true
EndVectorNum=1000000
BufferLength=6
```

### 4.2 创建 SPFresh 运行配置

创建文件 `config/spfresh_sift1m_rocksdb.ini`：

```ini
[Index]
IndexAlgoType=SPANN
ValueType=Float

[Base]
ValueType=Float
DistCalcMethod=L2
IndexAlgoType=BKT
Dim=128
VectorPath=/media/ray/1tb/sift1m/sift_base.fvecs
VectorType=XVEC
VectorSize=1000000
VectorDelimiter=
QueryPath=/media/ray/1tb/sift1m/sift_query.fvecs
QueryType=XVEC
QuerySize=10000
QueryDelimiter=
WarmupPath=
WarmupType=XVEC
WarmupSize=10000
WarmupDelimiter=
TruthPath=/media/ray/1tb/sift1m/sift_groundtruth.ivecs
TruthType=XVEC
GenerateTruth=false
IndexDirectory=/media/ray/1tb/sift1m/spfresh_index
HeadIndexFolder=head_index

[SelectHead]
isExecute=false
TreeNumber=1
BKTKmeansK=32
BKTLeafSize=8
SamplesNumber=1000
NumberOfThreads=8
SaveBKT=false
AnalyzeOnly=false
CalcStd=true
SelectDynamically=true
NoOutput=false
SelectThreshold=12
SplitFactor=9
SplitThreshold=18
Ratio=0.15
RecursiveCheckSmallCluster=true
PrintSizeCount=true

[BuildHead]
isExecute=false
TreeFilePath=tree.bin
GraphFilePath=graph.bin
VectorFilePath=vectors.bin
DeleteVectorFilePath=deletes.bin
EnableBfs=0
BKTNumber=1
BKTKmeansK=32
BKTLeafSize=8
Samples=1000
BKTLambdaFactor=100.000000
TPTNumber=32
TPTLeafSize=2000
NumTopDimensionTpTreeSplit=5
NeighborhoodSize=32
GraphNeighborhoodScale=2.000000
GraphCEFScale=2.000000
RefineIterations=2
EnableRebuild=0
CEF=1000
AddCEF=500
MaxCheckForRefineGraph=8192
RNGFactor=1.000000
NumberOfThreads=8
DistCalcMethod=L2
DeletePercentageForRefine=0.400000
AddCountForRebuild=1000
MaxCheck=4096
ThresholdOfNumberOfContinuousNoBetterPropagation=3
NumberOfInitialDynamicPivots=50
NumberOfOtherDynamicPivots=4
HashTableExponent=2
DataBlockSize=1048576
DataCapacity=2147483647
MetaRecordSize=10

[BuildSSDIndex]
isExecute=true
BuildSsdIndex=false
NumberOfThreads=8
InternalResultNum=64
ReplicaCount=8
PostingPageLimit=4
OutputEmptyReplicaID=1
TmpDir=/media/ray/1tb/sift1m/spfresh_index/tmpdir

# ====== RocksDB 配置 ======
UseSPDK=false
Storage=ROCKSDBIO
KVFile=rocksdb
ExcludeHead=false
UseDirectIO=false
SpdkBatchSize=64

# 搜索参数
ResultNum=10
SearchInternalResultNum=64
SearchThreadNum=2
SearchTimes=1

# ====== 更新配置 ======
Update=true
SteadyState=true
Days=1
InsertThreadNum=2
AppendThreadNum=1
ReassignThreadNum=0

# 数据路径
FullVectorPath=/media/ray/1tb/sift1m/sift_base.fvecs
DisableReassign=false
ReassignK=64
LatencyLimit=100.0
CalTruth=true
SearchPostingPageLimit=4
MaxDistRatio=1000000
SearchDuringUpdate=true
UpdateFilePrefix=/media/ray/1tb/sift1m/update_trace_
TruthFilePrefix=/media/ray/1tb/sift1m/truth_after_
DeleteQPS=1000
MergeThreshold=10
Sampling=4
ShowUpdateProgress=true
EndVectorNum=1000000
SearchResult=/media/ray/1tb/sift1m/search_result
BufferLength=6
```

---

## 五、构建初始索引

### 5.1 执行索引构建

```bash
cd /home/ray/code/SPTAG

# 确保配置目录存在
mkdir -p config

# 执行构建
./Release/ssdserving config/build_spann_sift1m_rocksdb.ini
```

### 5.2 构建过程输出

预期输出：

```
Start SelectHead...
...
Finish SelectHead, select 31250 head vectors.

Start BuildHead...
Building BKT tree...
Building RNG graph...
Refine RNG graph iteration 1...
Refine RNG graph iteration 2...
Finish BuildHead.

Start BuildSSDIndex...
Building SSD index with RocksDB...
Finish BuildSSDIndex.

Total build time: XXX seconds.
```

### 5.3 验证索引文件

```bash
# 检查索引目录
ls -la /media/ray/1tb/sift1m/spfresh_index/

# 应该看到以下文件/目录：
# head_index/        - 头索引（BKT 树 + RNG 图）
# rocksdb/           - RocksDB 数据目录
# ssdmapping.bin     - SSD 映射文件
# metadata.bin       - 元数据文件
# indexloader.ini    - 配置文件（需要复制）
```

---

## 六、生成更新追踪文件

SPFresh 需要更新追踪文件来模拟动态更新操作。

### 6.1 使用 usefultool 生成追踪

```bash
cd /home/ray/code/SPTAG

# 生成更新追踪文件
# 参数说明：
#   --UpdateSize: 每批次更新数量
#   --BaseNum: 基础向量数量
#   --Batch: 批次编号（从 0 开始）

./Release/usefultool -GenTrace true \
    --vectortype Float \
    --VectorPath /media/ray/1tb/sift1m/sift_base.fvecs \
    --filetype XVEC \
    --UpdateSize 100000 \
    --BaseNum 1000000 \
    --ReserveNum 1000000 \
    --CurrentListFileName /media/ray/1tb/sift1m/current_list \
    --ReserveListFileName /media/ray/1tb/sift1m/reserve_list \
    --TraceFileName /media/ray/1tb/sift1m/update_trace \
    -d 128 \
    --Batch 0 \
    -f XVEC
```

### 6.2 追踪文件格式

生成的 `update_trace0` 文件结构：

```
[更新数量: int32]
[删除向量ID列表: int32 × 更新数量]
[插入向量ID列表: int32 × 更新数量]
```

### 6.3 批量生成多天追踪（可选）

如果需要模拟多天更新（`Days > 1`）：

```bash
# 生成 10 天的更新追踪
for i in {0..9}; do
    ./Release/usefultool -GenTrace true \
        --vectortype Float \
        --VectorPath /media/ray/1tb/sift1m/sift_base.fvecs \
        --filetype XVEC \
        --UpdateSize 100000 \
        --BaseNum 1000000 \
        --ReserveNum 1000000 \
        --TraceFileName /media/ray/1tb/sift1m/update_trace \
        -d 128 \
        --Batch $i \
        -f XVEC
done
```

---

## 七、运行 SPFresh 测试

### 7.1 准备配置文件

```bash
# 复制运行配置到索引目录
cp config/spfresh_sift1m_rocksdb.ini /media/ray/1tb/sift1m/spfresh_index/indexloader.ini
```

### 7.2 执行 SPFresh

```bash
cd /home/ray/code/SPTAG

# 运行 SPFresh
./Release/spfresh /media/ray/1tb/sift1m/spfresh_index
```

### 7.3 测试流程

SPFresh 执行流程：

```
1. 加载索引
   └─ 加载头索引 (BKT + RNG)
   └─ 打开 RocksDB

2. 初始搜索测试
   └─ 执行搜索，记录初始 QPS 和 Recall

3. 动态更新循环 (Days 次)
   ├─ 加载更新追踪文件
   ├─ 并行执行：
   │  ├─ 插入线程：插入新向量
   │  ├─ 删除线程：删除旧向量
   │  └─ 搜索线程：持续搜索测试
   ├─ 等待所有更新完成
   ├─ 执行 Checkpoint
   └─ 搜索测试，记录 QPS 和 Recall

4. 输出统计结果
```

---

## 八、输出结果解读

### 8.1 典型输出示例

```
Start loading index...
ExtraDynamicSearcher:UseKV
SPFresh: New Rocksdb: /media/ray/1tb/sift1m/spfresh_index/rocksdb
Load VectorSet(1000000,128).

Initial Search:
Searching: numThread: 2, numQueries: 10000, searchTimes: 1.
Current time: 5, Searching Times: 1, AvgQPS: 4523.67.

Ex Latency Distribution:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles  Max
0.412   0.356    0.723    0.856    1.234    2.156     3.456

Recall10@10: 0.9523

========================================
Updating: numThread: 2, total days: 1.
Loading update_trace0
Update Size: 100000
========================================

Insert: Finish sending in 45.234 seconds, sending throughput is 2210.45.
Insert: Finish syncing in 52.678 seconds, actuall throughput is 1898.23.

Insert Latency Distribution:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles  Max
15.23   10.56    32.45    45.67    78.90    125.34    234.56

Delete: Finish sending in 50.123 seconds.

Current time: 60, Total Vector Num: 1000000.
Recall10@10: 0.9487

========================================
Final Statistics
========================================
Total update time: 55.4 seconds
Final QPS: 4312.89
Final Recall: 0.9487
```

### 8.2 关键指标说明

| 指标 | 说明 | 理想值 |
|------|------|--------|
| **AvgQPS** | 平均查询吞吐量 | 越高越好 |
| **Recall@K** | 召回率 | > 0.95 |
| **Ex Latency** | 磁盘索引搜索延迟 | < 1ms |
| **Insert Latency** | 插入延迟 | 取决于负载 |
| **Total Vector Num** | 当前向量总数 | 应正确更新 |

---

## 九、关键配置参数详解

### 9.1 存储配置

| 参数 | 说明 | 推荐值 |
|------|------|--------|
| `Storage` | 存储后端类型 | `ROCKSDBIO` |
| `KVFile` | RocksDB 数据目录名 | `rocksdb` |
| `UseSPDK` | 是否使用 SPDK | `false` |
| `UseDirectIO` | 是否使用 Direct I/O | `false` |

### 9.2 索引结构配置

| 参数 | 说明 | 推荐值 |
|------|------|--------|
| `PostingPageLimit` | 每个 posting 页的向量数限制 | `4` (每页 4×4096=16KB) |
| `ReplicaCount` | 每个向量的副本数 | `8` |
| `InternalResultNum` | 内部搜索结果数 | `64` |

### 9.3 更新配置

| 参数 | 说明 | 推荐值 |
|------|------|--------|
| `Update` | 是否启用更新 | `true` |
| `SteadyState` | 稳态测试模式 | `true` |
| `Days` | 模拟更新天数 | `1-10` |
| `InsertThreadNum` | 插入线程数 | `2-8` |
| `AppendThreadNum` | 追加线程数 | `1-4` |
| `ReassignThreadNum` | 重分配线程数 | `0` (自动) |
| `DisableReassign` | 禁用路由重分配 | `false` |
| `DeleteQPS` | 删除操作 QPS 限制 | `1000` |

### 9.4 搜索配置

| 参数 | 说明 | 推荐值 |
|------|------|--------|
| `SearchThreadNum` | 搜索线程数 | `2` |
| `SearchTimes` | 每轮搜索次数 | `1` |
| `ResultNum` | 返回结果数 | `10` |
| `SearchDuringUpdate` | 更新时是否搜索 | `true` |

---

## 十、常见问题与解决方案

### 10.1 编译问题

**问题：找不到 RocksDB**

```
CMake Error: Could not find RocksDB!
```

**解决方案：**

```bash
# 确认 RocksDB 已安装
ldconfig -p | grep rocksdb

# 如果没有，安装 RocksDB
sudo apt-get install librocksdb-dev

# 或从源码编译后安装
```

**问题：链接错误**

```
undefined reference to `rocksdb::DB::Open...'
```

**解决方案：**

```bash
# 更新动态链接库缓存
sudo ldconfig

# 重新编译
cd build && make clean && make -j$(nproc)
```

### 10.2 运行问题

**问题：无法加载索引**

```
Failed to load index.
```

**解决方案：**

```bash
# 检查索引目录结构
ls -la /media/ray/1tb/sift1m/spfresh_index/

# 确保包含以下文件：
# - head_index/ 目录
# - rocksdb/ 目录
# - indexloader.ini 配置文件
```

**问题：RocksDB 打开失败**

```
Rocksdb Open Error: IO error...
```

**解决方案：**

```bash
# 检查目录权限
chmod -R 755 /media/ray/1tb/sift1m/spfresh_index/

# 检查磁盘空间
df -h /media/ray/1tb/
```

**问题：找不到更新追踪文件**

```
Failed open trace file: update_trace0
```

**解决方案：**

```bash
# 确认追踪文件存在
ls -la /media/ray/1tb/sift1m/update_trace*

# 检查配置文件中的路径
grep UpdateFilePrefix /media/ray/1tb/sift1m/spfresh_index/indexloader.ini
```

### 10.3 性能问题

**问题：QPS 过低**

可能原因：
1. 磁盘 I/O 瓶颈 - 使用 SSD 而非 HDD
2. 线程数不足 - 增加 `SearchThreadNum`
3. RocksDB 配置不当 - 调整 RocksDB 参数

**问题：内存占用过高**

解决方案：
- 减小 `PostingPageLimit`
- 设置 `LoadAllVectors=false`

---

## 十一、性能对比参考

### 11.1 SIFT1M 测试参考值

| 指标 | SPDK | RocksDB | 文件系统 |
|------|------|---------|---------|
| 初始 QPS | ~5000 | ~4500 | ~3000 |
| 更新后 QPS | ~4800 | ~4300 | ~2800 |
| Recall@10 | ~0.95 | ~0.95 | ~0.95 |
| 插入延迟 (avg) | ~10ms | ~15ms | ~25ms |

### 11.2 不同规模测试

| 数据集 | 向量数 | 维度 | 内存需求 | 磁盘需求 |
|--------|--------|------|---------|---------|
| SIFT1M | 1M | 128 | ~2GB | ~5GB |
| SIFT10M | 10M | 128 | ~8GB | ~50GB |
| SIFT100M | 100M | 128 | ~40GB | ~500GB |

---

## 十二、附录

### 12.1 完整测试脚本

创建 `scripts/test_spfresh_sift1m.sh`：

```bash
#!/bin/bash
# SPFresh + RocksDB 测试脚本 (SIFT1M)

set -e

# 配置路径
SPTAG_ROOT=/home/ray/code/SPTAG
DATA_DIR=/media/ray/1tb/sift1m
INDEX_DIR=$DATA_DIR/spfresh_index

echo "=========================================="
echo "SPFresh + RocksDB 测试 (SIFT1M)"
echo "=========================================="

# 1. 检查数据文件
echo "[1] 检查数据文件..."
if [ ! -f "$DATA_DIR/sift_base.fvecs" ]; then
    echo "错误: 找不到 sift_base.fvecs"
    exit 1
fi
echo "    数据文件 OK"

# 2. 构建索引
echo ""
echo "[2] 构建索引..."
if [ ! -d "$INDEX_DIR/rocksdb" ]; then
    mkdir -p $INDEX_DIR
    $SPTAG_ROOT/Release/ssdserving $SPTAG_ROOT/config/build_spann_sift1m_rocksdb.ini
    echo "    索引构建完成"
else
    echo "    索引已存在，跳过构建"
fi

# 3. 生成更新追踪
echo ""
echo "[3] 生成更新追踪..."
if [ ! -f "$DATA_DIR/update_trace0" ]; then
    $SPTAG_ROOT/Release/usefultool -GenTrace true \
        --vectortype Float \
        --VectorPath $DATA_DIR/sift_base.fvecs \
        --filetype XVEC \
        --UpdateSize 100000 \
        --BaseNum 1000000 \
        --ReserveNum 1000000 \
        --TraceFileName $DATA_DIR/update_trace \
        -d 128 --Batch 0 -f XVEC
    echo "    追踪文件生成完成"
else
    echo "    追踪文件已存在，跳过生成"
fi

# 4. 运行 SPFresh
echo ""
echo "[4] 运行 SPFresh..."
cp $SPTAG_ROOT/config/spfresh_sift1m_rocksdb.ini $INDEX_DIR/indexloader.ini
$SPTAG_ROOT/Release/spfresh $INDEX_DIR

echo ""
echo "=========================================="
echo "测试完成！"
echo "=========================================="
```

### 12.2 相关文档

- [SPTAG 主文档](README.md)
- [参数配置文档](docs/Parameters.md)
- [SPFresh 论文](https://dl.acm.org/doi/10.1145/3609553.3609587)

---

**文档版本**: 1.0  
**最后更新**: 2026-04-21
