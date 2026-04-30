# SPANN NVMe 快速压测瓶颈分析报告

## 1. 背景

本报告基于新机器上执行 `TEST_COMMANDS.md` 中“快速压测（减少组合数）”得到的结果，对 SPANN 在 **SIFT1M + NVMe SSD + BATCH_READ** 条件下的性能瓶颈进行分析。

测试结果目录：`results/io_analysis/sweep_quick/`

有效组合：

- `st2_nt4`
- `st2_nt8`
- `st2_nt16`
- `st4_nt4`
- `st4_nt8`
- `st4_nt16`
- `st8_nt8`
- `st8_nt16`
- `st16_nt16`

其中：

- `st` = `SearchThreadNum`
- `nt` = `NumberOfThreads`

---

## 2. 核心结论

### 2.1 总结

本次 NVMe 测试说明：

1. **NVMe 显著提高了 SPANN 的吞吐上限**，相比此前 SATA 结果，QPS 明显提升；
2. 但 **SPANN 当前的 batch-read/query 调度方式，没有把 NVMe 的并发能力充分转成有效吞吐**；
3. 当前瓶颈已经不是“纯磁盘瓶颈”，而是 **I/O + CPU/调度 的混合瓶颈**；
4. 最优点出现在 **中等并发**，即 `SearchThreadNum=4`，而不是更高并发；
5. 随着并发继续增大，**尾延迟和 CPU 竞争迅速恶化，但 QPS 不再提升，甚至下降**。

### 2.2 推荐基线

当前 NVMe 机器上的推荐搜索基线为：

- **首选：`st4_nt4`**
- **备选：`st4_nt8`**

原因：

- 这两组 QPS 最高；
- 平均延迟和 P99 延迟显著优于 `st8` / `st16`；
- 继续增加线程并没有带来额外吞吐收益。

---

## 3. 结果总览

### 3.1 关键结果表

| 配置 | QPS | Avg Latency | P95 | P99 |
|------|----:|------------:|----:|----:|
| st2_nt4 | 162.85 | 12.28 ms | 13.79 ms | 14.64 ms |
| st2_nt8 | 167.66 | 11.93 ms | 13.51 ms | 14.51 ms |
| st2_nt16 | 167.76 | 11.92 ms | 13.66 ms | 14.78 ms |
| st4_nt4 | **182.70** | 21.89 ms | 24.06 ms | 25.37 ms |
| st4_nt8 | **182.58** | 21.91 ms | 24.08 ms | 25.34 ms |
| st4_nt16 | 166.59 | 24.01 ms | 31.41 ms | 34.77 ms |
| st8_nt8 | 155.11 | 51.57 ms | 64.16 ms | 73.09 ms |
| st8_nt16 | 166.88 | 47.94 ms | 57.81 ms | 63.71 ms |
| st16_nt16 | 161.12 | 99.14 ms | 237.07 ms | 250.78 ms |

### 3.2 直接观察

1. `st2 -> st4` 吞吐有明显提升；
2. `st4 -> st8 -> st16` 没有继续带来 QPS 增长；
3. `st16_nt16` 出现严重尾延迟膨胀，说明系统已经进入过度并发区；
4. `st4_nt16` 比 `st4_nt4/st4_nt8` 更差，说明 `NumberOfThreads` 不是越大越好。

---

## 4. 与 SATA 结果的关系

这一节的关键问题不是“NVMe 有没有用”，而是：

> 既然当前结论是 **混合瓶颈**，为什么从 SATA 换到 NVMe 后，吞吐仍然明显提升？

答案是：

## **I/O 不再是 NVMe 场景下唯一的主导瓶颈，但它仍然是决定吞吐基线的重要限制项。**

也就是说，SATA 时代更像“磁盘先卡住系统”，而 NVMe 时代则是“磁盘上限被抬高后，CPU、Head Search、callback 内计算和 query 调度开始更明显地暴露出来”。

### 4.1 NVMe 相比 SATA 的直接收益

对同一组 quick sweep 配置，NVMe 的 QPS 基本都达到了 SATA 的约 2 倍：

