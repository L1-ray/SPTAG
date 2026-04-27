# SPANN 搜索阶段查询级与设备级 I/O 观测闭环方案（修订版）

## 一、背景与目标

### 1.1 问题背景

已有测试结果显示，SPANN 搜索阶段存在明显 I/O 瓶颈：

- 16 线程配置下 QPS 下降，延迟显著增长；
- 磁盘读取速率随并发增加没有线性提升，反而下降；
- 当前日志只能看到较粗粒度的 latency、disk page、disk IO 分布，无法解释瓶颈到底来自 posting 读放大、重复 VID、SSD 队列堆积，还是 CPU 侧解析/距离计算。

因此，本方案的目标不是单纯增加几个日志字段，而是建立一个完整的 **query-level + process-level + device-level** 观测闭环。

### 1.2 修订后的目标

本方案要回答以下问题：

```text
1. 每个 query 逻辑上请求了多少 posting、page 和 bytes？
2. 每个 query 原始扫描了多少 posting 元素？
3. 其中多少元素因为重复 VID 被跳过？
4. 去重后真正参与距离计算的元素有多少？
5. 每个 query 在 I/O 等待、posting 解析、距离计算上分别花了多久？
6. 高并发时 SSD queue depth、read bandwidth、process read bytes 是否异常？
7. P99 latency 是否和 bytes_read、queue_depth、duplicate_ratio、scan_count 强相关？
8. QPS 在不同 thread_count 下从哪里开始平顶或下降？
```

核心原则：

```text
先定义指标口径，再埋点；
先保证 query 级指标正确，再和系统级指标关联；
先证明瓶颈来源，再推进 M1/M2/M3 改造。
```

---

## 二、关键修订点摘要

相对于原方案，本修订版做了以下调整：

| 原方案设计 | 修订后设计 | 原因 |
|---|---|---|
| `bytes_read = m_diskAccessCount * PageSize` | 改为 `requested_read_bytes` | 这是 C++ 查询逻辑请求字节数，不等同于设备实际物理读取字节数 |
| `m_bytesRead` 使用 `int` | 所有新增计数字段使用 `uint64_t` | bytes/pages/elements 都可能很大，避免溢出 |
| 复用 `m_totalListElementsCount` 表示扫描元素数 | 新增 `m_postingElementsRaw` | legacy 字段可能受去重逻辑影响，不适合直接表示 raw scanned count |
| `useful_candidate_ratio = final top-k / scanned` | 拆成多个 ratio | final top-k 天然很小，不能充分表达查询效率 |
| 只统计 `m_diskReadLatency` | 拆成 issue / wait / decode / distance compute | 避免把 SSD 等待、解析、计算混在一起 |
| 热路径输出 JSON | 改为聚合 percentile + 可选 per-query CSV | 避免日志格式化和写日志扰动性能 |
| 只看 `/proc/stat` iowait | 增加 `/proc/pressure/io` | iowait 只能作辅助证据，PSI 更适合观察 I/O stall |
| 只看瞬时 queue depth | 增加平均和峰值 queue depth | 瞬时值容易漏掉短时尖峰 |
| 直接实施代码 | 增加 TDD 验收 | 防止指标口径错误和三条 I/O 路径统计不一致 |

---

## 三、指标契约

### 3.1 Query-level 指标

