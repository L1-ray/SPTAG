# SPANN 搜索阶段细粒度 I/O 性能分析方案

## 一、背景与目标

### 1.1 问题背景

之前的测试表明 SPANN 搜索存在 I/O 瓶颈：

- 16 线程配置下 QPS 下降 26%，延迟增长 9 倍；
- 磁盘读取速率从 65 MB/s 降至 52 MB/s；
- 但由于监控粒度太粗，无法定位具体瓶颈原因。

现有结果只能说明“存在 I/O 相关退化”，还不能回答以下问题：

- 每个 query 到底读取了多少 posting、page 和 bytes；
- posting 内实际扫描了多少原始元素；
- 重复 VID 消耗了多少扫描与读取；
- I/O 等待、posting 解码、posting 解析和距离计算分别占多少；
- SSD 队列深度、读取带宽、CPU idle/iowait 与 query 延迟是否同时间相关；
- QPS 平顶是由 search 线程、I/O 线程、SSD 队列，还是 posting 读放大导致。

因此，本方案的目标不是单纯新增日志，而是建立 **查询级、posting 级、设备级** 之间可对齐、可复现、可验证的观测闭环。

### 1.2 分析目标

通过细粒度监测以下指标，定位 SPANN 搜索阶段 I/O 瓶颈的根因。

| 类别 | 指标 | 说明 |
|------|------|------|
| **查询级 I/O** | requested_read_bytes_per_query | C++ 查询逻辑请求读取的字节数，不等同于设备实际物理读取字节数 |
| **查询级 I/O** | pages_read_per_query | 静态 SPANN 路径下读取的 posting page 数 |
| **查询级 I/O** | postings_touched_per_query | 每个 query 访问的 posting list 数量 |
| **查询级扫描** | posting_elements_scanned_per_query | posting 中实际遍历到的原始元素数，必须是 raw count |
| **查询级扫描** | distance_evaluated_per_query | 去重后真正执行距离计算的元素数 |
| **效率指标** | duplicate_vector_read_ratio | 重复 VID 数 / 原始扫描元素数 |
| **效率指标** | distance_eval_ratio | 距离计算元素数 / 原始扫描元素数 |
| **效率指标** | final_result_ratio | 最终结果数 / 原始扫描元素数，仅作为辅助指标 |
| **未来 M2 指标** | coarse_candidate_ratio | 两阶段 posting 后：rerank 候选数 / 原始扫描元素数 |
| **未来 M2 指标** | coarse_candidate_recall | 两阶段 posting 后：真实近邻进入 coarse candidates 的比例 |
| **系统级 I/O** | SSD instant_queue_depth | `/proc/diskstats` 中瞬时 in-flight I/O 数 |
| **系统级 I/O** | SSD avg_queue_depth | 通过 weighted I/O time 推导的平均队列深度 |
| **系统级 I/O** | SSD read_bandwidth_utilization | 实际读取带宽 / 设备基线读取带宽 |
| **系统级 CPU** | CPU idle / iowait | 辅助信号，不单独作为阻塞判断依据 |
| **系统级 stall** | PSI io some/full | I/O pressure stall 信息，用于辅助判断 I/O stall |
| **延迟分析** | per_query_io_wait_time | query 等待 I/O 完成的时间，必须与 decode/compute 分离 |
| **延迟分析** | posting_decode_latency, posting_parse_latency, distance_calc_latency | 分离 I/O 后的 CPU 侧开销 |
| **延迟分析** | P50 / P95 / P99 latency | query 延迟分布 |
| **性能曲线** | QPS vs searchThreadNum × ioThreads | 线程组合下的 QPS 曲线 |

特别说明：

1. `bytes_read_per_query` 在本方案中统一命名为 `requested_read_bytes_per_query`，表示 C++ 查询逻辑请求读取的字节数。
2. 设备实际读取字节数由 `/proc/diskstats` 或 `/proc/[pid]/io` 统计，不与 C++ 逻辑请求字节数混用。
3. `useful_candidate_ratio` 不再使用“最终 TopK 数 / 扫描元素数”作为核心定义。legacy 阶段优先使用 `distance_eval_ratio`、`duplicate_vector_read_ratio` 和 `final_result_ratio`；两阶段 posting 落地后再使用 `coarse_candidate_ratio` 和 `coarse_candidate_recall`。
4. `CPU iowait` 只作为辅助信号，因为它不能单独可靠表达 I/O 阻塞；需要结合 PSI、SSD queue depth 和 per-query I/O wait 分析。

---

## 二、现有代码分析

### 2.1 现有统计结构 (`IExtraSearcher.h`)

当前 `SearchStats` 已包含部分搜索统计字段：

```cpp
struct SearchStats {
    int m_check;                    // 检查的节点数
    int m_exCheck;                  // 扩展检查数
    int m_totalListElementsCount;   // legacy 字段：不同路径下语义需谨慎解释
    int m_diskIOCount;              // legacy 字段：通常表示磁盘 I/O / posting 访问次数
    int m_diskAccessCount;          // legacy 字段：不同路径下语义不统一
    double m_totalSearchLatency;    // Head Index 搜索延迟
    double m_totalLatency;          // 总延迟
    double m_exLatency;             // 扩展延迟（磁盘搜索）
    double m_asyncLatency0/1/2;     // 异步延迟细分
    double m_queueLatency;          // 队列等待延迟
    double m_sleepLatency;          // 睡眠延迟
    double m_compLatency;           // 计算延迟
    double m_diskReadLatency;       // 磁盘读取延迟
    double m_exSetUpLatency;        // 扩展设置延迟
};
```

