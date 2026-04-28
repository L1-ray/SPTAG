# SPANN 搜索阶段参数完全指南

本文档汇总 SPANN（Disk-based ANN Index）搜索阶段涉及的所有关键参数，按功能分类说明其作用、默认值及相互关系。

---

## 一、线程与并发控制

| 参数名 | 变量名 | 代码默认值 | 当前测试值 | 搜索阶段作用 |
|--------|--------|-----------|-----------|-------------|
| **SearchThreadNum** | `m_searchThreadNum` | 2 | 16 | 前端查询并发线程数。决定同时处理多少个查询。每个线程独立执行完整的搜索流程（Head Index 搜索 + Posting List 读取 + 重排序）。 |
| **NumberOfThreads** | `m_iSSDNumberOfThreads` | 16 | 16 | **BATCH_READ 模式下**：AIO context 数量和 workspace 池大小。每个搜索线程需要绑定一个 workspace 才能执行，因此 **必须 >= SearchThreadNum**。 |
| **IOThreads** | `m_ioThreads` | 4 | — | 每个 handler 的 I/O 线程数。**仅在非 BATCH_READ 模式有效**。Linux 下非 BATCH_READ 路径因 `RequestQueue.pop()` 编译错误无法使用，因此该参数在 Linux 实际无效。 |

### 关键关系

```
有效配置必须满足: NumberOfThreads >= SearchThreadNum
```

- 当 `SearchThreadNum > NumberOfThreads` 时，部分查询线程分配不到 workspace，会在 `Sent 0.00%` 处卡住，产生不完整运行。
- 在 SATA SSD 上，一旦满足 `NumberOfThreads >= SearchThreadNum`，继续增加 `NumberOfThreads` 不会提高 QPS，因为瓶颈在磁盘随机读取带宽而非 CPU。

---

## 二、搜索结果质量控制（读放大主要来源）

这组参数直接决定每次查询需要读取多少数据，是影响 I/O 开销和召回率的核心。

| 参数名 | 变量名 | 代码默认值 | 当前测试值 | 搜索阶段作用 |
|--------|--------|-----------|-----------|-------------|
| **InternalResultNum** | `m_searchInternalResultNum` | 64 | 32 | Head Index（内存索引）搜索后保留的最近邻 Head 向量数量。**直接决定需要读取多少个 Posting List**。设为 32 时，平均会读取约 32 个 Posting List。 |
| **MaxCheck** | `m_maxCheck` | 4096 | 2048 | Head Index 搜索时最多检查的节点数上限。控制 Head 搜索的精度和开销。**同时决定去重哈希表的大小**：实际大小为 `min(1 << HashTableExponent, MaxCheck * 64)`。 |
| **SearchPostingPageLimit** | `m_searchPostingPageLimit` | 3 | 15 | 单个 Posting List 最多读取的 4KB 页数。如果 Posting List 跨越多个页，此参数限制读取页数。设为 15 意味着读取几乎全部内容。 |
| **ResultNum** | `m_resultNum` | 5 | 10 | 最终返回给用户的结果数量。 |
| **MaxDistRatio** | `m_maxDistRatio` | 10000 | 8.0 | 距离比例阈值，用于过滤掉距离 query 过远的候选向量。8.0 是一个较严格的过滤条件。 |
| **HashTableExponent** | `m_hashExp` | 4 | 12 | 去重哈希表的指数，实际槽位数为 `2^HashTableExponent`。用于记录在 Posting List 中已见过的向量，避免重复距离计算。12 即 4096 个槽位。 |

### 读放大链条（以当前测试配置为例）

```
InternalResultNum = 32
    → 平均接触 ~31.7 个 Posting List

× SearchPostingPageLimit = 15
    → 每个 List 读取约 6 个有效页（受实际数据分布限制）
    → 每查询共读取 ~193 个 4KB 页面

= 每查询读取 ~792 KB 原始数据

→ 每查询解码评估 ~1248 个向量

→ 最终只返回 ResultNum = 10 个结果

→ final_result_ratio ≈ 10 / 1248 ≈ 0.008
```

这意味着 **99.2% 的读取和计算工作不直接产生最终输出**，是主要的读取放大来源。

---

## 三、I/O 模式与统计框架

### 3.1 运行时配置参数

| 参数名 | 变量名 | 代码默认值 | 当前测试值 | 说明 |
|--------|--------|-----------|-----------|------|
| **EnableDetailedIOStats** | `m_enableDetailedIOStats` | false | true | 是否启用细粒度 I/O 统计框架。开启后会在搜索结束后输出查询级和线程级的详细 I/O 指标分布。 |
| **DetailedIOStatsOutput** | `m_detailedIOStatsOutput` | "" | `results/io_analysis/.../query_io_stats.csv` | 详细统计 CSV 文件的输出路径。如果为空，则不输出 CSV。 |
| **DetailedIOStatsSampleRate** | `m_detailedIOStatsSampleRate` | 1.0 | 1.0 | 采样率，范围 [0.0, 1.0]。1.0 表示记录所有查询，0.1 表示只记录 10%。用于降低高频查询场景下的统计开销。 |

