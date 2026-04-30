# SPANN 问题分析与后续执行方案（2026-04-30 更新版）

> 本文档基于当前 SIFT10M I/O 分析结果，对 `SPANN_Problem.md` 的执行口径进行修正。结论不是推翻原方案，而是保留“查询级指标 + 系统级 I/O 采样 + 后处理分析”的大方向，同时修正指标语义、实验矩阵和结论强度。

> 旧版文档已保留为 `SPANN_Problem_old_20260430.md`。

---

## 1. 总结论

**不能完全按原来的设想继续执行，需要修改，但不需要推翻方案。**

原方案的大方向仍然成立：

```text
查询级指标
+ 系统级 I/O 采样
+ 后处理分析
```

这仍然符合 M0 观测闭环的要求，也仍然是后续评估 M1 / M2 / M3 收益的前提。

需要修改的是：

```text
1. 不能再把当前 BATCH_READ 路径下的 io_wait_ms 当成纯 I/O 等待时间。
2. 不能只基于 SIFT10M 单点结果直接判断 CPU 调度、锁竞争、st=8 最优或 nt=16 足够。
3. 不能把当前 1.6 GB/s 读带宽直接解释为“接近 NVMe 硬件上限”。
4. 不能把 distance_eval_ratio 解释成“有效距离计算比例”。
```

当前更准确的执行策略应为：

```text
保留原方案的三层监控架构；
修改指标语义；
先承认当前官方代码可稳定测试的主路径只有 BATCH_READ 默认模式；
把当前 BATCH_READ 结果用于端到端瓶颈定位；
把非 BATCH_READ / 同步读取作为“需要先修复代码后才能启用”的补充诊断路径；
再决定 M1 / M2 / M3 的优先级。
```

---

## 2. 哪些内容可以保留

### 2.1 M0 观测闭环必须保留

原方案中先补齐查询级指标的思路仍然正确。后续无论做两阶段 posting、posting chunk 化，还是全局 I/O 调度，都必须依赖统一的观测闭环来判断收益。

仍需保留的核心查询级指标包括：

```text
query_id
visited_heads
fetched_postings
fetched_pages
bytes_read
scan_count
duplicate_vid_count
io_wait_time
cpu_distance_time
final_recall
```

但这些指标需要**按执行路径重新定义口径**，否则不同实现路径下的同名指标会混淆。

建议改成：

```text
bytes_read
  -> requested_read_bytes
  -> device_read_bytes
  -> process_read_bytes

io_wait_time
  -> non_batch_io_wait_time
  -> batch_read_total_time
  -> sync_read_time
```

### 2.2 查询级读放大指标必须保留

当前 SIFT10M 结果已经足以支撑一个明确判断：

```text
avg requested read bytes ≈ 515,772 bytes/query
avg pages read ≈ 125.9/query
postings touched = 64/query
raw posting elements scanned ≈ 2,955.6/query
final result ratio ≈ 0.0036
```

这说明当前搜索阶段存在明显的：

```text
posting 读放大
+ 扫描放大
```

因此，以下指标仍然应作为主指标继续保留：

```text
requested_read_bytes_per_query
pages_read_per_query
postings_touched_per_query
posting_elements_scanned_per_query
duplicate_vector_read_ratio
final_result_ratio
```

### 2.3 M2 / M3 方向仍然成立

原方案里提出的两个结构性方向仍然正确：

```text
M2：两阶段 posting，降低每条向量查询期字节数；
M3：posting chunk 化，降低整张 posting 不可裁剪的问题。
```

SIFT10M 当前结果并没有削弱这两个方向，反而进一步支持：

```text
requested bytes/query 仍然偏高；
scanned elements/query 仍然偏高；
final_result_ratio 很低；
```

因此：

- M2 仍然是降低 payload 读取成本的主线；
- M3 仍然是降低整表扫描和整表读取的主线。

---

## 3. 必须修改的部分

### 3.1 不能再把 BATCH_READ 下的 `io_wait_ms` 当作纯 I/O wait

这是当前最重要的修正点。

在 BATCH_READ 路径下，当前 Ex 阶段观测到：

```text
Ex Latency ≈ 1.136 ms
Batch Read Total ≈ 1.119 ms
Posting Parse ≈ 0.094 ms
Distance Calc ≈ 0.089 ms
```

这说明 Ex 阶段绝大部分时间落在 `Batch Read Total` 中。

但 `Batch Read Total` 并不等于纯 I/O 等待时间。它实际混合了：

```text
I/O submit
I/O wait
completion handling
callback
posting parse
ProcessPosting
distance calculation
```