| 配置 | NVMe QPS | SATA QPS | 提升 |
|------|---------:|---------:|-----:|
| st2_nt4 | 162.85 | 78.56 | 107.29% |
| st2_nt8 | 167.66 | 79.87 | 109.91% |
| st2_nt16 | 167.76 | 80.95 | 107.23% |
| st4_nt4 | **182.70** | 81.19 | **125.03%** |
| st4_nt8 | **182.58** | 81.29 | **124.60%** |
| st4_nt16 | 166.59 | 82.05 | 103.04% |
| st8_nt8 | 155.11 | 81.11 | 91.24% |
| st8_nt16 | 166.88 | 81.36 | 105.12% |
| st16_nt16 | 161.12 | 76.37 | 110.96% |

对应地，系统级读带宽也显著提高：

- SATA quick sweep 大致稳定在 `58~61 MB/s`；
- NVMe quick sweep 提升到约 `118~144 MB/s`；
- 最优组合 `st4_nt4 / st4_nt8` 达到约 `144 MB/s`。

这说明：

## **换到 NVMe 后，同样的查询读放大模式，确实能以更高带宽、更短完成时间执行，所以吞吐先明显上升。**

### 4.2 为什么这不和“混合瓶颈”矛盾

系统总延迟并不是单一由磁盘决定，而更接近：

```text
Total Latency ≈ Head Search + Disk Search + decode/parse/compute + 调度/竞争
```

因此，只要磁盘阶段在 SATA 上占比足够大，那么即使它不是唯一部分，只要把这一段显著压短，总吞吐就会明显提升。

从对比数据看，这一点非常明确。以几个代表性配置为例：

| 配置 | NVMe Avg Total | SATA Avg Total | NVMe Avg Head | SATA Avg Head | NVMe Avg Ex | SATA Avg Ex | NVMe Batch | SATA Batch |
|------|---------------:|---------------:|--------------:|--------------:|------------:|------------:|-----------:|-----------:|
| st2_nt4 | 12.28 ms | 25.46 ms | 8.99 ms | 16.37 ms | 3.29 ms | 9.09 ms | 1.57 ms | 5.17 ms |
| st4_nt4 | 21.89 ms | 49.26 ms | 16.15 ms | 35.25 ms | 5.74 ms | 14.01 ms | 1.95 ms | 5.43 ms |
| st8_nt8 | 51.57 ms | 98.62 ms | 39.52 ms | 74.26 ms | 12.05 ms | 24.36 ms | 2.52 ms | 6.23 ms |
| st16_nt16 | 99.14 ms | 209.41 ms | 76.93 ms | 163.94 ms | 22.21 ms | 45.47 ms | 3.11 ms | 7.88 ms |

可以看到：

1. **`ex latency` 和 `batch_read_total_ms` 在 NVMe 上都明显下降**，说明磁盘相关阶段确实被压短；
2. **`head latency` 也同步下降**，这不是因为 Head Search 算法变了，而是因为 SATA 上更慢的 disk phase 会放大全系统拥塞、线程等待与调度开销；
3. 所以换 NVMe 后，不只是纯 I/O 等待更短，整个 query 生命周期都变短了。

### 4.3 为什么说“访问模式没变，只是介质变快了”

虽然 NVMe 吞吐翻倍，但查询级读放大指标几乎完全没变：

- `avg_requested_read_bytes ≈ 792029.594 bytes/query`（约 773.5 KB/query）
- `avg_pages_read ≈ 193.37 pages/query`
- `avg_duplicate_vector_read_ratio ≈ 0.118684`
- `avg_distance_eval_ratio ≈ 0.881316`
- `avg_final_result_ratio ≈ 0.008018`
- `avg_recall ≈ 0.94175`

这说明：

## **NVMe 解决的是介质速度问题，不是 SPANN 在 SIFT1M 上的访问模式问题。**

即：

- 每个 query 仍然要读取大量 posting/page；
- 每个 query 的有效最终结果占比仍然极低；
- 算法层的读放大没有因为换 NVMe 而改变；
- NVMe 的收益主要来自“同样多的数据，被更快地读完”。

### 4.4 为什么到了 NVMe 上，I/O 不再是唯一主瓶颈

如果 NVMe 场景仍然是“纯 I/O 瓶颈”，那么继续提高并发时应该更容易观察到：