| 指标 | 推荐字段 | 单位 | 来源 | 说明 |
|---|---|---:|---|---|
| `bytes_read_per_query` | `m_requestedReadBytes` | bytes | C++ posting read path | 查询逻辑请求读取的字节数，不等同设备实际读取字节数 |
| `pages_read_per_query` | `m_readPages` | pages | `listPageCount` 累加 | 可与 legacy `m_diskAccessCount` 对照 |
| `postings_touched_per_query` | `m_postingsTouched` | count | posting list loop | 一次 query 触达的 posting 数 |
| `posting_elements_scanned_per_query` | `m_postingElementsRaw` | count | `ProcessPosting()` 原始循环 | 必须表示原始扫描元素数，不能被 dedup 修正 |
| `distance_evaluated_per_query` | `m_distanceEvaluatedCount` | count | `ComputeDistance()` 前后 | 去重后真正计算距离的元素数 |
| `duplicate_vector_count` | `m_duplicateVectorCount` | count | `deduper.CheckAndSet()` 命中处 | 重复 VID 数 |
| `duplicate_vector_read_ratio` | `duplicateVectorCount / postingElementsRaw` | ratio | 派生指标 | 衡量边界副本导致的重复读取/解析 |
| `distance_eval_ratio` | `distanceEvaluatedCount / postingElementsRaw` | ratio | 派生指标 | 原始扫描元素中真正参与距离计算的比例 |
| `final_result_ratio` | `finalResultCount / postingElementsRaw` | ratio | 派生指标 | 最终结果占扫描元素比例，仅作辅助指标 |
| `per_query_io_wait_time` | `m_ioWaitLatencyMs` | ms | I/O completion wait | 每个 query 等待 I/O 完成的时间 |
| `posting_decode_time` | `m_postingDecodeLatencyMs` | ms | 解压/解析阶段 | 区分 SSD 等待和 CPU 解析成本 |
| `distance_calc_time` | `m_distanceCalcLatencyMs` | ms | 距离计算阶段 | 区分 I/O 瓶颈和 CPU 计算瓶颈 |
| `total_latency` | `m_totalLatency` | ms | query start/end | 用于 P50/P95/P99 统计 |

### 3.2 当前阶段不建议使用单一 `useful_candidate_ratio`

原方案中的：

```text
useful_candidate_ratio = usefulCandidates / totalListElementsCount
```

建议暂不作为核心指标，因为“最终进入 Top-K 的结果数”通常固定为 K，天然很小，不能准确反映查询过程是否有效。

在 legacy SPANN 阶段，建议使用以下三个指标替代：

```text
duplicate_vector_read_ratio
= duplicate_vector_count / posting_elements_raw

distance_eval_ratio
= distance_evaluated_count / posting_elements_raw

final_result_ratio
= final_result_count / posting_elements_raw
```

等进入 M2 两阶段 posting 后，再新增真正有意义的 coarse 指标：

```text
coarse_candidate_ratio
= rerank_candidate_count / posting_elements_raw

coarse_candidate_recall
= true_nn_in_coarse_candidates / true_nn_total
```

其中 `coarse_candidate_recall` 是两阶段 posting 的关键质量指标，因为 rerank 只能修正候选集内部排序，不能恢复粗筛阶段漏掉的真实近邻。

### 3.3 System-level 指标

| 指标 | 来源 | 计算方式 | 说明 |
|---|---|---|---|
| `device_read_bytes` | `/proc/diskstats` | `delta(sectors_read) * 512` | 设备级完成读取字节数 |
| `process_read_bytes` | `/proc/[pid]/io` | `delta(read_bytes)` | 进程级实际读取字节数 |
| `read_bandwidth_mbs` | `/proc/diskstats` | `delta_read_bytes / elapsed_seconds / 1024 / 1024` | 设备读带宽 |
| `read_bandwidth_utilization` | diskstats + baseline | `read_bandwidth_mbs / device_baseline_read_mbs` | baseline 由 fio 或设备参数提供 |
| `instant_queue_depth` | `/proc/diskstats` field 9 | `io_in_progress` | 瞬时 in-flight I/O 数 |
| `avg_queue_depth` | `/proc/diskstats` weighted time | `delta(weighted_io_time_ms) / delta(time_ms)` | 平均队列堆积程度 |
| `peak_queue_depth` | 采样窗口 | `max(io_in_progress)` | 捕捉短时尖峰 |
| `cpu_idle_percent` | `/proc/stat` | delta CPU ticks | CPU 空闲比例 |
| `cpu_iowait_percent` | `/proc/stat` | delta iowait ticks | 只能作辅助判断 |
| `psi_io_some` | `/proc/pressure/io` | parsed avg10/avg60/avg300 | 至少有任务因 I/O stall |
| `psi_io_full` | `/proc/pressure/io` | parsed avg10/avg60/avg300 | 所有非 idle 任务同时 I/O stall |

### 3.4 Latency 与吞吐指标

