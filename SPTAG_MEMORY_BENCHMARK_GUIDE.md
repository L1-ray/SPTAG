# SPTAG 内存索引性能测试指南

本文档记录 SPTAG 内存索引（BKT 算法）的性能测试方法、结果和分析。

## 一、测试环境

| 配置项 | 值 |
|--------|-----|
| CPU | 16 核心 |
| 操作系统 | Ubuntu 24.04.4 LTS |
| 内核版本 | 6.17.0-20-generic |
| 测试数据集 | SIFT1M (100万 128维向量) |

## 二、测试框架

### 2.1 文件结构

```
SPTAG/
├── config/
│   └── bkt_build.ini              # BKT 索引构建配置
├── scripts/
│   ├── sptag_memory_monitor.sh    # 内存索引监控脚本
│   ├── test_bkt_build.sh          # 构建测试脚本
│   ├── test_bkt_search.sh         # 搜索测试脚本
│   ├── run_benchmark.sh           # 完整测试流程脚本
│   └── convert_groundtruth.py     # Ground truth 格式转换
└── results/
    ├── build/                     # 构建测试结果
    └── search/                    # 搜索测试结果
```

### 2.2 与 SPANN 磁盘索引的区别

| 特性 | SPANN (磁盘索引) | SPTAG 内存索引 |
|------|-----------------|----------------|
| 可执行文件 | `ssdserving` | `indexbuilder` + `indexsearcher` |
| 工作流 | 单程序多阶段 | 构建和搜索分离 |
| 阶段 | SelectHead → BuildHead → BuildSSDIndex → SearchSSDIndex | BuildTree → BuildGraph → Search |
| 数据存储 | SSD 磁盘 | 内存 |
| 配置方式 | 单个 .ini 文件 | 构建配置 + 命令行参数 |

## 三、使用方法

### 3.1 构建索引

```bash
# 单线程构建
./scripts/test_bkt_build.sh -t 8

# 多线程对比测试
./scripts/test_bkt_build.sh -t 8 -t 16

# 清除缓存后构建
./scripts/test_bkt_build.sh -t 8 -C
```

### 3.2 搜索测试

```bash
# 单个 MaxCheck 测试
./scripts/test_bkt_search.sh -x /media/ray/1tb/sift1m/bkt_memory_index_8t -m 8192 -t 8

# 多个 MaxCheck 对比测试
./scripts/test_bkt_search.sh -x /media/ray/1tb/sift1m/bkt_memory_index_8t -m 1024 -m 2048 -m 4096 -m 8192 -t 8

# 指定 KNN 数量
./scripts/test_bkt_search.sh -x /media/ray/1tb/sift1m/bkt_memory_index_8t -m 8192 -k 10 -t 8
```

### 3.3 完整测试流程

```bash
# 构建并搜索
./scripts/run_benchmark.sh -t 8 -m 8192

# 仅构建
./scripts/run_benchmark.sh --build-only -t 16

# 仅搜索
./scripts/run_benchmark.sh --search-only -m 8192 -x /path/to/index
```

### 3.4 Ground Truth 格式转换

SPTAG 要求 ground truth 文件使用特定格式。SIFT1M 原始数据使用 ivecs 格式，需要转换：

```bash
# 转换为 SPTAG DEFAULT 格式（推荐）
python3 scripts/convert_groundtruth.py \
    /media/ray/1tb/sift1m/sift_groundtruth.ivecs \
    /media/ray/1tb/sift1m/sift_groundtruth_sptag \
    --format default --k 100

# 支持的格式
# --format default: SPTAG DEFAULT 格式 (文件名后缀 .bin)
# --format xvec:    XVEC 格式
# --format txt:     TXT 格式（空格分隔）
```

## 四、配置文件说明

### 4.1 构建配置 (config/bkt_build.ini)