### 3.2 编译级 I/O 模式（非配置参数）

I/O 模式由 `AnnService/inc/Helper/AsyncFileReader.h` 中的宏控制，**无法通过 INI 配置文件修改**：

```cpp
#define ASYNC_READ 1
#define BATCH_READ 1
```

| 模式 | 宏配置 | Linux 可用性 | 说明 |
|------|--------|-------------|------|
| 批量异步读取 | `ASYNC_READ=1, BATCH_READ=1` | ✅ 可用（默认） | 使用 Linux AIO + 批量提交。搜索线程批量收集 I/O 请求后统一提交。 |
| 非批量异步读取 | `ASYNC_READ=1, BATCH_READ=0` | ❌ 编译失败 | `RequestQueue` 使用 TBB `ConcurrentQueue`，只有 `try_pop()` 方法，但代码调用 `pop()`，导致编译错误。 |
| 同步读取 | `ASYNC_READ=0` | ⚠️ 未验证 | 使用标准阻塞式读取，预计性能极差。 |

**结论**：Linux 下 SPANN 搜索实际上只能使用 BATCH_READ 模式。

---

## 四、索引构建参数（间接影响搜索）

这些参数在索引构建阶段生效，但决定了搜索时面对的 Posting List 结构。

| 参数名 | 变量名 | 代码默认值 | 当前测试值 | 对搜索的影响 |
|--------|--------|-----------|-----------|-------------|
| **PostingPageLimit** | `m_postingPageLimit` | 3 | — | 构建时每个 Posting List 的最大页数。**SearchPostingPageLimit 不应超过此值**，否则读取的是未初始化的数据。 |
| **PostingVectorLimit** | `m_postingVectorLimit` | 118 | — | 构建时每个 Posting List 最多容纳的向量数。影响单个 Posting List 的大小和页数。 |
| **EnableDeltaEncoding** | `m_enableDeltaEncoding` | false | — | 是否对 Posting List 中的向量 ID 进行增量编码。开启后 Posting List 体积减小，但搜索时需要额外解码开销。 |
| **EnablePostingListRearrange** | `m_enablePostingListRearrange` | false | — | 是否重排 Posting List 内的向量顺序以提高访问局部性。开启后可能减少缺页，提高缓存命中率。 |
| **EnableDataCompression** | `m_enableDataCompression` | false | — | 是否使用 ZSTD 压缩 Posting List 数据。开启后磁盘读取量减少，但增加了 CPU 解压开销。 |
| **ExcludeHead** | `m_excludehead` | true | — | Head 向量是否从 Posting List 中排除。如果为 true，搜索结果需要单独合并 Head Index 的结果。 |
| **ReplicaCount** | `m_replicaCount` | 8 | — | 每个向量分配的 Posting List 副本数。增加副本数可以提高召回率，但会增加构建时间和存储空间。 |
| **RNGFactor** | `m_rngFactor` | 1.0 | — | Relative Neighborhood Graph 的边数控制因子。影响 Head Index 的图密度和搜索精度。 |

---

## 五、其他搜索相关参数

| 参数名 | 变量名 | 代码默认值 | 当前测试值 | 说明 |
|--------|--------|-----------|-----------|------|
| **SearchResult** | `m_searchResult` | "" | `/media/ray/1tb/sift1m/search_results.bin` | 搜索结果二进制文件的输出路径。如果为空，不保存结果。 |
| **LogFile** | `m_logFile` | "" | — | 日志文件路径。如果为空，日志输出到 stderr/stdout。 |
| **QpsLimit** | `m_qpsLimit` | 0 | — | 查询速率限制（Queries Per Second）。0 表示不限制。用于模拟特定负载。 |
| **QueryCountLimit** | `m_queryCountLimit` | max | — | 最大查询处理数量。用于在完整查询集上只测试前 N 条。 |
| **IOTimeout** | `m_iotimeout` | 30 | — | 单次 I/O 操作的超时时间（秒）。超过此时间未完成的 I/O 会被视为失败。 |
| **SearchTimes** | `m_searchTimes` | 1 | — | 重复搜索的次数。用于获取更稳定的平均性能指标。 |
| **Rerank** | `m_rerank` | 0 | — | 对召回的候选结果进行精确距离重排序的数量。0 表示不重排序。 |
| **EnableADC** | `m_enableADC` | false | — | 是否启用非对称距离计算（Asymmetric Distance Computation）。通常配合量化使用。 |
| **CacheSizeGB** | `m_cacheSize` | 0 | — | SSD 索引缓存大小（GB）。0 表示不启用缓存。增加缓存可以减少重复查询的磁盘 I/O。 |
| **CacheShards** | `m_cacheShards` | 1 | — | 缓存分片数。增加分片可以减少多线程并发访问缓存时的锁竞争。 |
| **IterativeSearchHeadBatch** | `m_headBatch` | 32 | — | 迭代搜索时 Head Index 的批处理大小。 |

