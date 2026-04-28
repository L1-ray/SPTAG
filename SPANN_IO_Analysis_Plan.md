# SPANN 搜索阶段细粒度 I/O 性能分析方案

## 一、背景与目标

### 1.1 问题背景

之前的测试表明 SPANN 搜索存在 I/O 瓶颈：
- 16 线程配置下 QPS 下降 26%，延迟增长 9 倍
- 磁盘读取速率从 65 MB/s 降至 52 MB/s
- 但由于监控粒度太粗，无法定位具体瓶颈原因

### 1.2 分析目标

通过细粒度监测以下指标，找到 I/O 瓶颈的根本原因：

| 类别 | 指标 |
|------|------|
| **查询级 I/O** | bytes_read_per_query, pages_read_per_query, postings_touched_per_query, posting_elements_scanned_per_query |
| **效率指标** | useful_candidate_ratio, duplicate_vector_read_ratio |
| **系统级 I/O** | SSD queue depth, SSD read bandwidth utilization, CPU iowait/idle |
| **延迟分析** | per_query_io_wait_time, P50/P95/P99 latency |
| **性能曲线** | QPS vs thread_count curve |

---

## 二、现有代码分析

### 2.1 现有统计结构 (`IExtraSearcher.h`)

```cpp
struct SearchStats {
    int m_check;                    // 检查的节点数
    int m_exCheck;                  // 扩展检查数
    int m_totalListElementsCount;   // 总列表元素计数（已扫描的向量数）
    int m_diskIOCount;              // 磁盘 I/O 次数
    int m_diskAccessCount;          // 磁盘页访问数
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

### 2.2 现有输出 (`SSDIndex.h`)

已输出的统计：
- Ex Elements Count（m_totalListElementsCount 分布）
- Head Latency Distribution
- Ex Latency Distribution
- Total Latency Distribution
- Total Disk Page Access Distribution
- Total Disk IO Distribution

### 2.3 缺失的指标

| 指标 | 当前状态 | 需要添加 |
|------|----------|----------|
| bytes_read_per_query | ❌ | 需要从 `m_diskAccessCount * PageSize` 计算 |
| postings_touched_per_query | ❌ | 需要统计访问的 Posting List 数量 |
| useful_candidate_ratio | ❌ | 需要统计有效候选数 vs 扫描元素数 |
| duplicate_vector_read_ratio | ❌ | 需要去重统计 |
| per_query_io_wait_time | 部分 | `m_diskReadLatency` 已有，但未输出分布 |
| SSD queue depth | ❌ | 需要系统级监控 |
| CPU iowait | ❌ | 需要系统级监控 |

---

## 三、实现方案

### 3.1 方案概述

采用 **三层监控架构**：

```
┌─────────────────────────────────────────────────────────────┐
│                    Layer 1: C++ 代码层                        │
│  修改 SearchStats，添加细粒度指标收集，输出详细 JSON 日志        │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    Layer 2: 系统监控层                        │
│  Python 脚本实时采集 /proc/diskstats, /proc/stat, iostat     │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    Layer 3: 分析聚合层                        │
│  合并查询级指标 + 系统级指标，生成分析报告和可视化图表            │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 Layer 1: C++ 代码修改

#### 3.2.1 扩展 SearchStats 结构

**文件**: `AnnService/inc/Core/SPANN/IExtraSearcher.h`

```cpp
struct SearchStats {
    // === 现有字段 ===
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

    // === 新增字段 ===
    int m_postingsTouched;           // 访问的 Posting List 数量
    int m_usefulCandidates;          // 有效候选数（进入最终结果集的）
    int m_duplicateReads;            // 重复读取的向量数
    int m_totalCandidates;           // 总候选数（去重前）
    int m_uniqueCandidates;          // 去重后候选数
    int m_bytesRead;                 // 实际读取字节数
    double m_headSearchLatency;      // Head Index 搜索延迟
    double m_postingParseLatency;    // Posting List 解析延迟
    double m_distanceCalcLatency;    // 距离计算延迟
    double m_ioSubmitLatency;        // I/O 提交延迟
    double m_ioCompleteLatency;      // I/O 完成等待延迟
    
    // 初始化
    SearchStats() : m_check(0), m_exCheck(0), ...
        , m_postingsTouched(0), m_usefulCandidates(0), 
          m_duplicateReads(0), m_totalCandidates(0), m_uniqueCandidates(0),
          m_bytesRead(0), m_headSearchLatency(0), m_postingParseLatency(0),
          m_distanceCalcLatency(0), m_ioSubmitLatency(0), m_ioCompleteLatency(0) {}
};
```

#### 3.2.2 修改搜索代码收集指标

**文件**: `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`

在 `SearchIndex` 函数中添加：