这些字段可以作为兼容输出保留，但不适合直接承担新的观测闭环：

- `m_totalListElementsCount` 在部分路径中可能已被 dedup 修正，不能直接等价为 posting 原始扫描元素数；
- `m_diskAccessCount` 在静态 SPANN 中接近 page count，但在 dynamic / SPFresh 相关路径中可能被解释为 KB 或其他单位，禁止跨模式比较；
- `m_diskReadLatency` 的语义不足以稳定区分 I/O 等待、callback 执行、posting 解压、posting 解析和距离计算；
- 当前缺少可与系统级监控对齐的 query_start_ns / query_end_ns。

因此，新增指标必须使用新的明确字段，不应复用 legacy 字段作为核心分析口径。

### 2.2 现有输出 (`SSDIndex.h`)

当前已输出的统计包括：

- Ex Elements Count；
- Head Latency Distribution；
- Ex Latency Distribution；
- Total Latency Distribution；
- Total Disk Page Access Distribution；
- Total Disk IO Distribution。

这些输出适合保留，但需要注意：

- `Total Disk Page Access Distribution` 在 static SPANN 下可以理解为 page access；
- 对 dynamic / SPFresh，不能简单沿用同一语义；
- percentile 打印函数需要先支持空样本保护，否则新增更多统计项后，空 query、过滤样本为空或 queryCountLimit 为 0 时可能出现除零或越界。

### 2.3 缺失的指标

| 指标 | 当前状态 | 需要添加或修正 |
|------|----------|----------------|
| requested_read_bytes_per_query | ❌ | 新增 `m_requestedReadBytes`，由 C++ 查询路径累计逻辑请求字节数 |
| pages_read_per_query | 部分 | 新增 `m_readPages`，静态路径由 `listPageCount` 累计 |
| postings_touched_per_query | 部分 | 新增 `m_postingsTouched`，不要只依赖 legacy `m_diskIOCount` |
| posting_elements_scanned_per_query | ❌ | 新增 `m_postingElementsRaw`，必须在 posting loop 中统计 raw count |
| distance_evaluated_per_query | ❌ | 新增 `m_distanceEvaluatedCount`，在真正执行 ComputeDistance 时统计 |
| duplicate_vector_read_ratio | ❌ | 新增 `m_duplicateVectorCount`，在 deduper 命中处统计 |
| final_result_ratio | ❌ | 新增 `m_finalResultCount`，仅作为辅助效率指标 |
| per_query_io_wait_time | 部分 | 新增 `m_ioWaitLatencyMs`，必须与 decode/parse/compute 分离 |
| posting_decode_latency | ❌ | 新增 `m_postingDecodeLatencyMs` |
| posting_parse_latency | ❌ | 新增 `m_postingParseLatencyMs` |
| distance_calc_latency | 部分 | 新增 `m_distanceCalcLatencyMs`，不直接复用 `m_compLatency` |
| query_start_ns / query_end_ns | ❌ | 新增 monotonic ns 时间戳，用于和系统监控 join |
| SSD queue depth | ❌ | 系统级监控脚本采集 `/proc/diskstats` field 9 和 weighted I/O time |
| SSD read bandwidth utilization | ❌ | 需要设备 baseline 作为分母 |
| CPU idle / iowait / PSI | ❌ | 系统级监控脚本采集 `/proc/stat` 和 `/proc/pressure/io` |

---

## 三、实现方案

### 3.1 方案概述

采用 **三层监控架构**：

```text
┌─────────────────────────────────────────────────────────────┐
│                    Layer 1: C++ 代码层                       │
│  扩展 SearchStats，记录 query-level/posting-level 指标，       │
│  输出聚合 percentile 和可选 per-query CSV。                    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    Layer 2: 系统监控层                       │
│  Python 脚本实时采集 /proc/diskstats, /proc/[pid]/io,          │
│  /proc/stat, /proc/pressure/io。                               │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    Layer 3: 分析聚合层                       │
│  基于同一 monotonic 时钟域合并查询级指标和系统级指标，          │
│  生成报告和可视化图表。                                        │
└─────────────────────────────────────────────────────────────┘
```

三个关键约束：

1. **统一时钟域**：Layer 1 和 Layer 2 必须使用同一类 monotonic clock。C++ 侧输出 `query_start_ns/query_end_ns`，Python 侧输出 `sample_start_ns/sample_end_ns`。
2. **统一延迟语义**：`io_wait_latency_ms` 只能表示 I/O 等待，不得混入 posting decode、posting parse 和 distance compute。
3. **禁用 legacy 字段跨模式解释**：新增报表不得用 `m_diskAccessCount` 作为跨 static SPANN / dynamic SPANN / SPFresh 的统一指标。