- 更高并发 → 更高平均队列深度；
- 更高带宽 → 更高 QPS。

但实际并不是这样：

- `st4_nt4 / st4_nt8` 达到最高 QPS（约 `182.6~182.7`）；
- 再往上到 `st8` / `st16`，QPS 不再增长，甚至下降；
- `st16_nt16` 的 `cpu_idle_percent` 只有 `6.296`，但读带宽只有 `117.767 MB/s`，反而低于 `st4_nt4` 的 `144.335 MB/s`。

这说明：

## **NVMe 已经把设备侧上限抬高了，但并发继续升高时，系统先进入了 CPU / 调度 / Head Search / callback 内处理的竞争区，而不是继续把更多 NVMe 并发能力转成更高吞吐。**

因此，NVMe vs SATA 的对比可以归纳为：

1. **SATA 上，I/O 是更硬的第一层上限**；
2. **NVMe 上，这层上限被明显放松，所以吞吐先翻倍**；
3. **但由于 query 调度与 BATCH_READ completion 路径没有和 CPU 处理充分解耦，系统很快进入混合瓶颈区**；
4. **所以 NVMe 的收益是真实的，但还没有被“充分吃满”。**

---

## 5. 为什么说“当前 batch-read/query 调度方式没有把 NVMe 并发能力充分转成有效吞吐”

这不是泛泛而谈，而是由代码路径和结果共同支持的结论。

### 5.1 搜索线程是端到端处理 query，而不是流水化调度

搜索入口位于：

- `AnnService/inc/SSDServing/SSDIndex.h:124-189`

当前逻辑中，每个搜索线程会：

1. 取一个 query；
2. 做 Head Index 搜索；
3. 调用 `SearchDiskIndex(...)` 做磁盘搜索；
4. 整个 query 完成后，再取下一个 query。

这意味着：

## **query 并发是按搜索线程数直接控制的，而不是通过全局 I/O 调度器持续维持深队列。**

换句话说，当前执行模型更像：

```text
query thread:
  head search
  prepare batch reads
  submit batch
  wait + process completions
  finish query
  next query
```

而不是：

```text
global scheduler:
  cross-query aggregate requests
  keep bytes/pages in flight high
  separate I/O completion from decode/parse/compute
```

---

### 5.2 BATCH_READ callback 内直接做 decode / parse / distance compute

关键代码位于：

- `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:388-402`
- `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:326-350`

在 `BATCH_READ` 分支下，request 的 callback 不是只标记“读取完成”，而是直接执行：

- posting 解压/解码；
- posting 解析；
- dedup；
- `ComputeDistance(...)`；
- `AddPoint(...)`。

这意味着：

## **batch-read 的 completion 路径混入了大量 CPU 工作。**

因此，当前 `batch_read_total_ms` 不是“纯 I/O 等待时间”，而是：

- I/O submit / completion；
- callback 调度；
- posting decode；
- posting parse；
- distance compute；

的混合值。

这与分析方案文档的说明一致：

> BATCH_READ 路径下 `batch_read_total_latency_ms` 混合了 I/O wait、callback、decode、parse 和 distance compute，不能直接等同于纯 `io_wait_ms`。

---

### 5.3 Linux `BatchReadFileAsync(...)` 中 completion 后直接同步执行 callback

关键代码位于：

- `AnnService/src/Helper/AsyncFileReader.cpp:67-157`

Linux 下 `BatchReadFileAsync(...)` 的执行模式是：

1. 构造 `iocb`；
2. `io_submit` 提交；
3. `io_getevents` 等待完成；
4. 对每个完成事件，直接调用 `req->m_callback(true)`。

这意味着：

## **callback 不是分发给独立计算线程池，而是在当前批处理线程里同步执行。**

因此：

- query 线程/批处理线程在处理 callback 时，会被 decode/parse/compute 占住；
- 它不能同时继续高效地为后续 query 维持持续 I/O 投喂；
- I/O 与 CPU 计算没有被充分解耦。

---

### 5.4 每个 query 更像“打一波 I/O，再把这一波吃完”

在 `ExtraStaticSearcher::SearchIndex(...)` 中：