| 指标 | 来源 | 说明 |
|---|---|---|
| `P50/P95/P99 total_latency` | `SearchStats` 聚合 | 总延迟分布 |
| `P50/P95/P99 io_wait_latency` | `m_ioWaitLatencyMs` 聚合 | I/O 等待尾延迟 |
| `P50/P95/P99 requested_read_bytes` | `m_requestedReadBytes` 聚合 | 查询级逻辑读放大 |
| `QPS vs thread_count` | 测试脚本 | 至少覆盖 1/2/4/8/12/16 线程 |
| `latency vs queue_depth` | query CSV + diskstats join | 判断 P99 是否受 SSD 队列影响 |
| `latency vs duplicate_ratio` | query CSV | 判断边界副本是否影响查询 |
| `latency vs scan_count` | query CSV | 判断扫描量是否影响延迟 |

---

## 四、总体架构

采用三层监控架构：

```text
┌─────────────────────────────────────────────────────────────┐
│ Layer 1: C++ 查询级埋点                                       │
│ - SearchStats 扩展                                            │
│ - ExtraStaticSearcher 统计 posting/page/bytes/elements        │
│ - SSDIndex 输出 percentile + per-query CSV                    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│ Layer 2: 系统级采样                                           │
│ - /proc/diskstats                                             │
│ - /proc/[pid]/io                                              │
│ - /proc/stat                                                  │
│ - /proc/pressure/io                                           │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│ Layer 3: 分析聚合                                             │
│ - query-level CSV                                             │
│ - system-level CSV                                            │
│ - 按 start_ns/end_ns 做时间窗口 join                          │
│ - 输出瓶颈报告和 QPS 曲线                                      │
└─────────────────────────────────────────────────────────────┘
```

---

## 五、Layer 1：C++ 查询级埋点

### 5.1 修改文件清单

| 文件 | 修改内容 |
|---|---|
| `AnnService/inc/Core/SPANN/IExtraSearcher.h` | 扩展 `SearchStats` 字段与派生指标函数 |
| `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h` | 统计 posting/page/bytes/raw elements/duplicate/distance eval/latency |
| `AnnService/inc/SSDServing/SSDIndex.h` | 输出新增 percentile 与 per-query CSV |
| `AnnService/inc/Core/SPANN/Options.h` | 增加开关与输出路径参数，若项目参数体系允许 |
| `AnnService/inc/Core/SPANN/ParameterDefinitionList.h` | 注册新增参数，若项目参数体系允许 |

动态路径 `ExtraDynamicSearcher.h` 可作为第二阶段补齐。本方案第一阶段优先覆盖静态 SPANN 搜索路径。

### 5.2 `SearchStats` 推荐字段

保留现有字段，同时新增字段全部使用 `uint64_t` 或 `double`：

```cpp
struct SearchStats {
    // === legacy fields: keep unchanged ===
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

    // === new query-level I/O stats ===
    uint64_t m_queryId;
    uint64_t m_queryStartNs;
    uint64_t m_queryEndNs;

    uint64_t m_postingsTouched;
    uint64_t m_readPages;
    uint64_t m_requestedReadBytes;

    // === posting scan stats ===
    uint64_t m_postingElementsRaw;
    uint64_t m_distanceEvaluatedCount;
    uint64_t m_duplicateVectorCount;

    // === candidate/result stats ===
    uint64_t m_finalResultCount;
    uint64_t m_truthHitCount;             // optional, only when ground truth is available
    uint64_t m_rerankCandidateCount;      // future M2 two-stage posting

    // === latency breakdown ===
    double m_ioIssueLatencyMs;
    double m_ioWaitLatencyMs;
    double m_postingDecodeLatencyMs;
    double m_distanceCalcLatencyMs;

    // === derived metrics ===
    double DuplicateVectorReadRatio() const {
        return m_postingElementsRaw == 0 ? 0.0 :
            static_cast<double>(m_duplicateVectorCount) /
            static_cast<double>(m_postingElementsRaw);
    }

    double DistanceEvalRatio() const {
        return m_postingElementsRaw == 0 ? 0.0 :
            static_cast<double>(m_distanceEvaluatedCount) /
            static_cast<double>(m_postingElementsRaw);
    }

    double FinalResultRatio() const {
        return m_postingElementsRaw == 0 ? 0.0 :
            static_cast<double>(m_finalResultCount) /
            static_cast<double>(m_postingElementsRaw);
    }

    double BytesPerScannedElement() const {
        return m_postingElementsRaw == 0 ? 0.0 :
            static_cast<double>(m_requestedReadBytes) /
            static_cast<double>(m_postingElementsRaw);
    }

    double IoWaitRatio() const {
        return m_totalLatency <= 0.0 ? 0.0 :
            m_ioWaitLatencyMs / m_totalLatency;
    }
};
```