因此，后续分析中不能再把它直接命名为 `io_wait_ms`，更不能仅凭它判断“纯 SSD 等待是否主导”。

### 3.2 当前结果不能证明 CPU 调度或锁竞争是瓶颈

当前系统级观测显示：

```text
CPU iowait ≈ 0.41%
CPU idle ≈ 83.0%
```

这组指标并不支持以下强结论：

```text
CPU 总体算力不足；
搜索线程竞争 CPU 时间片；
锁竞争已经是主要瓶颈；
```

仍然可能存在局部问题，例如：

```text
单线程热点
callback 局部拥堵
queue lock contention
workspace acquire wait
futex wait
```

但这些都需要额外证据，例如：

```text
perf record
off-CPU profile
mutex/futex wait
context switch rate
run queue length
per-thread CPU utilization
queue wait time
```

因此，CPU 调度 / 锁竞争在当前阶段只能写成：

```text
待验证假设
```

而不能作为当前报告的确定结论。

### 3.3 当前结果不能证明 st=8 最优、nt=16 足够

当前 SIFT10M 只是单点配置：

```text
SearchThreadNum = 8
NumberOfThreads = 16
InternalResultNum = 64
SearchPostingPageLimit = 4
```

这不能支撑如下结论：

```text
st=8 已最优
nt=16 已足够
继续增加线程无益
```

此前 SIFT1M sweep 的趋势只能作为参考，不能直接外推到 SIFT10M。

### 3.4 当前结果不能证明 1608 MB/s 接近 NVMe 硬件上限

当前观测为：

```text
read bandwidth ≈ 1,608.6 MB/s
avg queue depth ≈ 102.9
peak queue depth ≈ 278
```

这说明当前 workload 下 I/O 压力不低，但不能直接推出：

```text
已接近设备硬件上限
```

更准确的表述应该是：

```text
当前 SPANN workload 在 st=8 下形成约 1.6 GB/s 的读带宽平台；
是否接近硬件上限，需要 fio 顺序读 / 随机读 / 混合块大小基准确认。
```

### 3.5 `distance_eval_ratio` 的解释必须修正

`distance_eval_ratio = 0.8805` 更准确的语义是：

```text
去重后仍然进入 ComputeDistance 的比例
```

它不等于：

```text
有效距离计算比例
```

真正反映最终有效性的，是：

```text
final_result_ratio = final_topk / raw_scanned_elements
```

因此应改成：

```text
distance_eval_ratio:
  去重后进入距离计算的比例；
  不代表最终有效候选比例。

final_result_ratio:
  最终 Top-K 结果 / raw scanned elements；
  用于衡量扫描放大。
```

---

## 4. 调整后的指标设计

### 4.1 查询级结构性指标

这组指标在 BATCH_READ、非 BATCH_READ、同步读取三条路径下都应保持一致：

```text
requested_read_bytes_per_query
pages_read_per_query
postings_touched_per_query
posting_elements_scanned_per_query
distance_evaluated_count
duplicate_vector_count
duplicate_vector_read_ratio
final_result_ratio
```

它们用于回答：

```text
当前 query 到底读了多少字节；
扫描了多少 posting 元素；
重复读取有多严重；
最终有效结果占比有多低。
```

### 4.2 BATCH_READ 专用延迟指标

BATCH_READ 路径不再使用 `io_wait_ms` 作为命名，而改为：

```text
batch_read_total_ms
batch_submit_ms
batch_wait_and_callback_ms
batch_posting_parse_ms
batch_distance_calc_ms
```

如果暂时无法继续拆分，也必须明确说明：

```text
batch_read_total_ms 是混合指标，不等价于纯 io_wait_ms。
```

### 4.3 非 BATCH_READ 专用延迟指标

**理想情况下**，非 BATCH_READ 路径最适合做真正的 per-request I/O wait 分析：

```text
io_issue_ms
io_wait_ms
completion_consume_delay_ms
posting_parse_ms
distance_calc_ms
```

这组指标原本用于回答：

```text
纯 SSD wait 是否主导；
IOThreads 是否不足；
completion 消费是否拥堵；
parse / distance 是否才是主成本。
```

但根据 `TEST_COMMANDS.md` 9.7 节，**当前官方 Linux 代码下该模式不能直接编译通过**：关闭 `BATCH_READ` 后会进入未修复分支，调用与 Linux `RequestQueue` 不匹配的 `m_processIocp.pop(...)`。因此这组指标目前只能作为**目标口径**，不能直接当作现阶段可执行测试项。

### 4.4 同步读取 baseline 指标

**理想情况下**，同步路径可提供最基础的串行读取口径：