- 先为本 query 的 posting 构造全部请求；
- 调用 `BatchReadFileAsync(...)`；
- 再在本轮内把 completion 都处理掉。

见：

- `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:360-445`

这更像：

## **每个 query 自己打一波 batch I/O，然后自己把结果处理完，再结束。**

而不是多个 query 的 I/O 请求在更大范围内统一混排与调度。

对 NVMe 而言，这种“按 query 分段式发起与回收”的方式，会导致：

- 峰值队列深度偶尔很高；
- 但平均队列深度不高；
- 设备缺乏稳定的深队列输入流；
- 应用层先在 CPU/回调/调度层面产生内耗。

---

## 6. 结果文件如何支持这一判断

### 6.1 NVMe 没有被持续打到很深的平均队列深度

以两个代表性组合为例：

#### `st4_nt4`

- `read_bandwidth_mbs: 144.335`
- `instant_queue_depth_avg: 1.551`
- `avg_queue_depth_from_weighted_io_time: 1.815`
- `peak_queue_depth: 49`

#### `st16_nt16`

- `read_bandwidth_mbs: 117.767`
- `instant_queue_depth_avg: 1.938`
- `avg_queue_depth_from_weighted_io_time: 2.218`
- `peak_queue_depth: 37`

解读：

1. 平均队列深度只有 `~1.8 ~ 2.2`，对 NVMe 来说并不高；
2. 峰值可以很高，但平均不高，说明系统更像“偶发突发提交”，而不是“持续深队列”；
3. 如果真正充分利用 NVMe 并发能力，通常会看到更稳定、更高的平均队列深度与持续更高的吞吐。

因此：

## **当前应用层没有持续把足够深的有效 I/O 队列喂给 NVMe。**

---

### 6.2 更高并发没有带来更高带宽，反而先打满了 CPU

对比：

#### `st4_nt4`

- QPS: `182.70`
- `cpu_idle_percent: 63.605`
- `cpu_iowait_percent: 14.891`
- `read_bandwidth_mbs: 144.335`

#### `st16_nt16`

- QPS: `161.12`
- `cpu_idle_percent: 6.296`
- `cpu_iowait_percent: 6.126`
- `read_bandwidth_mbs: 117.767`

这说明：

1. 到 `st16_nt16` 时，CPU 基本已经很忙；
2. 但磁盘带宽没有提升，反而下降；
3. iowait 下降，不代表问题消失，而是说明系统不再主要表现为“CPU 等盘”，而是“CPU 自己已经忙不过来”；
4. 此时 query 调度、head search、callback、decode/parse/compute 等开销开始主导。

因此：

## **更高 query 并发并没有转化为更多有效 I/O 吞吐，而是先转化成了更多 CPU/调度竞争。**

---

### 6.3 latency 结构说明增长主要不来自纯 I/O 等待

基于 query CSV 汇总：

#### `st4_nt4`

- head latency ≈ `16.15 ms`
- ex latency ≈ `5.74 ms`
- batch_read_total_ms ≈ `1.95 ms`

#### `st16_nt16`

- head latency ≈ `76.93 ms`
- ex latency ≈ `22.21 ms`
- batch_read_total_ms ≈ `3.11 ms`

解读：

1. 高并发下增长最快的是 `head latency` 和整体 query latency；
2. `batch_read_total_ms` 虽然上升，但远没有上升到足以单独解释整体退化；
3. 这说明问题不是“NVMe 被纯 I/O 等待卡死”，而是**query 并发、Head Search、callback 内 CPU 处理、线程竞争共同导致退化**。

---

## 7. 为什么 `st4_nt4 / st4_nt8` 是当前 NVMe 最优点

### 7.1 `st2 -> st4` 仍然能把设备和系统利用得更充分

- `st2_nt8`: 167.66 QPS
- `st4_nt8`: 182.58 QPS

这说明 `SearchThreadNum=2` 还没有完全发挥系统吞吐。

### 7.2 `st4` 达到一个平衡点

在 `st4_nt4/st4_nt8`：

- QPS 达到全组最高；
- 延迟仍处于可接受范围；
- CPU 还有大量空闲；
- 磁盘读带宽已经达到这轮测试中的最高水平。

