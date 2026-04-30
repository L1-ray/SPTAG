# SIFT10M SPANN 搜索 I/O 性能分析报告

## 测试配置

### 数据集
- **名称**: SIFT10M
- **向量数**: 10,000,000
- **维度**: 128
- **数据类型**: UInt8 + DEFAULT
- **距离度量**: L2

### 搜索参数
- **SearchThreadNum**: 8
- **NumberOfThreads**: 16
- **InternalResultNum**: 64
- **SearchPostingPageLimit**: 4
- **ResultNum**: 10

### 索引信息
- **Head vectors**: 1,496,408 (14.96%)
- **Total pages**: 2,061,442
- **Index size**: 8.4 GB
- **Total elements**: 63,692,544

---

## 性能指标

### 吞吐量与召回

| 指标 | 值 |
|------|-----|
| **QPS** | 5,608.52 |
| **Recall@10** | 0.9491 |
| **MRR@10** | 1.0000 |

### 延迟分布

| 指标 | 值 |
|------|-----|
| **avg latency** | 1.425 ms |
| **p50 latency** | 1.410 ms |
| **p90 latency** | 1.590 ms |
| **p95 latency** | 1.655 ms |
| **p99 latency** | 1.821 ms |
| **p99.9 latency** | 5.060 ms |
| **max latency** | 13.461 ms |

---

## I/O 性能分析

### 系统级 I/O 指标

| 指标 | 值 |
|------|-----|
| **read bandwidth** | 1,608.6 MB/s |
| **process read bytes (total)** | 6.79 GB |
| **avg queue depth** | 102.9 |
| **peak queue depth** | 278 |
| **CPU iowait** | 0.41% |
| **CPU idle** | 83.0% |
| **duration** | 4.025 s |

### 查询级 I/O 指标

| 指标 | 值 |
|------|-----|
| **avg requested read bytes** | 515,772 bytes (~504 KB) |
| **avg pages read** | 125.9 |
| **postings touched** | 64 |
| **raw posting elements scanned** | 2,955.6 |

### 效率指标

| 指标 | 值 | 说明 |
|------|-----|------|
| **dup ratio** | 0.1195 | 12% 重复向量读取 |
| **distance eval ratio** | 0.8805 | 去重后仍需执行距离计算的比例，不代表最终有效候选比例 |
| **final result ratio** | 0.0036 | 最终结果仅占扫描元素约 0.36%，存在明显扫描放大 |

---

## 延迟分解

| 阶段 | 平均延迟 | 占比 |
|------|---------|------|
| Head Latency | 0.286 ms | 20% |
| Ex Latency (I/O + 计算) | 1.136 ms | 80% |
| **Total** | 1.422 ms | 100% |

### Ex Latency 细分

| 阶段 | 平均延迟 | 备注 |
|------|---------|------|
| Batch Read Total | 1.119 ms | 主体路径；包含 I/O wait、callback、posting parse、distance calc 等混合开销 |
| Posting Parse | 0.094 ms | Batch Read Total 内部子项，不应与其并列相加 |
| Distance Calc | 0.089 ms | Batch Read Total 内部子项，不应与其并列相加 |

---

## 瓶颈分析

### 结论
当前 SIFT10M 结果显示，SPANN 搜索阶段的主要成本集中在 **Ex / Batch Read + posting 处理路径**；单查询约读取 516 KB、扫描约 2956 个 posting 元素，只返回 Top-10，说明仍然存在明显的 posting 读放大和扫描放大。

由于当前是 `BATCH_READ` 路径，`Batch Read Total` 混合了 I/O 等待、callback、posting parse 和 distance compute，因此还不能严格拆分“纯 I/O 等待”和“CPU 计算”各自占比。

### 相关性分析

| 相关性 | 值 | 说明 |
|--------|-----|------|
| corr(latency, requested_bytes) | 0.143 | 弱正相关；当前不足以单独证明 latency 主要由 requested bytes 决定 |
| corr(latency, queue_depth) | -0.227 | 弱负相关；当前不足以单独证明队列深度是主导变量 |

### 当前已确认的主要信号

1. **Ex / Batch 路径占主导**：Ex Latency 约占总延迟 80%，而 Batch Read Total 几乎覆盖 Ex Latency 主体。
2. **posting 读放大明显**：单 query 平均读取约 516 KB、125.9 pages、64 个 postings。
3. **扫描放大明显**：单 query 平均扫描约 2955.6 个 posting 元素，但最终只返回 Top-10，`final result ratio ≈ 0.0036`。
4. **重复读取存在但不是唯一问题**：`dup ratio ≈ 12%`，说明有一部分向量被重复读取，但更大的问题仍是总体 posting 读取与扫描规模。

### 当前仍待验证、不能下结论的部分

- **“I/O 带宽已接近 NVMe 硬件上限”**：当前只能说明该 workload 下读带宽稳定在约 1.6 GB/s，是否接近设备上限需要 `fio` 或设备基准确认。
- **“CPU 调度是瓶颈”**：当前 `CPU idle ≈ 83%`，不能证明 CPU 总体已饱和；若怀疑调度问题，需要 per-thread CPU / context switch / off-CPU profile。
- **“锁竞争是瓶颈”**：当前报告没有直接测量 mutex/futex/queue wait，只能作为后续 profiling 假设，不能视为已确认结论。

### 口径差异说明

- query-level `avg requested read bytes ≈ 515,772 bytes/query`，按 10,000 queries 估算约为 **5.16 GB**；
- system/process-level `process read bytes delta ≈ 6.79 GB`。