Layer 1 默认只输出聚合统计。per-query 明细采用 CSV，并支持 sample rate。默认不在热路径逐 query 打 JSON 日志，避免日志锁和格式化开销扰动结果。

### 3.2 Layer 1: C++ 代码修改

#### 3.2.1 扩展 SearchStats 结构

**文件**: `AnnService/inc/Core/SPANN/IExtraSearcher.h`

建议保留现有字段，并新增以下字段。新增计数字段统一使用 `uint64_t`，避免大规模数据集或高并发下溢出。

```cpp
struct SearchStats {
    // === 现有字段：兼容保留 ===
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
    std::chrono::steady_clock::time_point m_searchRequestTime;
    int m_threadID;

    // === 新增字段：query timeline ===
    uint64_t m_queryStartNs;
    uint64_t m_queryEndNs;

    // === 新增字段：query-level I/O ===
    uint64_t m_postingsTouched;
    uint64_t m_readPages;
    uint64_t m_requestedReadBytes;

    // === 新增字段：posting scan ===
    uint64_t m_postingElementsRaw;
    uint64_t m_distanceEvaluatedCount;
    uint64_t m_duplicateVectorCount;

    // === 新增字段：candidate / result ===
    uint64_t m_finalResultCount;
    uint64_t m_truthHitCount;             // 可选：仅有 ground truth 时填充
    uint64_t m_rerankCandidateCount;      // 未来 M2 两阶段 posting 使用

    // === 新增字段：latency breakdown ===
    double m_headSearchLatencyMs;
    double m_ioIssueLatencyMs;
    double m_ioWaitLatencyMs;
    double m_batchReadTotalLatencyMs;     // 仅在未重构 BATCH_READ callback 时使用
    double m_postingDecodeLatencyMs;
    double m_postingParseLatencyMs;
    double m_distanceCalcLatencyMs;
};
```

建议同时给 `SearchStats` 增加统一方法，避免 SPFresh 或其他路径手工漏字段：

```cpp
void Reset();
void Add(const SearchStats& other);
void Divide(double n);
```

如果短期不改结构方法，而是继续在各处手动 reset/add/avg，则必须同步更新所有使用 `SearchStats` 聚合的代码。若本阶段仅分析静态 SPANN，需要在代码和文档中明确：SPFresh / dynamic 路径暂不保证新增字段聚合完整。

#### 3.2.2 修改搜索代码收集指标

**文件**: `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`

在 `SearchIndex` 和 `ProcessPosting` 相关路径中添加统计。

##### 1. 统计 posting、page、requested bytes

在遍历 posting list 时：

```cpp
p_stats->m_postingsTouched += 1;
p_stats->m_readPages += listInfo->listPageCount;
p_stats->m_requestedReadBytes +=
    static_cast<uint64_t>(listInfo->listPageCount) << p_exWorkSpace->m_pageSizeEx;
```

注意：

- 这些字段表示 query 逻辑请求，不表示设备实际物理读取；
- 不能再用 `m_diskAccessCount * PageSize` 作为跨模式 bytes_read；
- `m_diskAccessCount` 可继续作为 legacy 输出，但新增分析使用 `m_readPages` 和 `m_requestedReadBytes`。

##### 2. 统计 raw scan、duplicate、distance evaluated

在 posting 元素解析循环中，必须区分三类计数：

```cpp
// 每遍历到一条 posting 记录就计 raw count
p_stats->m_postingElementsRaw += 1;

// deduper 命中表示重复 VID
if (p_exWorkSpace->m_deduper.CheckAndSet(vectorID)) {
    p_stats->m_duplicateVectorCount += 1;
    continue;
}

// 只有真正执行距离计算时才计 distance evaluated
p_stats->m_distanceEvaluatedCount += 1;
ComputeDistance(...);
```

这样才能得到：

```text
duplicate_vector_read_ratio
= m_duplicateVectorCount / m_postingElementsRaw

distance_eval_ratio
= m_distanceEvaluatedCount / m_postingElementsRaw
```

不要用被 dedup 修正后的 `m_totalListElementsCount` 代替 `m_postingElementsRaw`。

##### 3. 统计延迟阶段

目标延迟拆分为：

```text
io_issue_latency_ms
io_wait_latency_ms
posting_decode_latency_ms
posting_parse_latency_ms
distance_calc_latency_ms
```

三条读取路径的语义必须一致。

###### sync read 路径

```text
io_issue_latency_ms:
  可记录为 0 或 ReadBinary 调用前的准备时间

io_wait_latency_ms:
  ReadBinary wall time

posting_decode_latency_ms:
  解压耗时

posting_parse_latency_ms:
  posting buffer 解析耗时，不含 ComputeDistance

distance_calc_latency_ms:
  ComputeDistance 累计耗时
```

###### ASYNC_READ 非 BATCH 路径

```text
io_issue_latency_ms:
  提交 AsyncReadRequest 的时间

io_wait_latency_ms:
  submit 完成后，到最后一个 completion 被 query thread 消费前的等待时间

posting_decode_latency_ms / posting_parse_latency_ms / distance_calc_latency_ms:
  query thread 消费 completion 后统一统计
```

###### BATCH_READ 路径