```ini
[Base]
ValueType=Float              # 向量值类型: Float, Int8, Int16
DistCalcMethod=L2            # 距离度量: L2, Cosine
IndexAlgoType=BKT            # 索引算法: BKT, KDT
Dim=128                      # 向量维度
VectorPath=/media/ray/1tb/sift1m/sift_base.fvecs
IndexDirectory=/media/ray/1tb/sift1m/bkt_memory_index

[Index]
; BKT树参数
TreeNumber=1                 # BKT 树数量
BKTKmeansK=32                # K-means 聚类中心数
BKTLeafSize=8                # 叶子节点大小
Samples=1000                 # 初始化采样数

; 图构建参数
NeighborhoodSize=32          # 邻居大小
TPTNumber=8                  # TPT 树数量（用于初始化 KNN 图）
TPTLeafSize=2000             # TPT 树叶子大小
MaxCheckForRefineGraph=512   # 图优化最大检查数
RefineIterations=1           # 图优化迭代次数

; 线程数（可通过命令行覆盖）
NumberOfThreads=16
```

**注意**：
1. SPTAG INI 文件使用 `;` 作为注释符号，不支持 `#`
2. 配置参数需要放在 `[Index]` section，不是 `[IndexBuilder]`
3. 可通过命令行参数覆盖：`Index.NumberOfThreads=8`

## 五、测试结果

### 5.1 构建性能（SIFT1M, 8线程）

| 指标 | 值 |
|------|-----|
| 总运行时间 | 659.6 秒 (~11分钟) |
| BuildTree 阶段 | 52.9s |
| BuildGraph 阶段 | ~600s |
| CPU 使用率（平均） | 623.2% |
| CPU 使用率（峰值） | 749% |
| 内存占用（平均） | 789.6 MB |
| 内存占用（峰值） | 1030.3 MB |

#### 生成的索引文件

| 文件 | 大小 | 说明 |
|------|------|------|
| vectors.bin | 512 MB | 原始向量数据 |
| graph.bin | 128 MB | RNG 图结构 |
| tree.bin | 12 MB | BKT 树结构 |
| deletes.bin | 1 MB | 删除标记 |
| indexloader.ini | 1 KB | 索引配置 |

### 5.2 搜索性能（SIFT1M, 8线程）

| MaxCheck | Recall@32 | QPS | 平均延迟 | P99 延迟 | P95 延迟 |
|----------|-----------|-----|----------|----------|----------|
| 1024 | 92.07% | 32,818 | 0.2ms | 0.3ms | 0.3ms |
| 2048 | 97.59% | 17,295 | 0.5ms | 1.0ms | 0.7ms |
| 4096 | 99.43% | 10,062 | 0.8ms | 1.0ms | 0.9ms |
| 8192 | 99.85% | 5,149 | 1.6ms | 2.5ms | 1.9ms |

### 5.3 MaxCheck 与性能权衡分析

```
Recall vs QPS 权衡曲线：

Recall
  │
100%├─────────────────────────────────────────────
    │                                    ● 8192 (99.85%, 5149 QPS)
 99%├───────────────────────────────●───
    │                          4096 (99.43%, 10062 QPS)
 98%├───────────────────────●───────
    │                  2048 (97.59%, 17295 QPS)
 95%├───────────────●───────────────
    │          1024 (92.06%, 32818 QPS)
 90%├─────────────────────────────────────────────
    └─────────────────────────────────────────────► QPS
        0     5000    10000   15000   20000   25000
```

**关键观察**：
- MaxCheck=4096 提供了良好的召回率(99.43%)和较高的 QPS(10062)
- MaxCheck=8192 召回率接近完美(99.85%)，但 QPS 下降约 50%
- 对于延迟敏感场景，MaxCheck=2048 可达到 97.59% 召回率，延迟仅 0.5ms

## 六、与 SPANN 磁盘索引对比

### 6.1 构建时间对比

| 索引类型 | 总时间 | 数据存储 |
|----------|--------|----------|
| SPANN (磁盘) | ~788s | SSD |
| BKT (内存) | ~660s | 内存 |

### 6.2 搜索性能对比

| 索引类型 | Recall@10 | QPS | 延迟 |
|----------|-----------|-----|------|
| SPANN (2线程, MaxCheck=2048) | 94.03% | 83 | 24ms |
| BKT 内存 (8线程, MaxCheck=8192) | 99.85% | 5,149 | 1.6ms |