### 5.3 统计口径要求

#### 5.3.1 posting/page/bytes

在每次准备读取 posting 时统计：

```cpp
p_stats->m_postingsTouched += 1;
p_stats->m_readPages += listInfo->listPageCount;
p_stats->m_requestedReadBytes +=
    static_cast<uint64_t>(listInfo->listPageCount) << PageSizeEx;
```

注意：

```text
m_requestedReadBytes 是 C++ 查询逻辑请求字节数；
不能把它命名为 physical_read_bytes；
实际设备读取字节数应由 /proc/diskstats 或 /proc/[pid]/io 统计。
```

#### 5.3.2 raw scanned elements

进入 `ProcessPosting()` 时，应把 posting 原始元素数统计到新字段：

```cpp
p_stats->m_postingElementsRaw += listInfo->listEleCount;
```

或者在循环中逐条累加：

```cpp
for (...) {
    p_stats->m_postingElementsRaw++;
    ...
}
```

两种方式要确保不被 dedup 修正。

#### 5.3.3 duplicate VID

在 deduper 命中处统计：

```cpp
if (p_exWorkSpace->m_deduper.CheckAndSet(vectorID)) {
    p_stats->m_duplicateVectorCount++;
    continue;
}
```

#### 5.3.4 distance evaluated

只有真正进入距离计算时才统计：

```cpp
p_stats->m_distanceEvaluatedCount++;
float dist = ComputeDistance(...);
```

#### 5.3.5 latency 拆分

建议拆成四类：

```text
m_ioIssueLatencyMs       提交 I/O 请求的耗时
m_ioWaitLatencyMs        等待 I/O 完成的耗时
m_postingDecodeLatencyMs 解压 / 解析 posting 的耗时
m_distanceCalcLatencyMs  距离计算耗时
```

同步、异步、batch read 三条路径的语义需保持一致：

| 读取路径 | `ioIssueLatencyMs` | `ioWaitLatencyMs` |
|---|---|---|
| sync read | 可为 0 或极小 | `ReadBinary` wall time |
| async read | submit request wall time | submit 完成到最后一个 completion 被消费的时间 |
| batch read | batch submit wall time | batch read completion wall time |

### 5.4 输出方式

#### 5.4.1 聚合 percentile 输出

继续在 `SSDIndex.h` 中输出 percentile，新增以下分布：

```text
Requested Bytes Read Per Query
Pages Read Per Query
Postings Touched Per Query
Raw Posting Elements Scanned Per Query
Distance Evaluated Per Query
Duplicate Vector Count Per Query
Duplicate Vector Read Ratio
Distance Eval Ratio
Bytes Per Scanned Element
I/O Wait Latency
Posting Decode Latency
Distance Calc Latency
I/O Wait Ratio
```

#### 5.4.2 per-query CSV 输出

不要在热路径逐 query 打 JSON 日志。建议输出 CSV，并支持 sample rate。

新增配置：

```text
EnableDetailedIOStats=false
DetailedIOStatsOutput=results/query_io_stats.csv
DetailedIOStatsSampleRate=1.0
```

CSV 字段建议：

```csv
run_id,thread_count,query_id,thread_id,start_ns,end_ns,total_latency_ms,head_latency_ms,ex_latency_ms,io_issue_ms,io_wait_ms,posting_decode_ms,distance_calc_ms,postings_touched,pages_read,requested_read_bytes,posting_elements_raw,distance_evaluated_count,duplicate_vector_count,duplicate_vector_read_ratio,distance_eval_ratio,bytes_per_scanned_element,final_result_count,recall
```

实现建议：

```text
1. 查询线程写入内存 buffer；
2. 测试结束后统一 flush；
3. 支持 sample_rate；
4. 默认关闭；
5. 不在高并发热路径直接频繁调用日志宏输出长 JSON。
```

---

## 六、Layer 2：系统级监控脚本