当前 BATCH_READ callback 内会执行 decompress 和 ProcessPosting，因此如果直接把 `BatchReadFileAsync(...)` 的 wall time 记为 `io_wait_latency_ms`，会混入 decode 和 compute，导致三条路径语义不一致。

为保证观测闭环可信，本方案要求优先采用以下改法：

```text
BATCH_READ callback 只标记完成并把 request 放入完成队列；
posting decode / parse / distance compute 统一在 query thread 消费完成队列时执行。
```

如果短期不重构 BATCH_READ callback，则该路径不得输出严格意义上的 `io_wait_latency_ms`，只能输出：

```text
batch_read_total_latency_ms
```

并在报告中明确：

```text
batch_read_total_latency_ms 包含 I/O wait、callback、decode、parse 和 distance compute，不能与 sync/async 的 io_wait_latency_ms 直接比较。
```

#### 3.2.3 添加详细统计输出

**文件**: `AnnService/inc/SSDServing/SSDIndex.h`

输出分为两类：

1. 聚合 percentile 输出；
2. 可选 per-query CSV 输出。

不建议默认输出逐 query JSON。

##### 1. 修复 percentile 打印空样本问题

`PrintPercentiles` 必须先处理空样本：

```cpp
if (collects.empty()) {
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
        "Avg\t50tiles\t90tiles\t95tiles\t99tiles\t99.9tiles\tMax\n");
    SPTAGLIB_LOG(Helper::LogLevel::LL_Info,
        "NA\tNA\tNA\tNA\tNA\tNA\tNA\n");
    return;
}
```

percentile index 建议使用 helper，避免小样本下越界或含义不稳定：

```cpp
size_t PercentileIndex(size_t n, double p) {
    if (n == 0) return 0;
    size_t idx = static_cast<size_t>(std::ceil(p * n)) - 1;
    return std::min(idx, n - 1);
}
```

##### 2. 新增聚合输出

```cpp
SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n=== Detailed I/O Statistics ===\n");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nRequested Bytes Read Per Query:\n");
PrintPercentiles<uint64_t, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> uint64_t { return ss.m_requestedReadBytes; },
    "%llu");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPages Read Per Query:\n");
PrintPercentiles<uint64_t, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> uint64_t { return ss.m_readPages; },
    "%llu");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPostings Touched Per Query:\n");
PrintPercentiles<uint64_t, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> uint64_t { return ss.m_postingsTouched; },
    "%llu");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nRaw Posting Elements Scanned Per Query:\n");
PrintPercentiles<uint64_t, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> uint64_t { return ss.m_postingElementsRaw; },
    "%llu");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDistance Evaluated Per Query:\n");
PrintPercentiles<uint64_t, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> uint64_t { return ss.m_distanceEvaluatedCount; },
    "%llu");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDuplicate Vector Read Ratio:\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double {
        return ss.m_postingElementsRaw > 0
            ? static_cast<double>(ss.m_duplicateVectorCount) / ss.m_postingElementsRaw
            : 0.0;
    }, "%.6lf");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDistance Eval Ratio:\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double {
        return ss.m_postingElementsRaw > 0
            ? static_cast<double>(ss.m_distanceEvaluatedCount) / ss.m_postingElementsRaw
            : 0.0;
    }, "%.6lf");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nI/O Wait Latency (ms):\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double { return ss.m_ioWaitLatencyMs; },
    "%.3lf");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPosting Decode Latency (ms):\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double { return ss.m_postingDecodeLatencyMs; },
    "%.3lf");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nPosting Parse Latency (ms):\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double { return ss.m_postingParseLatencyMs; },
    "%.3lf");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nDistance Calc Latency (ms):\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double { return ss.m_distanceCalcLatencyMs; },
    "%.3lf");
```

##### 3. per-query CSV 输出

新增可选参数：

```text
EnableDetailedIOStats = false
DetailedIOStatsOutput = results/query_io_stats.csv
DetailedIOStatsSampleRate = 1.0
```

CSV 字段建议：

```csv
run_id,thread_count,io_threads,ssd_threads,query_id,thread_id,query_start_ns,query_end_ns,total_latency_ms,head_latency_ms,ex_latency_ms,io_issue_ms,io_wait_ms,batch_read_total_ms,posting_decode_ms,posting_parse_ms,distance_calc_ms,postings_touched,pages_read,requested_read_bytes,posting_elements_raw,distance_evaluated_count,duplicate_vector_count,final_result_count,recall
```

写 CSV 时建议使用内存 buffer，查询结束或 run 结束后统一 flush，避免热路径频繁写日志。

### 3.3 Layer 2: 系统级监控脚本

**文件**: `scripts/spann_io_monitor.py`

主要功能：

- 读取 `/proc/diskstats` 获取磁盘 I/O 统计和队列深度；
- 读取 `/proc/[pid]/io` 获取进程级实际读取字节数；
- 读取 `/proc/stat` 获取 CPU idle / iowait；
- 读取 `/proc/pressure/io` 获取 I/O pressure stall；
- 使用 monotonic ns 作为采样时间戳；
- 计算读取带宽、平均队列深度、峰值队列深度、设备带宽利用率。

关键数据结构：