```cpp
// 记录 I/O 提交时间
auto ioSubmitStart = std::chrono::steady_clock::now();

// 提交异步读取
for (uint32_t pi = 0; pi < postingListCount; ++pi) {
    // ... 现有代码 ...
    p_stats->m_postingsTouched++;
    p_stats->m_bytesRead += totalBytes;
}

auto ioSubmitEnd = std::chrono::steady_clock::now();
p_stats->m_ioSubmitLatency = std::chrono::duration<double, std::milli>(ioSubmitEnd - ioSubmitStart).count();

// 等待 I/O 完成
auto ioCompleteStart = std::chrono::steady_clock::now();
// ... 等待逻辑 ...
auto ioCompleteEnd = std::chrono::steady_clock::now();
p_stats->m_ioCompleteLatency = std::chrono::duration<double, std::milli>(ioCompleteEnd - ioCompleteStart).count();
```

#### 3.2.3 添加详细统计输出

**文件**: `AnnService/inc/SSDServing/SSDIndex.h`

在 `Search` 函数末尾添加：

```cpp
// 输出细粒度 I/O 统计
SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\n=== Detailed I/O Statistics ===\n");

// Bytes read per query
PrintPercentiles<int, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> int { return ss.m_bytesRead; },
    "%d");

// Postings touched per query
PrintPercentiles<int, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> int { return ss.m_postingsTouched; },
    "%d");

// Useful candidate ratio
SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nUseful Candidate Ratio:\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double {
        return ss.m_totalListElementsCount > 0 
            ? (double)ss.m_usefulCandidates / ss.m_totalListElementsCount 
            : 0.0;
    }, "%.4lf");

// I/O wait time breakdown
SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nI/O Submit Latency:\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double { return ss.m_ioSubmitLatency; },
    "%.3lf");

SPTAGLIB_LOG(Helper::LogLevel::LL_Info, "\nI/O Complete Latency:\n");
PrintPercentiles<double, SPANN::SearchStats>(stats,
    [](const SPANN::SearchStats& ss) -> double { return ss.m_ioCompleteLatency; },
    "%.3lf");
```

### 3.3 Layer 2: 系统级监控脚本

**文件**: `scripts/spann_io_monitor.py`

主要功能：
- 读取 `/proc/diskstats` 获取磁盘 I/O 统计和队列深度
- 读取 `/proc/stat` 获取 CPU iowait 时间
- 读取 `/proc/[pid]/io` 获取进程级 I/O 统计
- 计算带宽利用率、平均队列深度、峰值队列深度

关键数据结构：

```python
@dataclass
class DiskStats:
    """磁盘统计（来自 /proc/diskstats）"""
    timestamp: float
    device: str
    reads_completed: int      # 读完成次数
    sectors_read: int         # 读扇区数
    read_time_ms: int         # 读时间（毫秒）
    io_in_progress: int       # 正在进行的 I/O 数（队列深度）
    weighted_io_time_ms: int  # 加权 I/O 时间

@dataclass
class CPUStats:
    """CPU 统计（来自 /proc/stat）"""
    timestamp: float
    user: int
    system: int
    idle: int
    iowait: int      # I/O 等待时间

@dataclass
class ProcessIOStats:
    """进程级 I/O 统计（来自 /proc/[pid]/io）"""
    timestamp: float
    pid: int
    read_bytes: int  # 实际读取字节数
    write_bytes: int # 实际写入字节数
```

### 3.4 Layer 3: 分析聚合脚本

**文件**: `scripts/analyze_spann_io.py`

主要功能：
- 解析 SPTAG 输出日志，提取查询级统计
- 合并系统级指标（磁盘、CPU）
- 计算效率指标：useful_ratio, duplicate_ratio
- 分析延迟分解：head_latency, io_complete_latency 占比
- 生成瓶颈分析报告

---

## 四、文件清单

| 序号 | 文件路径 | 说明 |
|------|----------|------|
| 1 | `AnnService/inc/Core/SPANN/IExtraSearcher.h` | 扩展 SearchStats 结构 |
| 2 | `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h` | 添加指标收集代码 |
| 3 | `AnnService/inc/SSDServing/SSDIndex.h` | 添加详细统计输出 |
| 4 | `scripts/spann_io_monitor.py` | 系统级 I/O 监控脚本 |
| 5 | `scripts/analyze_spann_io.py` | 分析聚合脚本 |
| 6 | `scripts/run_io_analysis.sh` | 完整测试流程脚本 |

---

## 五、实施步骤

### 5.1 Phase 1: 代码修改（C++ 层）

1. **修改 `IExtraSearcher.h`**
   - 添加新的统计字段到 `SearchStats` 结构
   - 添加初始化代码

2. **修改 `ExtraStaticSearcher.h`**
   - 在 `SearchIndex` 函数中添加指标收集
   - 记录 I/O 提交/完成时间
   - 统计有效候选数

3. **修改 `SSDIndex.h`**
   - 添加详细统计输出函数
   - 输出 JSON 格式的查询级统计

4. **重新编译**
   ```bash
   cd build && make -j
   ```