```text
sync_read_ms
posting_parse_ms
distance_calc_ms
```

但同样根据 9.7 节，**当前官方代码关闭 `ASYNC_READ` 后也不能直接编译通过**，会进入未充分维护的同步分支并触发 `ExtraStaticSearcher.h` 编译错误。因此同步模式当前也应视为**需要先修代码才能启用的对照路径**。

### 4.5 系统级指标

系统级指标继续保留，但解释需要谨慎：

```text
read_bandwidth_mbs
avg_queue_depth
peak_queue_depth
process_read_bytes_delta
cpu_idle
cpu_iowait
psi_io_some
psi_io_full
```

同时补入设备基准：

```text
fio_sequential_read_baseline
fio_random_read_baseline
fio_mixed_blocksize_read_baseline
device_max_read_mbps
spann_workload_read_mbps
spann_read_bandwidth_utilization
```

---

## 5. 调整后的实验矩阵

## 5.1 三套构建的正确理解与当前可执行范围

### A. BATCH_READ：当前唯一稳定可测的官方主路径

```text
ASYNC_READ = ON
BATCH_READ = ON
```

这是当前默认配置，也是**当前官方代码下可稳定编译和测试的主路径**。

用途：

```text
评估当前实际路径下的 QPS / P99 / batch_read_total_ms / requested bytes；
观察 Ex / Batch Read + posting processing 的端到端瓶颈；
分析 SearchThreadNum、NumberOfThreads、InternalResultNum 对默认路径的影响。
```

额外注意两点：

```text
1. BATCH_READ 是通过 #ifdef 判断是否“定义”，不是判断值；
   因此 #define BATCH_READ 0 仍然会进入 BATCH_READ 分支。
   正确切换方式是注释掉宏或使用 #undef。

2. 在默认批量异步模式下，IOThreads 不是主要参数；
   NumberOfThreads 才是更关键的 I/O 相关参数，因为它影响 AIO context / maxNumBlocks / workspace 等行为。
```

### B. 非 BATCH_READ：概念上重要，但当前官方 Linux 代码不可直接测试

```text
ASYNC_READ = ON
BATCH_READ = OFF
```

理论用途：

```text
拆 io_issue / io_wait / completion delay / parse / distance。
```

但根据 `TEST_COMMANDS.md` 9.7 节，这一路径在当前官方 Linux 代码下**不能直接编译通过**。因此它暂时不能作为“下一步立刻补跑”的实验项，除非先修复对应非批量异步分支。

### C. 同步读取：概念上的 baseline，但当前官方代码也不可直接测试

```text
ASYNC_READ = OFF
BATCH_READ = OFF
```

理论用途：

```text
验证基础计数；
验证单 query 串行读取成本；
对照异步路径是否真的改善端到端行为。
```

但根据 9.7 节，这一路径在当前官方代码下同样**不能直接编译通过**。因此它也不是现阶段可直接执行的 baseline，而是后续若修复同步分支后才可启用的对照模式。

## 5.2 SIFT10M 必须补 sweep

当前 SIFT10M 只有单点，必须至少补最小 sweep。

建议优先跑：

```text
固定：
InternalResultNum = 64
SearchPostingPageLimit = 4

扫：
SearchThreadNum = 4, 8, 12, 16
NumberOfThreads = 16, 40
```

然后再补 IR sweep：

```text
固定：
SearchThreadNum = 8
NumberOfThreads = 16 或 40
SearchPostingPageLimit = 4

扫：
InternalResultNum = 32, 64, 96, 128
```

如果资源更充足，再扩展为：

```text
SearchThreadNum = 1, 2, 4, 8, 12, 16
NumberOfThreads = 8, 16, 24, 40, 64
InternalResultNum = 32, 64, 96, 128
SearchPostingPageLimit = 4, 8
```

---

## 6. M0 / M1 / M2 / M3 的调整后优先级

总顺序不变：

```text
M0 -> M1 -> M2 -> M3
```

但 M0 内部需要拆成更符合当前代码现实的子阶段：

```text
M0a：统一当前 BATCH_READ 路径下的指标语义
M0b：补 SIFT10M sweep（仍基于默认 BATCH_READ）
M0c：确认非 BATCH_READ / 同步模式的代码缺口，决定是否先修测试路径
M0d：在现有证据下确认 M1 / M2 / M3 的收益目标
```

也就是说，原先设想的“直接切到非 BATCH_READ 拆纯 I/O wait”在当前仓库状态下并不能立刻执行；它本身已经变成一个前置工程问题。

在当前证据下，更合理的优先级理解是：