### 6.1 脚本文件

```text
scripts/spann_io_monitor.py
```

### 6.2 采集来源

| 来源 | 用途 |
|---|---|
| `/proc/diskstats` | device-level read bytes、queue depth、weighted I/O time |
| `/proc/[pid]/io` | process-level read bytes |
| `/proc/stat` | CPU idle / iowait 辅助信号 |
| `/proc/pressure/io` | I/O stall 压力 |

### 6.3 命令行参数

```bash
python3 scripts/spann_io_monitor.py \
  --device nvme0n1 \
  --pid <SPTAG_PID> \
  --interval-ms 100 \
  --device-max-read-mbps 3500 \
  --output-dir results/io_monitor
```

参数说明：

| 参数 | 说明 |
|---|---|
| `--device` | 监控的磁盘设备，例如 `nvme0n1` 或 `sda` |
| `--pid` | SPTAG 搜索进程 PID |
| `--interval-ms` | 采样间隔，建议 50ms 到 200ms |
| `--device-max-read-mbps` | 设备读带宽基线，用于计算 utilization |
| `--output-dir` | 输出 CSV 目录 |

### 6.4 数据结构

```python
@dataclass
class DiskStats:
    timestamp_ns: int
    device: str
    reads_completed: int
    sectors_read: int
    read_time_ms: int
    io_in_progress: int
    weighted_io_time_ms: int

@dataclass
class ProcessIOStats:
    timestamp_ns: int
    pid: int
    read_bytes: int
    write_bytes: int

@dataclass
class CPUStats:
    timestamp_ns: int
    user: int
    system: int
    idle: int
    iowait: int

@dataclass
class PSIStats:
    timestamp_ns: int
    io_some_avg10: float
    io_some_avg60: float
    io_some_avg300: float
    io_some_total: int
    io_full_avg10: float
    io_full_avg60: float
    io_full_avg300: float
    io_full_total: int
```

### 6.5 计算指标

```python
read_bytes = delta(sectors_read) * 512
read_bandwidth_mbs = read_bytes / elapsed_seconds / 1024 / 1024
read_bandwidth_utilization = read_bandwidth_mbs / device_max_read_mbps

instant_queue_depth = io_in_progress
avg_queue_depth = delta(weighted_io_time_ms) / elapsed_ms
peak_queue_depth = max(io_in_progress samples)

process_read_bytes_delta = delta(process.read_bytes)
```

CPU iowait 只能作为辅助信号：

```python
cpu_iowait_percent = delta(iowait_ticks) / delta(total_cpu_ticks)
cpu_idle_percent = delta(idle_ticks) / delta(total_cpu_ticks)
```

I/O stall 建议看 PSI：

```text
/proc/pressure/io some avg10/avg60/avg300
/proc/pressure/io full avg10/avg60/avg300
```

### 6.6 输出文件

```text
results/io_monitor/disk_stats.csv
results/io_monitor/process_io.csv
results/io_monitor/cpu_stats.csv
results/io_monitor/psi_io.csv
results/io_monitor/summary.json
```

---

## 七、Layer 3：分析聚合脚本

### 7.1 脚本文件

```text
scripts/analyze_spann_io.py
```

### 7.2 输入

```bash
python3 scripts/analyze_spann_io.py \
  --query-stats results/query_io_stats.csv \
  --disk-stats results/io_monitor/disk_stats.csv \
  --process-io results/io_monitor/process_io.csv \
  --cpu-stats results/io_monitor/cpu_stats.csv \
  --psi-io results/io_monitor/psi_io.csv \
  --output-dir results/analysis/16t_nocache
```

### 7.3 关联方式

query-level CSV 必须包含：

```text
start_ns
end_ns
run_id
thread_count
query_id
```

系统级 CSV 必须包含：

```text
timestamp_ns
```

分析脚本按 query 的 `[start_ns, end_ns]` 时间窗口聚合系统采样点：

```text
query_window_disk_avg_queue_depth
query_window_disk_peak_queue_depth
query_window_read_bandwidth_mbs
query_window_process_read_bytes_delta
query_window_cpu_iowait_percent
query_window_psi_io_some_avg10
```

### 7.4 输出分析

输出以下报告：