### 5.2 Phase 2: 监控脚本开发

1. **创建 `spann_io_monitor.py`**
   - 实现 `/proc/diskstats` 读取
   - 实现 `/proc/stat` 读取
   - 实现 `/proc/[pid]/io` 读取
   - 计算队列深度、带宽利用率、iowait

2. **创建 `analyze_spann_io.py`**
   - 解析 SPTAG 日志
   - 合并系统级指标
   - 生成分析报告

3. **创建 `run_io_analysis.sh`**
   - 整合监控和分析流程
   - 支持不同线程配置对比

### 5.3 Phase 3: 测试验证

```bash
# 1. 运行测试（带监控）
./scripts/run_io_analysis.sh -t 8 -C

# 2. 分析结果
python3 scripts/analyze_spann_io.py \
    -l results/spann_search_8t_nocache.log \
    -d results/io_monitor/disk_stats.csv \
    -o results/analysis/8t

# 3. 对比不同线程配置
for t in 2 4 8 16; do
    ./scripts/run_io_analysis.sh -t $t -C
done
```

---

## 六、预期输出

### 6.1 查询级统计

```
=== Detailed I/O Statistics ===

Bytes Read Per Query:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
786432  720896   917504   983040   1114112  1245184  1376256

Postings Touched Per Query:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
32      32       38       42       48       52       58

Useful Candidate Ratio:
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
0.0423  0.0398   0.0512   0.0589   0.0723   0.0891   0.1234

I/O Submit Latency (ms):
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
0.152   0.134    0.198    0.256    0.412    0.689    1.234

I/O Complete Latency (ms):
Avg     50tiles  90tiles  95tiles  99tiles  99.9tiles Max
12.45   10.23    18.56    24.89    45.12    78.34    156.78
```

### 6.2 系统级统计

```
=== I/O Monitor Summary ===
duration_s: 116.5
read_bandwidth_mbs: 66.32
avg_queue_depth: 3.45
peak_queue_depth: 12
io_utilization_percent: 94.5
cpu_iowait_percent: 8.23
sample_count: 1165
```

### 6.3 瓶颈分析报告

```markdown
# SPANN 搜索 I/O 性能分析报告

配置: 8t_nocache

## 瓶颈分析

**I/O 等待是主要瓶颈**（占比 58.3%）

**读取放大严重**（有效候选比例仅 4.2%）

## 延迟分解

| 阶段 | 平均延迟 | P99 延迟 | 占比 |
|------|----------|----------|------|
| Head Index 搜索 | 70.1 ms | 100.9 ms | 33% |
| I/O 提交 | 0.2 ms | 0.4 ms | <1% |
| I/O 完成 | 12.5 ms | 45.1 ms | 58% |
| Posting 解析 | 5.3 ms | 12.3 ms | 8% |

## 优化建议

1. **减少读取放大**：实施两阶段 Posting 结构，先读 PQ Code 过滤
2. **增加并发控制**：限制同时进行的 I/O 请求数
3. **使用 SSD**：可显著降低 I/O 完成延迟
```

---

## 七、验证方法

### 7.1 功能验证

```bash
# 验证代码修改正确
./Release/ssdserving test_config.ini 2>&1 | grep "Detailed I/O Statistics"

# 验证监控脚本
python3 scripts/spann_io_monitor.py -d sda -p $$ -i 0.1 -o /tmp/test_monitor &
sleep 2
kill $!
cat /tmp/test_monitor/disk_stats.csv
```

### 7.2 结果验证

```bash
# 对比分析结果与已知问题
# 预期：16t 配置的 io_complete_latency 应显著高于 8t
# 预期：useful_ratio 应该很低（< 5%）
```

---

## 八、风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 代码修改引入 bug | 高 | 先在分支开发，充分测试后再合并 |
| 监控开销影响性能 | 中 | 使用低频采样（0.1s），可配置关闭 |
| 日志输出过多 | 低 | 使用 JSON 格式，便于解析且紧凑 |

---

## 九、后续扩展

1. **实时可视化**：使用 Grafana + Prometheus 展示实时指标
2. **自动化调优**：根据分析结果自动推荐最优线程数
3. **对比测试**：支持 HDD vs SSD、不同 Posting 结构的对比

---

## 十、任务跟踪

| 任务 ID | 描述 | 状态 |
|---------|------|------|
| #8 | 扩展 SearchStats 结构添加新指标字段 | 待开始 |
| #13 | 修改 ExtraStaticSearcher.h 添加指标收集代码 | 待开始 |
| #10 | 修改 SSDIndex.h 添加详细统计输出 | 待开始 |
| #11 | 创建系统级 I/O 监控脚本 spann_io_monitor.py | 待开始 |
| #12 | 创建分析聚合脚本 analyze_spann_io.py | 待开始 |
| #9 | 创建完整测试流程脚本 run_io_analysis.sh | 待开始 |