```python
@dataclass
class DiskStats:
    """磁盘统计，来自 /proc/diskstats"""
    sample_start_ns: int
    sample_end_ns: int
    device: str
    reads_completed: int
    sectors_read: int
    read_time_ms: int
    io_in_progress: int
    weighted_io_time_ms: int

@dataclass
class CPUStats:
    """CPU 统计，来自 /proc/stat"""
    sample_start_ns: int
    sample_end_ns: int
    user: int
    system: int
    idle: int
    iowait: int

@dataclass
class ProcessIOStats:
    """进程级 I/O 统计，来自 /proc/[pid]/io"""
    sample_start_ns: int
    sample_end_ns: int
    pid: int
    read_bytes: int
    write_bytes: int

@dataclass
class PSIStats:
    """I/O pressure stall，来自 /proc/pressure/io"""
    sample_start_ns: int
    sample_end_ns: int
    some_avg10: float
    some_avg60: float
    some_avg300: float
    some_total: int
    full_avg10: float
    full_avg60: float
    full_avg300: float
    full_total: int
```

关键计算：

```text
read_bandwidth_mbs
= delta(sectors_read) * 512 / elapsed_seconds / 1024 / 1024

avg_queue_depth
= delta(weighted_io_time_ms) / elapsed_ms

instant_queue_depth
= io_in_progress

peak_queue_depth
= max(io_in_progress samples)

read_bandwidth_utilization
= read_bandwidth_mbs / device_baseline_read_mbps
```

脚本参数建议：

```bash
python3 scripts/spann_io_monitor.py \
  --device nvme0n1 \
  --pid <sptag_pid> \
  --interval-ms 100 \
  --device-max-read-mbps 3500 \
  --output-dir results/io_monitor
```

说明：

- `device-max-read-mbps` 不应写死，建议由 fio 或设备规格测得；
- `io_in_progress` 是瞬时值，不能单独代表平均队列深度；
- `avg_queue_depth` 应使用 weighted I/O time 计算；
- CPU iowait 只作为辅助，需要结合 PSI 和 per-query io_wait 解释。

### 3.4 Layer 3: 分析聚合脚本

**文件**: `scripts/analyze_spann_io.py`

主要功能：

- 解析 SPTAG 聚合输出；
- 解析 per-query CSV；
- 合并系统级指标；
- 按 `query_start_ns/query_end_ns` 与 `sample_start_ns/sample_end_ns` 做时间窗口 join；
- 计算效率指标；
- 分析延迟分解；
- 输出瓶颈分析报告和图表。

核心派生指标：

```text
bytes_per_scanned_element
= requested_read_bytes / posting_elements_raw

duplicate_vector_read_ratio
= duplicate_vector_count / posting_elements_raw

distance_eval_ratio
= distance_evaluated_count / posting_elements_raw

final_result_ratio
= final_result_count / posting_elements_raw

io_wait_ratio
= io_wait_latency_ms / total_latency_ms

read_amplification_vs_distance_eval
= requested_read_bytes / max(1, distance_evaluated_count * vector_record_bytes)

read_amplification_vs_final_result
= requested_read_bytes / max(1, final_result_count * vector_record_bytes)
```

核心关联分析：

```text
query latency vs requested_read_bytes
query latency vs pages_read
query latency vs posting_elements_raw
query latency vs duplicate_vector_read_ratio
query latency vs SSD avg_queue_depth
query latency vs process read_bytes delta
query latency vs PSI io some/full
QPS vs searchThreadNum × ioThreads
```

输入兼容性要求：

- 能解析旧版 `spann_search_16t_nocache.log` 和 `spann_search_16t_nocache.csv`；
- 新字段缺失时应 graceful fallback；
- 对空样本、缺列、NaN、除零做保护。

---

## 四、文件清单

| 序号 | 文件路径 | 说明 |
|------|----------|------|
| 1 | `AnnService/inc/Core/SPANN/IExtraSearcher.h` | 扩展 `SearchStats`，新增明确口径的 query-level 指标 |
| 2 | `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h` | 添加 static SPANN 搜索路径指标收集；修正 BATCH_READ 统计语义 |
| 3 | `AnnService/inc/SSDServing/SSDIndex.h` | 添加详细统计输出、per-query CSV、修复 percentile 空样本 |
| 4 | `AnnService/inc/SPFresh/SPFresh.h` | 如需覆盖 SPFresh，则同步更新 reset/add/avg/print；否则明确本阶段不支持 |
| 5 | `scripts/spann_io_monitor.py` | 系统级 I/O / CPU / PSI 监控脚本 |
| 6 | `scripts/analyze_spann_io.py` | 分析聚合脚本 |
| 7 | `scripts/run_io_analysis.sh` | 完整测试流程脚本 |
| 8 | `tests/...` | 新增 SearchStats、ExtraStaticSearcher 统计、监控解析、CSV 解析相关测试 |

---

## 五、实施步骤

### 5.1 Phase 1: 指标契约与 C++ 层最小埋点