```text
1. Query latency percentile
2. Requested bytes/page/posting percentile
3. Raw scanned elements percentile
4. Duplicate ratio percentile
5. Distance eval ratio percentile
6. I/O wait latency percentile
7. Latency breakdown
8. Device read bandwidth summary
9. Queue depth summary
10. QPS vs thread_count curve
11. Bottleneck diagnosis
```

建议额外输出相关性分析：

```text
corr(total_latency_ms, requested_read_bytes)
corr(total_latency_ms, posting_elements_raw)
corr(total_latency_ms, duplicate_vector_read_ratio)
corr(total_latency_ms, io_wait_ms)
corr(total_latency_ms, avg_queue_depth)
corr(io_wait_ms, avg_queue_depth)
```

---

## 八、实施步骤

### 8.1 Phase 0：指标契约先行

目标：先把每个指标的定义、单位、来源写清楚。

交付物：

```text
1. 本文档中的指标契约落地到代码注释或 README；
2. SearchStats 新字段命名确认；
3. CSV 输出 schema 确认；
4. 系统监控 CSV schema 确认。
```

验收：

```text
任意一个指标都能回答：
- 来源是什么？
- 单位是什么？
- 是否 query-level？
- 是否 device-level？
- 是否派生指标？
- 是否受缓存或 O_DIRECT 影响？
```

### 8.2 Phase 1：C++ 最小埋点

优先新增：

```text
m_postingsTouched
m_readPages
m_requestedReadBytes
m_postingElementsRaw
m_distanceEvaluatedCount
m_duplicateVectorCount
m_ioIssueLatencyMs
m_ioWaitLatencyMs
m_postingDecodeLatencyMs
m_distanceCalcLatencyMs
m_queryStartNs
m_queryEndNs
```

暂不新增含义不清的 `m_usefulCandidates`。

验收：

```text
1. legacy 查询结果不变；
2. 新字段默认关闭时不输出；
3. 开启后 stats 中字段非负；
4. sync / async / batch read 三条路径的核心计数一致。
```

### 8.3 Phase 2：聚合输出与 per-query CSV

交付物：

```text
1. SSDIndex.h 新增 percentile 输出；
2. query_io_stats.csv 输出；
3. sample rate；
4. 默认关闭；
5. 测试结束统一 flush。
```

验收：

```text
1. 16t 测试时不会因日志输出明显扰动 QPS；
2. CSV 可被 pandas / awk 正常解析；
3. 旧日志格式兼容，分析脚本遇到新字段缺失时 graceful fallback。
```

### 8.4 Phase 3：系统级监控脚本

交付物：

```text
scripts/spann_io_monitor.py
```

采集：

```text
/proc/diskstats
/proc/[pid]/io
/proc/stat
/proc/pressure/io
```

验收：

```text
1. 能输出 disk_stats.csv、process_io.csv、cpu_stats.csv、psi_io.csv；
2. 能计算 read_bandwidth_mbs；
3. 能计算 instant / avg / peak queue depth；
4. 能计算 read_bandwidth_utilization；
5. 支持 device baseline 参数。
```

### 8.5 Phase 4：分析聚合脚本

交付物：

```text
scripts/analyze_spann_io.py
```

验收：

```text
1. 能读取 query_io_stats.csv；
2. 能读取系统监控 CSV；
3. 能按 query start/end 时间窗口 join；
4. 能输出 markdown 分析报告；
5. 能输出 QPS vs thread_count 汇总表；
6. 能输出 latency 与 I/O 指标的相关性。
```

### 8.6 Phase 5：多线程 sweep

> **硬件约束**：测试机器为 8 核 16 线程，最大测试线程数不应超过 16，否则线程调度开销会干扰测量结果。

推荐测试线程：

```text
1, 2, 4, 8, 12, 16
```

测试线程选择说明：

| 线程数 | 说明 |
|--------|------|
| 1 | 单线程基线，排除并发干扰 |
| 2, 4 | 低并发区域 |
| 8 | 物理核心数，预期 QPS 峰值附近 |
| 12 | 超线程区域，观察 QPS 变化趋势 |
| 16 | 逻辑核心数上限，观察是否有性能下降 |

每组至少重复 3 次：