**结论**：内存索引在搜索性能上显著优于磁盘索引，适合对延迟敏感的应用场景。

## 七、注意事项

### 7.1 Ground Truth 格式

SPTAG 支持三种 ground truth 格式，根据文件名后缀自动识别：

| 后缀 | 格式 | 说明 |
|------|------|------|
| `.bin` | DEFAULT | 文件头: [query_count][K], 然后是数据和距离 |
| `.xvec` | XVEC | 每个向量: [K][id1]...[idK] |
| `.txt` | TXT | 每行: id1 id2 ... idK（空格分隔） |

SIFT1M 原始 ground truth 文件 (`sift_groundtruth.ivecs`) 使用 XVEC 格式，但 SPTAG 根据文件名判断类型。推荐使用转换脚本生成 `.bin` 后缀的 DEFAULT 格式文件。

### 7.2 配置文件注释

SPTAG 的 INI 解析器只支持 `;` 作为注释符号，不支持 `#`。使用 `#` 会导致配置解析失败。

### 7.3 线程数设置

- 构建时通过 `Index.NumberOfThreads` 参数控制
- 搜索时通过 `-t` 命令行参数控制
- 线程数建议设置为物理核心数，过多线程可能导致性能下降

## 八、搜索效率分析

### 8.1 MaxCheck 与检索效率

MaxCheck 是 BKT 搜索时**最多检查的向量数**，直接影响召回率和搜索速度。

| MaxCheck | Recall@32 | 检索向量数 | 检索比例 | QPS | 延迟 |
|----------|-----------|-----------|---------|-----|------|
| 1024 | 92.06% | ~1,024 | 0.10% | 32,818 | 0.2ms |
| 2048 | 97.59% | ~2,048 | 0.20% | 17,295 | 0.5ms |
| 4096 | 99.43% | ~4,096 | 0.41% | 10,062 | 0.8ms |
| 8192 | 99.85% | ~8,192 | 0.82% | 5,149 | 1.6ms |

**核心发现**：用不到 1% 的计算量，获得 99%+ 的召回率！

### 8.2 效率对比

| 方式 | 检索向量数 | 召回率 | 计算量 |
|------|-----------|--------|--------|
| 暴力搜索 | 1,000,000 | 100% | 100% |
| BKT (MaxCheck=8192) | ~8,192 | 99.85% | 0.82% |
| **效率提升** | - | - | **122x** |

### 8.3 召回率曲线

```
召回率 vs 检索比例

100%│                                    ● 8192 (0.8%, 99.85%)
    │                              ●─────
 99%│                        ●─────
    │                  ●───── 4096 (0.4%, 99.43%)
 98%│            ●─────
    │      ●───── 2048 (0.2%, 97.59%)
 95%│──────●
    │  1024 (0.1%, 92.06%)
 90%│
    └──────────────────────────────────────────► 检索比例
        0.1%    0.2%    0.4%    0.8%
```

### 8.4 推荐配置

| 场景 | 推荐 MaxCheck | 召回率 | QPS |
|------|--------------|--------|-----|
| 低延迟优先 | 1024 | 92.06% | 32,818 |
| 平衡场景 | 2048 | 97.59% | 17,295 |
| 高召回场景 | 4096 | 99.43% | 10,062 |
| 接近完美召回 | 8192 | 99.85% | 5,149 |

## 九、监控脚本输出

### 9.1 CSV 输出格式

```
timestamp,elapsed_time,stage,cpu_percent,mem_rss_mb,mem_vsz_mb,process_status,threads,disk_read_bytes,disk_write_bytes,disk_read_rate_mbs,disk_write_rate_mbs
```

### 9.2 日志输出示例

```
========================================
实验统计摘要
========================================
结束时间: 2026-04-14 10:43:16
总运行时间: 6.0 秒
采样次数: 6

CPU 使用率: 平均 216.1%, 峰值 515.00%
内存占用 (RSS): 平均 528.1 MB, 峰值 689.39 MB
内存占用 (VSZ): 峰值 1269.76 MB

磁盘 I/O:
  累计读取: 631 MB
  累计写入: 0 MB

阶段耗时:
  Search: 3.3s
```