1. **明确指标契约**
   - `requested_read_bytes`：C++ 查询逻辑请求字节数；
   - `device_read_bytes`：系统设备实际读取字节数；
   - `process_read_bytes`：进程实际 read_bytes；
   - `posting_elements_raw`：posting loop 原始遍历元素数；
   - `distance_evaluated_count`：实际执行距离计算的元素数；
   - `duplicate_vector_count`：deduper 命中的重复 VID 数；
   - `query_start_ns/query_end_ns`：monotonic ns 时间戳。

2. **修改 `IExtraSearcher.h`**
   - 添加新的统计字段到 `SearchStats`；
   - 新增字段统一使用 `uint64_t` 或 `double`；
   - 添加初始化代码；
   - 优先实现 `Reset/Add/Divide` 方法，减少后续字段遗漏。

3. **修改 `ExtraStaticSearcher.h`**
   - 统计 `m_postingsTouched`；
   - 统计 `m_readPages`；
   - 统计 `m_requestedReadBytes`；
   - 在 posting loop 中统计 `m_postingElementsRaw`；
   - 在 deduper 命中处统计 `m_duplicateVectorCount`；
   - 在 ComputeDistance 前后统计 `m_distanceEvaluatedCount` 和 `m_distanceCalcLatencyMs`；
   - 拆分 `m_ioIssueLatencyMs`、`m_ioWaitLatencyMs`、`m_postingDecodeLatencyMs`、`m_postingParseLatencyMs`。

4. **修正 BATCH_READ 路径语义**
   - 优先将 BATCH_READ callback 改为只标记完成并入队；
   - decode / parse / distance compute 统一在 query thread 消费完成队列时执行；
   - 若暂不重构，则只输出 `m_batchReadTotalLatencyMs`，不得将其作为严格 `io_wait_latency_ms`。

5. **修改 `SSDIndex.h`**
   - 修复 `PrintPercentiles` 空样本问题；
   - 添加详细统计输出函数；
   - 添加可选 per-query CSV 输出；
   - 不默认输出逐 query JSON。

6. **编译验证**
   ```bash
   cd build && make -j
   ```

### 5.2 Phase 2: 系统监控脚本开发

1. **创建 `spann_io_monitor.py`**
   - 读取 `/proc/diskstats`；
   - 读取 `/proc/[pid]/io`；
   - 读取 `/proc/stat`；
   - 读取 `/proc/pressure/io`；
   - 使用 monotonic ns 记录采样窗口；
   - 输出 `disk_stats.csv`、`process_io_stats.csv`、`cpu_stats.csv`、`psi_io_stats.csv`。

2. **计算系统级指标**
   - `read_bandwidth_mbs`；
   - `instant_queue_depth`；
   - `avg_queue_depth`；
   - `peak_queue_depth`；
   - `read_bandwidth_utilization`；
   - `cpu_idle_percent`；
   - `cpu_iowait_percent`；
   - `psi_io_some_delta`；
   - `psi_io_full_delta`。

3. **支持必要参数**
   ```bash
   --device
   --pid
   --interval-ms
   --device-max-read-mbps
   --output-dir
   ```

### 5.3 Phase 3: 分析聚合脚本开发

1. **创建 `analyze_spann_io.py`**
   - 解析 SPTAG 聚合日志；
   - 解析 per-query CSV；
   - 解析系统监控 CSV；
   - 按 monotonic time window 做 join；
   - 生成 Markdown 报告和图表。

2. **计算查询级派生指标**
   - `bytes_per_scanned_element`；
   - `duplicate_vector_read_ratio`；
   - `distance_eval_ratio`；
   - `final_result_ratio`；
   - `io_wait_ratio`；
   - `read_amplification_vs_distance_eval`；
   - `read_amplification_vs_final_result`。

3. **生成关联分析**
   - latency vs requested bytes；
   - latency vs pages read；
   - latency vs raw scanned elements；
   - latency vs duplicate ratio；
   - latency vs SSD queue depth；
   - latency vs PSI；
   - QPS vs thread grid。

### 5.4 Phase 4: 线程与配置 sweep

不要只 sweep `searchThreadNum`。至少执行二维最小网格：

```text
searchThreadNum × ioThreads
```

推荐第一轮：

```text
NumberOfThreads: 固定 16
SearchThreadNum: 1, 2, 4, 8, 16, 32
IOThreadsPerHandler: 1, 2, 4, 8
```

第二轮：

```text
固定第一轮中较优的 IOThreadsPerHandler
NumberOfThreads: 4, 8, 16, 32
SearchThreadNum: 1, 2, 4, 8, 16, 32
```

每组至少重复 3 次，报告均值和波动范围。

### 5.5 Phase 5: 三路径构建验证

sync、ASYNC_READ、BATCH_READ 是编译期开关分支，不能假设一个二进制覆盖三条路径。

需要构建三套工件：

```text
build-sync:
  no ASYNC_READ, no BATCH_READ

build-async:
  ASYNC_READ, no BATCH_READ

build-batch:
  ASYNC_READ + BATCH_READ
```

三套构建使用同一测试 fixture，验证以下计数一致：

```text
postings_touched
read_pages
requested_read_bytes
posting_elements_raw
duplicate_vector_count
distance_evaluated_count
```

latency 字段不要求数值一致，但必须符合各路径定义。

---

## 六、预期输出

### 6.1 查询级统计