说明这里是一个比较好的平衡点：

## **query 并发足够多，能把 NVMe 用起来；但又没有把 CPU/调度层推入严重竞争区。**

### 7.3 `st8/st16` 开始进入“内耗大于收益”区间

当 `SearchThreadNum` 提高到 `8` 或 `16`：

- 吞吐不再提升；
- 平均延迟和 P99 急剧上升；
- CPU idle 显著下降；
- 读带宽没有继续上升。

因此：

## **更多并发已经不能转成更多有效 I/O，而是在放大系统内部竞争。**

---

## 8. 结论归纳

当前 NVMe quick sweep 可以得出以下更精确的判断：

### 8.1 硬件层

- NVMe 相比 SATA 明显更强；
- 它已经把之前部分“设备速度瓶颈”抬高；
- 但当前测试中 NVMe 仍未被持续以深队列方式高效利用。

### 8.2 算法/访问模式层

- 每 query 仍然存在明显读放大；
- `requested_read_bytes/query` 基本不变；
- `final_result_ratio` 极低，说明读取大量数据只产出极少最终结果；
- 这仍然是 SPANN 在 SIFT1M 上的结构性问题之一。

### 8.3 调度层

- 当前 BATCH_READ 路径没有把 I/O completion 与 decode/parse/compute 解耦；
- query 线程采用“端到端串行完成一个 query 再拿下一个”的模式；
- 全局没有形成持续、稳定、跨 query 的深 I/O 队列；
- 并发升高后，先出现的是 CPU/调度竞争，而不是 NVMe 吞吐继续上升。

因此，本次分析的中心结论可以概括为：

## **SPANN 当前 batch-read/query 调度方式，在 NVMe 上只能部分获益于更快介质，但无法把 NVMe 的高并发 I/O 能力持续转化为更高的有效 query 吞吐。**

---

## 9. 建议

### 9.1 作为当前测试基线

建议后续测试优先使用：

- `st4_nt4`
- `st4_nt8`

### 9.2 后续参数优化方向

在当前 NVMe 基线下，优先继续扫描以下参数组合：

- `InternalResultNum`
- `SearchPostingPageLimit`
- `MaxCheck`

重点观察是否可以降低：

- `requested_read_bytes`
- `pages_read`
- `posting_elements_raw`
- `final_result_ratio` 过低的问题

### 9.3 架构/实现层优化方向

如果目标是进一步发挥 NVMe 能力，代码层更值得尝试的方向包括：

1. **将 BATCH_READ callback 改为只标记完成，不在 callback 内做 decode/parse/compute**；
2. **把 completion 后处理转移到独立 query-consumer / worker 队列**；
3. **跨 query 统一调度 in-flight bytes/pages，而不是让每个 query 单独打一波 I/O**；
4. **引入更明确的 bytes-in-flight / pages-in-flight 限流与调度策略**；
5. **进一步降低单 query 读取放大，减少“读很多、真正有用很少”的现象**。

---

## 10. 附：本次分析引用的关键代码位置

- 搜索线程主循环：`AnnService/inc/SSDServing/SSDIndex.h:124-189`
- 搜索参数入口：`AnnService/inc/SSDServing/SSDIndex.h:195-267`
- Disk 搜索入口：`AnnService/src/Core/SPANN/SPANNIndex.cpp:519-589`
- Static Searcher 请求构造与统计：`AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:260-528`
- BATCH_READ callback 内 decode/parse/compute：`AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:388-402`
- Posting parse / dedup / ComputeDistance：`AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:326-350`
- Linux BatchReadFileAsync 实现：`AnnService/src/Helper/AsyncFileReader.cpp:67-157`
- Detailed I/O CSV 输出：`AnnService/inc/SSDServing/SSDIndex.h:405-529`

---

## 11. 最终一句话结论

## **NVMe 已经把 SPANN 的设备侧上限抬高了，但当前 batch-read/query 调度模型仍然更像“每个 query 自己打一批 I/O、自己同步吃完 callback”，而不是持续维持深而稳定的跨 query I/O 流；因此更多 NVMe 并发能力没有被有效转化成更高 QPS，而先转化成了 CPU/调度层的内耗。**