## 十、搜索参数配置详解

### 10.1 参数指定方式

参数来源优先级（从高到低）：

```
命令行参数 > 索引配置文件(indexloader.ini) > 程序默认值
```

### 10.2 命令行参数

| 参数 | 短选项 | 长选项 | 默认值 | 说明 |
|------|--------|--------|--------|------|
| MaxCheck | `-m` | `--maxcheck` | 8192 | 每次查询最多检查的向量数，多个值用 `#` 分隔 |
| KNN | `-k` | `--KNN` | 32 | 返回的最近邻数量 |
| Threads | `-t` | `--threads` | 32 | 搜索线程数 |
| QueryFile | `-i` | `--input` | - | 查询向量文件 |
| IndexFolder | `-x` | `--index` | - | 索引目录 |
| TruthFile | `-r` | `--truth` | - | Ground truth 文件 |

### 10.3 命令行示例

```bash
# 单个 MaxCheck 测试
./Release/indexsearcher \
    -x /media/ray/1tb/sift1m/bkt_memory_index \
    -i /media/ray/1tb/sift1m/sift_query.fvecs \
    -r /media/ray/1tb/sift1m/sift_groundtruth_sptag.bin \
    -m 8192 \
    -k 32 \
    -t 8

# 多个 MaxCheck 对比测试（用 # 分隔）
./Release/indexsearcher \
    -x /media/ray/1tb/sift1m/bkt_memory_index \
    -i /media/ray/1tb/sift1m/sift_query.fvecs \
    -r /media/ray/1tb/sift1m/sift_groundtruth_sptag.bin \
    -m "1024#2048#4096#8192" \
    -k 32 \
    -t 8

# 使用 KEY=VALUE 格式覆盖任意参数
./Release/indexsearcher \
    -x /media/ray/1tb/sift1m/bkt_memory_index \
    -i /media/ray/1tb/sift1m/sift_query.fvecs \
    "Index.MaxCheck=4096" \
    "Index.NumberOfThreads=16"
```

### 10.4 索引配置文件

构建索引时自动生成 `indexloader.ini`，保存在索引目录：

```ini
[Index]
IndexAlgoType=BKT
ValueType=Float
TreeFilePath=tree.bin
GraphFilePath=graph.bin
VectorFilePath=vectors.bin
DeleteVectorFilePath=deletes.bin
BKTNumber=1
BKTKmeansK=32
BKTLeafSize=8
NeighborhoodSize=32
NumberOfThreads=8
DistCalcMethod=L2
MaxCheck=8192
...
```

### 10.5 参数传递流程

```
搜索启动
    │
    ▼
┌─────────────────────────────────────────────────────┐
│ 1. 加载 indexloader.ini（从索引目录）               │
│    └─ MaxCheck=8192, NumberOfThreads=8, ...         │
├─────────────────────────────────────────────────────┤
│ 2. 解析命令行参数                                    │
│    └─ -m "1024#2048#8192" → [1024,2048,8192]        │
│    └─ -t 8 → m_threadNum = 8                        │
│    └─ "Index.XXX=YYY" → 覆盖配置文件值              │
├─────────────────────────────────────────────────────┤
│ 3. 应用参数到索引                                    │
│    └─ vecIndex->SetParameter("NumberOfThreads", "8")│
│    └─ vecIndex->UpdateIndex()                       │
├─────────────────────────────────────────────────────┤
│ 4. 搜索循环（可动态修改）                            │
│    for mc in [1024, 2048, 4096, 8192]:              │
│        index.SetParameter("MaxCheck", mc)           │
│        index.SearchIndex(results)                   │
└─────────────────────────────────────────────────────┘
```

### 10.6 构建参数与搜索参数对比

| 阶段 | 配置文件 | 主要参数 |
|------|----------|----------|
| 构建 | `config/bkt_build.ini` | TreeNumber, BKTKmeansK, NeighborhoodSize, NumberOfThreads |
| 搜索 | `indexloader.ini`（自动生成） | MaxCheck, NumberOfThreads |