```text
for t in 1 2 4 8 12 16; do
    for r in 1 2 3; do
        ./scripts/run_io_analysis.sh --threads $t --repeat $r --no-cache
    done
done
```

验收：

```text
1. 能定位 QPS 平顶点（预期在 8-12 线程附近）；
2. 能确认 P99 latency 是否随 queue depth 增长；
3. 能确认 requested_read_bytes 是否随 thread_count 变化；
4. 能确认 duplicate_vector_read_ratio 是否显著；
5. 能确认 io_wait_ratio 是否是高并发下的主导因素；
6. 能确认 16 线程时是否因线程调度开销导致性能下降。
```

---

## 九、TDD 验收计划

### 9.1 SearchStats 单元测试

```text
SearchStats_DefaultsAreZero
SearchStats_AccumulatesRequestedBytesAsUint64
SearchStats_AccumulatesPagesAndPostings
SearchStats_DerivedRatiosHandleZeroDenominator
SearchStats_DerivedRatiosAreCorrect
```

### 9.2 ExtraStaticSearcher 统计测试

构造小型 mock posting：

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
distance_eval_ratio = 5 / 6
```

### 9.3 bytes/pages 统计测试

输入：

```text
listPageCount = 4
PageSize = 4096
```

预期：

```text
read_pages = 4
requested_read_bytes = 16384
```

### 9.4 sync / async / batch read 路径一致性测试

覆盖：

```text
sync read path
ASYNC_READ path
BATCH_READ path
```

要求相同输入下以下字段一致：

```text
postings_touched
read_pages
requested_read_bytes
posting_elements_raw
duplicate_vector_count
distance_evaluated_count
```

latency 字段不要求完全一致，但必须非负且语义清楚。

### 9.5 diskstats 解析测试

给定两帧 fixture：

```text
t1 sectors_read = A
t2 sectors_read = B
```

验证：

```text
read_bytes = (B - A) * 512
read_bandwidth_mbs = read_bytes / elapsed_seconds / 1024 / 1024
avg_queue_depth = delta(weighted_io_time_ms) / elapsed_ms
```

### 9.6 CSV 解析测试

使用已有结果文件作为 fixture：

```text
results/spann_search_16t_nocache.log
results/spann_search_16t_nocache.csv
```

要求：

```text
1. 旧格式可以解析；
2. 新字段缺失时 graceful fallback；
3. 新 query_io_stats.csv 可以完整解析；
4. join 系统监控数据时不因缺失采样点崩溃。
```

---

## 十、预期输出

### 10.1 查询级聚合输出示例

```text
=== Detailed Query I/O Statistics ===

Requested Bytes Read Per Query:
Avg     P50      P90      P95      P99      P99.9    Max
786432  720896   917504   983040   1114112  1245184  1376256

Pages Read Per Query:
Avg     P50      P90      P95      P99      P99.9    Max
192     176      224      240      272      304      336

Postings Touched Per Query:
Avg     P50      P90      P95      P99      P99.9    Max
32      32       38       42       48       52       58

Raw Posting Elements Scanned Per Query:
Avg     P50      P90      P95      P99      P99.9    Max
120000  110000   150000   170000   230000   310000   400000

Duplicate Vector Read Ratio:
Avg     P50      P90      P95      P99      P99.9    Max
0.081   0.060    0.140    0.180    0.260    0.330    0.410

I/O Wait Latency ms:
Avg     P50      P90      P95      P99      P99.9    Max
12.45   10.23    18.56    24.89    45.12    78.34    156.78
```

### 10.2 系统级输出示例

```text
=== I/O Monitor Summary ===
duration_s: 116.5
read_bandwidth_mbs_avg: 66.32
read_bandwidth_mbs_p95: 88.10
read_bandwidth_utilization_avg: 0.019
instant_queue_depth_avg: 3.45
instant_queue_depth_peak: 12
avg_queue_depth: 5.82
process_read_bytes_total: 8123456789
cpu_iowait_percent_avg: 8.23
psi_io_some_avg10_max: 14.20
psi_io_full_avg10_max: 2.10
sample_count: 1165
```

### 10.3 分析报告示例

```markdown
# SPANN 搜索 I/O 性能分析报告

配置：16t_nocache

## 结论

I/O 等待是主要瓶颈。P99 total latency 与 io_wait_latency、avg_queue_depth 强相关。