两者存在差异，说明系统级采样窗口可能包含 query posting 之外的额外读取，或 query-level 统计仅覆盖 posting read。后续若要进一步收敛结论，应以 SearchSSDIndex 阶段时间窗做更严格对齐。

---

## 与 SIFT1M 对比

| 指标 | SIFT1M (st=8) | SIFT10M (st=8) | 比例 |
|------|---------------|----------------|------|
| 向量数 | 1M | 10M | 10x |
| Head 数 | 149,943 | 1,496,408 | 10x |
| **QPS** | 5,945 | 5,609 | 0.94x |
| **Recall@10** | 0.978 | 0.949 | 0.97x |
| **avg latency** | 1.34 ms | 1.43 ms | 1.07x |
| **p95 latency** | 1.60 ms | 1.66 ms | 1.04x |
| **read BW** | 1,613 MB/s | 1,609 MB/s | ~1x |
| **avg read bytes/query** | 486 KB | 516 KB | 1.06x |
| **dup ratio** | 0.163 | 0.120 | 0.74x |
| **queue depth** | ~100 | ~103 | ~1x |
| **Index size** | 0.79 GB | 8.4 GB | 10.6x |

### 对比结论

1. **当前 workload 下具有较好的规模扩展性**
   - 在 SIFT1M → SIFT10M、head 数也约同比扩展的条件下，QPS 仅下降约 6%，平均延迟仅增加约 7%，单 query 读取量仅增加约 6%。
   - 这说明当前 SIFT 同分布、相同查询参数下，单查询 posting 成本保持相对稳定。

2. **但 Recall 同时下降，需要纳入权衡**
   - SIFT1M 的 `Recall@10 ≈ 0.978`
   - SIFT10M 的 `Recall@10 ≈ 0.949`
   - 因此不能只看吞吐与延迟，也要考虑数据规模增大后质量有所下降。

3. **两者的报告结论不应简单视为相同**
   - SIFT1M 此前更接近“读取放大显著，且与延迟相关”；
   - SIFT10M 当前则更适合表述为“主要成本集中在 Ex / Batch Read + posting 处理路径，但纯 I/O 与 CPU 占比尚不能严格拆分”。

4. **读带宽接近并不等于证明硬件上限**
   - 两组 workload 都稳定在约 1.6 GB/s，说明这台机器上的该访问模式形成了相似的 workload-level 带宽平台；
   - 但是否接近 NVMe 设备硬件上限，需要独立基准确认。

---

## 关键发现

### 1. 当前主要成本集中在 Ex / Batch Read + posting 处理路径
- Ex Latency 约占总延迟 80%
- Batch Read Total 几乎覆盖 Ex Latency 主体
- 但由于其口径混合了 I/O wait、callback、posting parse 和 distance calc，当前还不能严格拆分纯 I/O 与纯 CPU 开销

### 2. posting 读放大与扫描放大仍然明显
- 单 query 平均读取约 516 KB
- 单 query 平均访问约 125.9 pages、64 个 postings
- 单 query 平均扫描约 2955.6 个 posting 元素，但最终只返回 Top-10
- `final result ratio ≈ 0.0036`，说明扫描放大明显

### 3. 与 SIFT1M 相比，单查询成本增长较小
- SIFT10M 每查询读取 516 KB，比 SIFT1M (486 KB) 仅多约 6%
- 平均延迟也只比 SIFT1M 高约 7%
- 说明在当前 SIFT workload 下，posting 组织的单查询成本随规模扩展较平稳

### 4. duplicate ratio 更低，但不是核心结论
- SIFT10M 的 `dup ratio ≈ 12%`，低于 SIFT1M 的约 16%
- 这说明重复读取有所缓解，但总体 posting 读取量与扫描量仍然是更主要的问题

### 5. 当前数据不足以支持更强的系统性结论
- 不能仅凭当前报告断言“已接近 NVMe 硬件上限”
- 不能断言“CPU 调度/锁竞争已经成为主瓶颈”
- 不能断言“st=8 已是 SIFT10M 最优点”

---

## 优化建议

### 当前最优先的方向
1. **降低 requested bytes/query**
   - 目标是减少每个 query 的 posting 读取总量，而不是单纯提高线程数。

2. **降低 scanned elements/query**
   - 当前每 query 平均扫描约 2956 个 posting 元素，但只返回 Top-10，扫描放大明显。

3. **拆分 BATCH_READ 计时口径**
   - 应进一步区分：I/O wait、callback、posting parse、distance calc，避免把混合时间直接解释为纯 I/O 或纯 CPU。

4. **补充 SIFT10M sweep 验证**
   - `SearchThreadNum`: 建议测试 `4 / 8 / 12 / 16`
   - `NumberOfThreads`: 建议测试 `16 / 24 / 40 / 64`
   - `InternalResultNum`: 建议测试 `32 / 64 / 96`

### Recall 提升方向
- 若需更高 Recall，可增加 `InternalResultNum` 到 96 或 128；
- 但这通常会带来更高 requested bytes/query、更大扫描量和更低 QPS，需要结合质量-性能 tradeoff 一起评估。

### 次级优化方向
- 对 hot postings 做 cache / prefetch；
- 尝试减少重复 VID 的读取与 page-level 重复访问；
- 研究更细粒度的 posting 组织方式，以减少一次 query 需要扫描的无效元素数。

---

## 测试环境

- **操作系统**: Ubuntu 24.04
- **内核版本**: 6.17.0-20-generic
- **存储设备**: NVMe (nvme0n1p8)
- **测试时间**: 2026-04-30
- **缓存状态**: 冷缓存 (已清除 page cache)