## 十一、BKT 内存索引 vs SPANN 磁盘索引

### 11.1 架构对比

| 特性 | BKT 内存索引 | SPANN 磁盘索引 |
|------|-------------|---------------|
| 数据存储 | 内存 | SSD 磁盘 |
| 构建程序 | `indexbuilder` | `ssdserving` |
| 搜索程序 | `indexsearcher` | `ssdserving` |
| 可执行文件 | 构建和搜索分离 | 单程序多阶段 |

### 11.2 数据访问方式

**BKT 内存索引：**
```
搜索启动:
  Load Vector (1000000,128)   ← 一次性加载 512MB 到内存
  Load BKT tree               ← 加载树结构
  Load RNG graph              ← 加载图结构

搜索过程:
  全程在内存中操作，无磁盘 I/O
```

**SPANN 磁盘索引：**
```
搜索启动:
  Load index metadata         ← 只加载元数据

搜索过程:
  每次查询 → 访问磁盘读取 posting list
  延迟 = 磁盘寻道 + 读取时间
```

### 11.3 性能对比

| 索引类型 | Recall@10 | QPS | 延迟 | 数据规模 |
|----------|-----------|-----|------|---------|
| BKT 内存 (MaxCheck=8192) | 99.85% | 5,149 | 1.6ms | 百万级 |
| BKT 内存 (MaxCheck=1024) | 92.06% | 32,818 | 0.2ms | 百万级 |
| SPANN 磁盘 (MaxCheck=2048) | 94.03% | 83 | 24ms | 十亿级 |

### 11.4 内存索引的使用模式

SPTAG 支持两种使用模式：

| 模式 | 流程 | 适用场景 |
|------|------|---------|
| 离线模式 | Build → Save → Load → Search | 生产环境、持久化 |
| 在线模式 | Build → Search（同一进程） | 实时构建、临时使用 |

**在线模式示例（Python）：**
```python
import SPTAG

# 构建索引（纯内存）
index = SPTAG.AnnIndex('BKT', 'Float', 128)
index.SetBuildParam("NumberOfThreads", "8", "Index")
index.Build(vectors, num_vectors, False)

# 直接搜索（无需保存到磁盘）
results = index.Search(query_vector, k=10)

# 可选：保存以便以后使用
index.Save('index_folder')
```

### 11.5 在线模式性能测试

测试脚本：`scripts/test_online_mode.py`

#### 测试结果对比

| 指标 | 在线模式（单个 Search） | 在线模式（BatchSearch） | 离线模式（indexsearcher） |
|------|------------------------|------------------------|--------------------------|
| 构建时间 | ~545s | ~545s | ~660s |
| 搜索 QPS | 958 | 4,158 | 5,149 |
| Recall@32 | 99.84% | 99.85% | 99.85% |

#### QPS 差距分析

1. **搜索方式差异**
   - 在线模式单个 Search：Python 循环逐个查询，单线程
   - 在线模式 BatchSearch：C++ 内部多线程（`hardware_concurrency()`）
   - 离线模式 indexsearcher：C++ 多线程池（用户指定线程数）

2. **BatchSearch 内部实现**
   ```cpp
   // VectorIndex.cpp:558-588
   int maxthreads = std::thread::hardware_concurrency();
   for (int tid = 0; tid < maxthreads; tid++) {
       mythreads.emplace_back([&]() {
           while (true) {
               i = sent.fetch_add(1);
               if (i < p_vectorCount)
                   SearchIndex(res);  // 并行搜索
               else return;
           }
       });
   }
   ```

3. **性能建议**
   - 生产环境高性能：使用离线模式（C++ indexsearcher）
   - Python 批量查询：使用在线模式 + BatchSearch
   - Python 交互式查询：使用在线模式单个 Search

## 十二、参考资料

- [SPTAG GitHub](https://github.com/microsoft/SPTAG)
- [SPANN_BENCHMARK_GUIDE.md](./SPANN_BENCHMARK_GUIDE.md) - SPANN 磁盘索引测试指南
- [docs/Parameters.md](./docs/Parameters.md) - 参数详细说明