```text
=== Detailed I/O Statistics ===

Requested Bytes Read Per Query:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
786432  720896   917504   983040   1114112  1245184  1376256

Pages Read Per Query:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
192     176      224      240      272      304      336

Postings Touched Per Query:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
32      32       38       42       48       52       58

Raw Posting Elements Scanned Per Query:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
125000  118000   160000   184000   230000   280000   340000

Distance Evaluated Per Query:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
98000   92000    130000   150000   190000   230000   270000

Duplicate Vector Read Ratio:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
0.2160  0.2021   0.2800   0.3200   0.4100   0.4800   0.5200

Distance Eval Ratio:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
0.7840  0.7979   0.8600   0.9000   0.9600   0.9900   1.0000

I/O Wait Latency (ms):
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
12.45   10.23    18.56    24.89    45.12    78.34    156.78

Posting Decode Latency (ms):
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
1.20    1.05     1.80     2.30     4.10     6.20     8.50

Distance Calc Latency (ms):
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
8.30    7.90     11.40    14.20    25.10    38.90    52.70
```

### 6.2 系统级统计

```text
=== I/O Monitor Summary ===
duration_s: 116.5
read_bandwidth_mbs: 66.32
device_baseline_read_mbps: 3500.00
read_bandwidth_utilization: 0.0189
instant_queue_depth_avg: 3.45
avg_queue_depth_from_weighted_io_time: 7.82
peak_queue_depth: 12
process_read_bytes_delta: 8053063680
cpu_idle_percent: 72.10
cpu_iowait_percent: 8.23
psi_io_some_delta_us: 1245000
psi_io_full_delta_us: 180000
sample_count: 1165
```

### 6.3 瓶颈分析报告

```markdown
# SPANN 搜索 I/O 性能分析报告

配置: 16t_nocache

## 瓶颈分析

**I/O 等待是主要瓶颈**：P99 latency 与 per-query io_wait_latency_ms、SSD avg_queue_depth 高度相关。

**读取放大明显**：requested_read_bytes_per_query 和 posting_elements_scanned_per_query 偏高，而 final_result_ratio 很低。

**重复 VID 消耗显著**：duplicate_vector_read_ratio 较高，说明 boundary replication 带来明显重复扫描。

**设备未达到顺序带宽上限**：read_bandwidth_utilization 较低，但 avg_queue_depth 和 P99 io_wait 较高，说明问题更接近小块/随机/并发排队，而不是简单顺序带宽打满。

## 延迟分解

| 阶段 | 平均延迟 | P99 延迟 | 占比 |
|------|----------|----------|------|
| Head Index 搜索 | 70.1 ms | 100.9 ms | 33% |
| I/O issue | 0.2 ms | 0.4 ms | <1% |
| I/O wait | 12.5 ms | 45.1 ms | 58% |
| Posting decode | 1.2 ms | 4.1 ms | 3% |
| Posting parse | 2.5 ms | 8.0 ms | 6% |
| Distance calc | 8.3 ms | 25.1 ms | 20% |

## 优化建议

1. **减少读取放大**：实施两阶段 posting，先读 compact code 做粗筛。
2. **降低整表读取**：实施 posting chunk 化，让大 posting 可裁剪。
3. **控制并发 I/O**：引入 bytes-in-flight / pages-in-flight 限流。
4. **减少重复读取**：基于 VID 和 page 做 dedup，后续配合 Primary-Secondary 去重。
5. **优化线程配置**：根据 searchThreadNum × ioThreads sweep 结果选择稳定配置。
```

---

## 七、验证方法

### 7.1 功能验证

```bash
# 验证代码修改正确
./Release/ssdserving test_config.ini 2>&1 | grep "Detailed I/O Statistics"

# 验证 per-query CSV
head -n 5 results/query_io_stats.csv

# 验证监控脚本
python3 scripts/spann_io_monitor.py \
  --device nvme0n1 \
  --pid $$ \
  --interval-ms 100 \
  --device-max-read-mbps 3500 \
  --output-dir /tmp/spann_io_monitor &

sleep 2
kill $!
cat /tmp/spann_io_monitor/disk_stats.csv | head
```

### 7.2 TDD 验证

#### 7.2.1 SearchStats 单元测试

```text
SearchStats_DefaultsAreZero
SearchStats_AccumulatesRequestedBytesAsUint64
SearchStats_DerivedRatiosHandleZeroDenominator
SearchStats_ResetAddDivideCoversNewFields
```

#### 7.2.2 ExtraStaticSearcher 统计测试

构造 mock posting：

```text
posting_1: VID 1, 2, 3
posting_2: VID 3, 4, 5
```

预期：

```text
posting_elements_raw = 6
duplicate_vector_count = 1
distance_evaluated_count = 5
duplicate_vector_read_ratio = 1 / 6
```

#### 7.2.3 bytes/pages 统计测试

构造：

```text
listPageCount = 4
PageSize = 4096
```

预期：

```text
read_pages = 4
requested_read_bytes = 16384
```

#### 7.2.4 三路径构建一致性测试

分别构建：

```text
build-sync
build-async
build-batch
```

验证同一 fixture 下以下字段一致：