```text
M0 先修口径并补数据；
M1 可以先做轻量限流和观测；
M2 / M3 仍然是结构性主线。
```

---

## 7. 当前已经可以确认的结论

基于现有 SIFT10M 报告，目前可以较稳妥地写出以下结论：

```text
1. 当前主要耗时集中在 Ex / Batch Read + posting processing 路径。
2. 每 query 平均读取约 516 KB，扫描约 2956 个 posting 元素，只返回 Top-10。
3. final_result_ratio 约 0.0036，说明扫描放大明显。
4. duplicate ratio 约 12%，重复读取存在，但不是唯一主因。
5. CPU idle 很高，不能证明 CPU 总体算力不足。
6. BATCH_READ 口径下无法严格拆出纯 I/O wait。
7. 当前 SIFT10M 单点不能证明 st=8 或 nt=16 是最优。
```

这些结论已经足以支持：

```text
M2 / M3 方向有必要继续推进；
但 M1 收益空间仍需更细粒度 I/O 拆分后再判断。
```

---

## 8. 修改后的执行建议

### 第一步：修正当前报告口径

需要统一替换以下表述：

```text
“混合瓶颈，I/O 不是唯一主导”
  -> “瓶颈集中在 Ex / Batch Read + posting processing 路径；
      纯 I/O 与 CPU 占比尚不能严格拆分。”

“I/O 带宽已接近 NVMe 上限”
  -> “当前 workload 达到约 1.6 GB/s 读带宽平台；
      是否接近硬件上限需 fio 验证。”

“CPU 调度 / 锁竞争”
  -> “待验证假设。”

“distance eval ratio = 有效距离计算”
  -> “去重后进入距离计算的比例，不代表最终有效性。”

“st=8 最优、nt=16 足够”
  -> “当前单点配置表现良好，但需 sweep 验证。”
```

### 第二步：先承认非 BATCH_READ / 同步路径当前不可直接跑通

根据 `TEST_COMMANDS.md` 9.7 节，现阶段不能把“补非 BATCH_READ 构建”当作一个立刻可执行的无成本动作：

```text
1. 非 BATCH_READ 在当前官方 Linux 代码下不能直接编译通过；
2. 同步模式在当前官方代码下也不能直接编译通过；
3. 因此三模式细粒度对比不是现阶段现成可跑的实验矩阵。
```

这意味着当前更现实的选择只有两个：

```text
A. 先继续基于默认 BATCH_READ 路径补 sweep 和做端到端分析；
B. 如果确实要拆 pure I/O wait，再单独立项修复非 BATCH_READ / 同步分支。
```

如果未来修好非 BATCH_READ 路径，再用它回答：

- `io_wait_ms` 是否真的高；
- queue depth / PSI 升高时是否对应纯 I/O wait 上升；
- `posting_parse_ms` / `distance_calc_ms` 是否才是主成本。

### 第三步：补 SIFT10M sweep

至少补：

```text
st = 4, 8, 12, 16
nt = 16, 40
ir = 32, 64, 96, 128
```

目标是回答：

```text
QPS 平台点在哪里；
P99 拐点在哪里；
IR 与 requested bytes 的关系；
Recall 与 requested bytes 的关系。
```

### 第四步：再决定 M1 与 M2 的相对优先级

如果新增数据表明：

```text
高并发下 queue depth / PSI / io_wait 显著上升
```

则优先推进 M1，例如：

```text
bytes-in-flight limit
pages-in-flight limit
large posting admission control
request coalescing
page-level dedup
```

如果新增数据表明：

```text
requested bytes 和 scanned elements 才是主因
```

则 M2 / M3 应更早推进，例如：

```text
two-stage posting
chunk directory
compact code coarse scoring
payload page-aware rerank
```

---

## 9. 最终判断

**原方案还能执行，但不能按原样执行。**

需要从：

```text
用当前 BATCH_READ 报告直接判断 I/O wait、CPU 调度、锁竞争
```

改成：

```text
先把 BATCH_READ 作为当前唯一稳定可测的端到端路径；
先在这一路径上补 sweep、修正指标口径、确认结构性瓶颈；
如果后续确实需要细粒度 pure I/O wait，再先修复非 BATCH_READ / 同步分支；
同时持续用 requested bytes、scanned elements、final_result_ratio
作为结构性瓶颈主指标。
```

一句话概括：

> 原方案的观测框架仍然成立，但指标语义、实验矩阵和可执行前提必须一起修改；当前结果已经证明 posting 读放大和扫描放大明显，而 9.7 节进一步说明：非 BATCH_READ / 同步模式在当前官方代码下并不能直接拿来补测。