---

## 六、延迟分解与参数影响

单次查询的延迟可以分解为以下几个阶段，各阶段受不同参数控制：

```
总延迟 (Total Latency)
├─ Head Index 搜索延迟 (~64-78% of single-query latency)
│   ├─ 受 MaxCheck 控制（检查的节点数上限）
│   └─ 受 InternalResultNum 控制（返回的 Head 候选数）
│
├─ I/O 提交与等待延迟 (~15-30%)
│   ├─ 受 NumberOfThreads / SearchThreadNum 控制（并发度）
│   ├─ 受 SearchPostingPageLimit 控制（每 List 读多少页）
│   └─ 受底层存储性能控制（SATA SSD ~60 MB/s）
│
├─ Posting List 解析与去重延迟 (~5-10%)
│   ├─ 受 SearchPostingPageLimit 控制（需要解析的数据量）
│   └─ 受 HashTableExponent 控制（去重哈希表大小）
│
└─ 距离计算与排序延迟 (~5-10%)
    ├─ 受实际读取的向量数控制
    └─ 受 MaxDistRatio 控制（提前过滤的候选数）
```

**重要区分**：
- **单查询延迟瓶颈**：Head Index 搜索通常是最大组成部分。
- **吞吐量（QPS）瓶颈**：对于 SIFT1M + SATA SSD 场景，磁盘 4KB 随机读取带宽（~60 MB/s）是硬上限。增加 `SearchThreadNum` 超过一定阈值后只会增加延迟而不会提高 QPS。

---

## 七、参数调优建议

### 7.1 针对 SATA SSD 瓶颈的调优

当前测试表明 QPS 被钉在 ~80，瓶颈在磁盘随机读带宽。可尝试：

| 调优方向 | 建议参数调整 | 预期效果 | 风险 |
|----------|-------------|----------|------|
| 减少 Posting List 读取 | `SearchPostingPageLimit` 从 15 降到 3-5 | 显著减少每查询 I/O 量 | 召回率下降 |
| 减少 Head 候选数 | `InternalResultNum` 从 32 降到 16-24 | 减少接触的 Posting List 数量 | 召回率下降 |
| 收紧距离过滤 | `MaxDistRatio` 从 8.0 降到 4.0-6.0 | 减少无效候选评估 | 可能过滤掉真实近邻 |
| 降低并发 | `SearchThreadNum=2~4`, `NumberOfThreads=4~8` | 降低队列深度，减少延迟 | 无法提高 QPS |

### 7.2 针对召回率/QPS 权衡的调优

如果当前召回率有富余，可以：

1. 逐步降低 `SearchPostingPageLimit`，观察 QPS 和 Recall@10 的变化曲线。
2. 同步调整 `InternalResultNum`，找到帕累托最优。
3. 使用 `MinInternalResultNum` / `StepInternalResultNum` / `MaxInternalResultNum` 进行自动化的召回率-延迟权衡扫描。

### 7.3 硬件升级建议

如果目标是评估 SPANN 的磁盘索引可扩展性：

- **更大规模数据集**（如 SIFT100M、Deep1B）：小数据集的读放大问题会被摊薄。
- **NVMe SSD**：高队列深度下 4KB 随机读可达 300K+ IOPS，是 SATA SSD 的 3-5 倍。
- **增加内存缓存**：设置 `CacheSizeGB` 将热数据缓存在内存中。

---

## 八、完整配置文件示例

以下是一个针对 I/O 分析优化的搜索配置：

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
NumberOfThreads=16
SearchThreadNum=16
HashTableExponent=12
ResultNum=10
MaxCheck=2048
MaxDistRatio=8.0
SearchPostingPageLimit=15
EnableDetailedIOStats=true
DetailedIOStatsOutput=/home/ray/code/SPTAG/results/io_analysis/query_io_stats.csv
DetailedIOStatsSampleRate=1.0
```

---

## 九、参考文档

- `docs/Parameters.md` - SPTAG 官方参数文档
- `TEST_COMMANDS.md` - 本项目测试命令与流程文档
- `SPANN_IO_Analysis_Plan_Revised.md` - I/O 性能分析方案
- `SPANN_SIFT1M_SATA_Bottleneck_Analysis.md` - SIFT1M SATA SSD 瓶颈分析