## 关键证据

| 指标 | 值 |
|---|---:|
| P99 total latency | 156.78 ms |
| P99 io_wait_latency | 78.34 ms |
| avg queue depth | 5.82 |
| peak queue depth | 12 |
| requested bytes/query P99 | 1.06 MB |
| duplicate vector read ratio P95 | 18.0% |
| distance eval ratio P50 | 94.0% |

## 解释

当前 query 级 requested bytes 较高，同时高并发下 queue depth 上升，说明搜索线程向底层设备提交了大量 posting/page 读取请求。duplicate ratio 显示边界副本也带来了重复读取和解析成本。

## 建议

1. M1：优先引入全局 I/O 调度，限制 bytes-in-flight 和 pages-in-flight。
2. M1：增加 page-level dedup 与细粒度 cache。
3. M2：推进两阶段 posting，降低每条候选的查询期字节数。
4. M3：推进 posting chunk 化，降低整张 posting 不可裁剪的问题。
```

---

## 十一、风险与缓解

| 风险 | 影响 | 缓解措施 |
|---|---|---|
| 指标口径不一致 | 误判瓶颈 | 先落地指标契约和 TDD |
| 使用 `int` 统计 bytes/elements | 溢出或截断 | 新字段统一使用 `uint64_t` |
| 逐 query JSON 日志影响性能 | 扰动测量结果 | 改为 per-query CSV + buffer + sample rate |
| 只看 iowait 误判 CPU/IO 状态 | 结论不可靠 | 增加 PSI 和 query-level io_wait |
| sync/async/batch 路径统计不一致 | 数据不可比 | TDD 覆盖三条路径 |
| diskstats 被其他进程污染 | 设备级指标偏差 | 同时采集 `/proc/[pid]/io`，测试机器尽量隔离 |
| 设备带宽 utilization 无基线 | 无法解释高低 | 提供 `--device-max-read-mbps` 或 fio baseline |

---

## 十二、任务拆分

| 任务 ID | 描述 | 优先级 | 依赖 |
|---|---|---:|---|
| IO-0 | 确认指标契约和 CSV schema | P0 | 无 |
| IO-1 | 扩展 `SearchStats` 新字段与派生指标 | P0 | IO-0 |
| IO-2 | 在 `ExtraStaticSearcher.h` 添加 posting/page/bytes 统计 | P0 | IO-1 |
| IO-3 | 在 `ProcessPosting()` 添加 raw scan / duplicate / distance eval 统计 | P0 | IO-1 |
| IO-4 | 添加 I/O issue/wait/decode/distance latency 拆分 | P0 | IO-1 |
| IO-5 | 在 `SSDIndex.h` 增加 percentile 输出 | P1 | IO-1~IO-4 |
| IO-6 | 增加 per-query CSV 输出 | P1 | IO-1~IO-4 |
| IO-7 | 创建 `spann_io_monitor.py` | P1 | IO-0 |
| IO-8 | 创建 `analyze_spann_io.py` | P1 | IO-6, IO-7 |
| IO-9 | 创建 `run_io_analysis.sh` | P2 | IO-6, IO-7, IO-8 |
| IO-10 | 多线程 sweep：1/2/4/8/12/16 | P2 | IO-9 |
| IO-11 | 输出最终瓶颈报告 | P2 | IO-10 |

---

## 十三、最终落地原则

本方案的最终目标是让 SPANN 搜索阶段的 I/O 问题可以被精确解释：

```text
查询逻辑请求了多少数据；
设备实际读了多少数据；
posting 中扫描了多少元素；
重复 VID 消耗了多少；
去重后真正计算了多少距离；
每个 query 等 I/O 等了多久；
SSD 队列在高并发下是否堆积；
P99 latency 和 QPS 平顶到底由哪些指标驱动。
```

一句话总结：

> 本修订版把原来的“细粒度 I/O 性能分析方案”升级为“查询级与设备级 I/O 观测闭环方案”。它不仅记录更多指标，更重要的是修正指标口径，区分逻辑请求字节数和设备实际读取字节数，区分原始扫描元素和去重后计算元素，并通过系统级采样与 TDD 验收保证后续 M1/M2/M3 改造有可靠依据。