```text
postings_touched
read_pages
requested_read_bytes
posting_elements_raw
duplicate_vector_count
distance_evaluated_count
```

#### 7.2.5 系统监控解析测试

给定两帧 diskstats fixture：

```text
t1 sectors_read = A
t2 sectors_read = B
```

验证：

```text
read_bandwidth_mbs = (B - A) * 512 / elapsed
avg_queue_depth = delta(weighted_io_time_ms) / elapsed_ms
```

#### 7.2.6 时间窗口 join 测试

给定：

```text
query_start_ns = 1000
query_end_ns = 3000
sample_1 = [500, 1500]
sample_2 = [1500, 2500]
sample_3 = [3000, 4000]
```

验证：

```text
query 应 join sample_1 和 sample_2
sample_3 是否 join 取决于边界策略，需固定为 half-open interval [start, end)
```

#### 7.2.7 日志/CSV 解析测试

使用现有 `results/spann_search_16t_nocache.log` 和 `results/spann_search_16t_nocache.csv` 作为 fixture，验证：

```text
旧字段可解析
新字段缺失时 graceful fallback
空样本不崩溃
缺列不崩溃
```

### 7.3 结果验证

```bash
# 对比分析结果与已知问题
# 预期：16t 配置的 io_wait_latency_ms 或 batch_read_total_latency_ms 应显著高于 8t
# 预期：duplicate_vector_read_ratio 可解释一部分重复扫描
# 预期：requested_read_bytes_per_query 与 P99 latency 存在正相关
# 预期：avg_queue_depth 或 PSI io some/full 在高线程配置下上升
```

---

## 八、风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 代码修改引入 bug | 高 | 先在分支开发；新增 SearchStats、ExtraStaticSearcher、三路径构建测试 |
| BATCH_READ 延迟口径混入 decode/compute | 高 | 优先重构 callback，只标记完成；否则只输出 `batch_read_total_latency_ms` |
| query-level 与 system-level 时间戳无法对齐 | 高 | C++ 和 Python 都使用 monotonic ns；输出 start/end 时间窗口 |
| legacy `m_diskAccessCount` 跨模式误用 | 高 | 新增 `m_requestedReadBytes/m_readPages`；禁止跨模式使用 legacy 字段分析 |
| 监控开销影响性能 | 中 | 默认关闭；支持 sample rate；per-query CSV 使用内存 buffer 后 flush |
| 日志输出过多 | 中 | 不默认逐 query JSON；使用 CSV；支持采样 |
| 只 sweep searchThreadNum 导致误判 | 中 | 使用 searchThreadNum × ioThreads 二维网格，固定或单独 sweep NumberOfThreads |
| SPFresh 新字段聚合遗漏 | 中 | 若纳入 SPFresh，必须同步更新 reset/add/avg/print；否则明确本阶段仅支持 static SPANN |
| percentile 空样本崩溃 | 中 | `PrintPercentiles` 先判空，输出 NA |
| CPU iowait 被过度解释 | 中 | 仅作为辅助信号，结合 PSI、queue depth、per-query io_wait 分析 |

---

## 九、后续扩展

1. **实时可视化**：使用 Grafana + Prometheus 展示实时指标。
2. **自动化调优**：根据 QPS/P99/queue depth 自动推荐 searchThreadNum、ioThreads、NumberOfThreads 组合。
3. **对比测试**：支持 HDD vs SSD、不同 Posting 结构、不同 cache 策略的对比。
4. **接入 M1 调度器验证**：用本方案指标验证 bytes-in-flight / pages-in-flight 调度是否降低 P99。
5. **接入 M2 两阶段 posting 验证**：新增 `coarse_candidate_ratio` 和 `coarse_candidate_recall`，验证 compact code 粗筛收益。
6. **接入 M3 chunk 化验证**：新增 `chunks_total/chunks_pruned/chunks_read`，验证 chunk pruning 是否降低 scan_count 与 bytes_read。

---

## 十、任务跟踪

| 任务 ID | 描述 | 状态 |
|---------|------|------|
| #8 | 扩展 `SearchStats`，新增明确口径的 query-level 字段 | 待开始 |
| #13 | 修改 `ExtraStaticSearcher.h`，添加 requested bytes、pages、raw scan、duplicate、distance evaluated 统计 | 待开始 |
| #14 | 修正 BATCH_READ 统计语义：callback 只标记完成，或单独输出 batch total latency | 待开始 |
| #10 | 修改 `SSDIndex.h`，添加详细统计输出、per-query CSV、percentile 空样本保护 | 待开始 |
| #15 | 增加 SearchStats / ExtraStaticSearcher / 三路径构建 TDD 测试 | 待开始 |
| #11 | 创建系统级 I/O 监控脚本 `spann_io_monitor.py`，采集 diskstats、pid io、stat、PSI | 待开始 |
| #12 | 创建分析聚合脚本 `analyze_spann_io.py`，支持 monotonic window join | 待开始 |
| #9 | 创建完整测试流程脚本 `run_io_analysis.sh`，支持 searchThreadNum × ioThreads sweep | 待开始 |
| #16 | 明确 SPFresh 支持边界；如纳入则同步更新 reset/add/avg/print | 待开始 |
