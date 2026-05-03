# SPANN Beyond Official Baseline Plan (2026-05-02)

## 1. Current Goal

目标是在 strict `UInt8 + DEFAULT` SIFT1M 口径下超过官方 legacy baseline，而不是继续证明 two-stage posting 可用。

官方代表性目标线：

| Metric | Official baseline |
|---|---:|
| Dataset | SIFT1M UInt8 |
| Recall@10 | `0.978319` |
| Representative QPS | `~5945` at `SearchThreadNum=8` |
| Avg latency | `~1.34 ms` |
| P95 latency | `~1.60 ms` |
| Requested bytes | `~486 KB/query` |

硬约束：

- Recall@10 必须保持 `>= 0.978319`，否则只能作为 low-recall 档位。
- result hash/final recall 必须稳定，性能不能来自语义漂移。
- 性能收益必须能被 pages/bytes/cache-hit/read-wait/lock-wait 等指标解释。
- 对默认高并发场景，`st8` 必须超过官方目标线；beyond-M1 的阶段性强成功线仍是 `st8 QPS >= 6500`。

## 2. Related Documents

- [SIFT1M_Official_Alignment_Summary.md](/home/ray/code/SPTAG/SIFT1M_Official_Alignment_Summary.md): official strict `UInt8 + DEFAULT` baseline、sweep 结果和当前目标线来源。
- [SPANN_M2_M3_Code_Plan_Review_20260501.md](/home/ray/code/SPTAG/SPANN_M2_M3_Code_Plan_Review_20260501.md): two-stage、P1-P4d、payload attribution、CoHit layout 的审查和结论。
- [SPANN_M2_M3_UInt8_Optimization_Memo.md](/home/ray/code/SPTAG/SPANN_M2_M3_UInt8_Optimization_Memo.md): UInt8 + DEFAULT 口径下的实验备忘录和复现实验入口。
- [SPANN_IO_Analysis_Plan_Fixed.md](/home/ray/code/SPTAG/SPANN_IO_Analysis_Plan_Fixed.md): historical instrumentation contract；保留统计口径，不再作为当前优化执行计划。
- [SPANN_Problem_old_20260430.md](/home/ray/code/SPTAG/SPANN_Problem_old_20260430.md): historical structural-problem analysis；其中 posting 读放大、边界复制和 payload 重复方向仍有参考价值，但当前主线以本文为准。

## 3. Executive Summary

当前结论：

- Full two-stage posting 已降级，不再作为超过官方 baseline 的主线。
- CoHit/reorder 对 page reduction 有效，但单独不足以超过官方 baseline。
- M1 page cache 路线已完成核心验证：`ShardedPageCache + single-page cache + no admission` 在 `st8/st16` 有稳定收益。
- M1 不适合低并发默认开启：`st1/st4` 下 cache 固定开销超过 I/O 节省。
- **M1 仅适用于小规模数据集（~1M vectors）：** SIFT10M 上 M1 完全失效（QPS -0.2%，hit rate 18%）。
- **M2-H selective hybrid 已证伪：** I/O wait 分布过于均匀 (SIFT1M top 10%=29.63%, SIFT10M top 10%=27.51% < 30%)，选择性优化收益有限。
- **M4 primary-secondary dedupe 已证伪：** query-level duplicate payload/record ratio 只有 15.3%（SIFT1M）和 10.91%（SIFT10M），远低于 30% 阈值。Storage-level 复制不等于 Query-level 复制。
- 关键洞察：全局复制模式（每个 VID 在 6.4 个 posting）不转化为查询级重复（每个查询访问的 posting 内 duplicate payload/record ratio 很低）。
- **SIFT10M 验证完成：** 所有 SIFT1M 结论迁移，较大改进空间已在两个数据集上被排除。
- 结构性问题应表述为：legacy posting/page 读取存在读放大，但 official baseline 的顺序读、成熟 pipeline 和 cross-query page reuse 很强；任何新方案必须证明它减少 physical pages/read-wait，并且新增 CPU/随机读成本低于节省。

当前推荐配置：

```ini
[SearchSSDIndex]
EnablePageCache=true
PageCacheMaxBytes=268435456
```

使用条件：

- `SearchThreadNum >= 8`: 推荐启用 `ShardedPageCache`。
- `SearchThreadNum < 8`: 推荐默认关闭 cache，除非业务明确吞吐优先且接受低并发回退。

## 3.1 Conclusion: Limited Improvement Space Under Current Constraints

**核心结论**：在当前 strict SIFT1M UInt8 + DEFAULT + Recall@10=0.978319 口径下，沿 legacy SPANN 结构继续做"较大幅度"性能提升的空间已经很有限。

**"较大改进"定义**：st8 相比当前 M1 6120 QPS 再提升 >=15%~20%，稳定到 7000+ QPS，且 recall/hash 不变。

**结论**：这个目标不现实。6500 仍可能作为工程优化强目标，但不是显然存在的结构红利。

### 为什么结构缺陷诊断正确但性能红利不大

`SPANN_Problem_old_20260430.md` 的核心判断是对的：
- Posting 是最小 I/O 单位，存在读放大
- 边界复制带来空间和重复 payload
- 高并发下缺少全局 I/O 调度
- Full payload 在 posting 内重复存储

但本文档的实验证明：这些问题在当前 workload 下被官方 baseline 的几个优势抵消了：

| 官方 Baseline 优势 | 说明 |
|-------------------|------|
| 顺序读 | 读较少数量的 posting 顺序页，而不是大量随机 primary payload 页 |
| Payload 小 | SIFT1M UInt8 只有 128B，还没重到覆盖随机读成本 |
| 页面局部性强 | Posting/page 粒度虽粗，但 cross-query reuse 很强，M1 cache 已吃掉部分高并发红利 |
| Recall 约束硬 | 任何 code-first/pruning/TopR 策略只要漏候选，就需要提高 TopR 恢复 recall，把 I/O saving 吃掉 |

**关键洞察**：旧文档指出的是"结构缺陷"，但实验证明这些缺陷不是当前 SIFT1M 官方口径下的巨大可套利空间。

### 各方向最终状态

| 方向 | 状态 | 证据 | 剩余空间 |
|------|------|------|----------|
| M1 Page Cache | **唯一正收益** | st8 +5%, st16 +13.6% | 产品化并发门控，5-10% 工程收益 |
| M2 / Two-stage | **封死** | compact scan/merge 成本 > I/O saving | 不继续 |
| P4 / CoHit reorder | **不够** | 15-19% page reduction，但 TopR tradeoff | 单独不足 |
| M2-H Selective | **证伪** | top 10% I/O wait = 29.63% < 30% | 无长尾，停止 |
| M4 Dedupe | **证伪** | Query-level duplicate 15.3%, primary pages 10x legacy | **关键新结论**，停止 |
| M3 Chunk Pruning | **低优先** | 需要 exact-safe bound，heuristic 有 recall 风险 | 待定 |

### M4 证伪的关键意义

M4-0 Oracle 是最关键的新结论：

- **Storage 视角**：每个 VID 平均出现在 6.4 个 posting，76% 空间节省潜力
- **Query 视角**：duplicate payload/record ratio 只有 15.3%
- **Primary layout**：最佳模拟约 1257 pages/query vs legacy 119 pages/query（约 10.6x）

这说明"全局去重"会把顺序 posting 读变成随机 primary 页读，查询性能会恶化而不是提升。

### 剩余改进空间评估

| 目标 | 可行性 | 说明 |
|------|--------|------|
| 6120 QPS (M1 st8) | **已达成** | 当前主线 |
| 6500 QPS | 可能 | 工程优化强目标 |
| 6631 QPS (M1 st16) | **已达成** | 高并发场景 |
| 7000+ QPS | 不现实 | 需要 15-20% 提升，已排除 |
| 30%+ QPS 提升 | 已排除 | 实验证明局部改造无效 |

### 下一步方向

**短期（工程层面，5-10% 收益）**：
1. M1 Productization：并发门控，消除 st1/st4 回退
2. Tail latency 优化：p99.9 已通过 sharded lock 解决
3. 稳定性验证：多次运行确认结果

**中长期（如果需要 7000+ QPS）**：
需要换更大的算法问题，不再是 legacy 结构的局部改造：
- Routing 改造（head graph 改造）
- Replica budget 重构
- Graph-assisted routing
- Learned router

**风险**：这些方向会显著增加构建复杂度、评估周期、recall 风险。不建议在当前 SIFT1M 口径下投入，除非业务需求明确。

## 3.2 SIFT10M 验证结论 (2026-05-03)

SIFT10M 验证已完成，所有 SIFT1M 结论完全迁移。详见 [SIFT10M_Beyond_Validation_Summary.md](/home/ray/code/SPTAG/results/io_analysis/sift10m_beyond_validation_20260503/SIFT10M_Beyond_Validation_Summary.md)。

### SIFT10M 验证结果

| 任务 | SIFT1M 结果 | SIFT10M 结果 | 一致？ |
|------|-------------|--------------|--------|
| **S0 st sweep** | st8 平台期 | st8 平台期 | ✓ |
| **M1 Page Cache** | +5.0% QPS (st8) | -0.2% QPS (st8) | ✗ |
| **M2-H** | Top10%=29.63% | Top10%=27.51% | ✓ |
| **M4** | 查询重复=15.3% | 查询重复=10.91% | ✓ |

### M1 在 SIFT10M 上失效的根因

| 指标 | SIFT1M | SIFT10M | 比值 |
|------|--------|---------|------|
| 总 postings | 150,076 | 1,496,408 | 10x |
| 单页 posting 数 | 48,158 | 440,342 | 9.1x |
| 单页 posting 占比 | 32.1% | 29.4% | 相近 |
| 被访问 posting 平均访问次数 | ~6.5 | 1.28 | 1/5 |
| Cache 命中率 | 77.71% | 18.25% | 1/4.3 |

**根因**: SIFT10M 索引规模大 10x，但 query 数量相同。每个被访问的 posting 平均只被访问 1.28 次（vs SIFT1M 的 6.5 次），导致跨查询复用 (cross-query reuse) 不足。

详见: `results/io_analysis/sift10m_beyond_validation_20260503/m1_cache/M1_Failure_Analysis.md`

### M1 适用范围限定

| 数据集规模 | M1 推荐 |
|------------|---------|
| 小规模 (~1M vectors) | ✓ 推荐 (+5% QPS) |
| 中规模 (~5M vectors) | ? 不确定 |
| 大规模 (~10M+ vectors) | ✗ 不推荐 (0% 收益) |

### 最终结论

**SIFT1M 和 SIFT10M 都无大幅超越空间：**

| 方向 | SIFT1M | SIFT10M | 产品化 |
|------|--------|---------|--------|
| M1: Page Cache | ✓ st8 +5% | ✗ st8 0% | 仅限小规模 |
| M2-H: Selective Hybrid | ✗ 已停止 | ✗ 已停止 | 否 |
| M4: Primary-Secondary | ✗ 已停止 | ✗ 已停止 | 否 |

**结论**: 当前结构下，无论数据集规模，legacy SPANN 的局部改造空间已穷尽。

---

## 4. Evidence So Far

已验证事实：

- 官方 baseline 的结构性风险真实存在：posting/page 粒度偏粗、full payload 较重、副本可能带来空间/读放大、高并发下存在 page reuse/cache 调度空间。但这些风险不等于任何结构改造都会更快；official legacy 路径的顺序读和成熟 pipeline 是很强的性能基线。
- All-in two-stage 失败原因明确：它降低了部分 payload/page 读取，但引入 compact scan、merge、read-plan、random payload fetch 等额外成本，matched recall 下整体 QPS 明显落后官方。
- P1/P2 attribution 证明 two-stage 主要瓶颈是 payload I/O wait，而不是 compact-code CPU pipeline。
- P3 oracle 显示理论 page reduction 上限约 `40%~50%`，说明 locality 优化有空间，但需要避免 two-stage 的结构性开销。
- P4/P4b/P4c/P4d 证明 build-side reorder 有效但有 TopR trade-off：
  - 512-only layout: 512 page reduction `18%~19%`，768 仅 `2%~3%`。
  - combined layout: 512/768 均约 `15%`。
  - 权重 sweep 证明不存在统一 Pareto layout。
- S0 official legacy diagnosis 显示 cross-query page reuse 极高，支撑 M1 page cache 方向。
- **M2-H Phase 1 diagnosis 显示 I/O wait 分布均匀，选择性 hybrid 不可行。**
- **M4-0 Oracle 显示 Query-level duplicate payload 只有 15.3%，primary layout 会增加 I/O 而非减少。**

因此，历史上继续投入以下方向的优先级应降低：

- 继续调 CoHit 权重。
- 全量 two-stage posting。
- chunk-locality / heuristic chunk pruning。
- PQ-code sort、payload copy、exact distance 微调。
- 单独 code async/cache，除非它成为新瓶颈。

## 4.1 Current Structural Diagnosis

这部分替代早期 `SPANN_Problem_old_20260430.md` 与 `SPANN_IO_Analysis_Plan_Fixed.md` 中较宽泛的判断，作为当前后续设计的诊断口径。

### Posting/Page Granularity

Legacy static SPANN 的关键成本不是“SSD 太慢”，而是 query 通常按 posting 的 aligned page span 读取，而不是按最终有价值候选读取：

```text
requested_bytes_per_query
≈ Σ aligned_page_bytes(fetched_posting_i)
```

更细分时应区分：

```text
requested_bytes_per_query
≈ directory/code/metadata bytes
 + payload/full-vector bytes
 + page-alignment waste
 + duplicate logical payload bytes
```

注意：`requested_bytes_per_query` 是 C++ 查询逻辑请求字节，不等同于设备 physical read bytes。OS cache、M1 page cache、zero-size request skip 和 batch read 行为都会改变实际设备读。

### Concurrency Bottleneck

高并发风险应描述为跨 query 的 pages/bytes-in-flight、page reuse 和 cache/coalescing 调度问题，而不是笼统地归因于“线程不够”或“SSD 必然堵塞”。

已验证信号：

- `ShardedPageCache` 在 `st8/st16` 有收益，说明 cross-query page reuse 可转化为吞吐提升。
- `st1/st4` 回退说明 cache lookup/memcpy/queue 固定开销不可忽略。
- M2-H posting trace 显示 I/O wait 没有集中到少量 bad postings，因此不应再押注 selective bad-posting hybrid。

仍需辅助观测的系统层信号包括 `SSD_queue_depth`、`read_latency_by_size`、`CPU_iowait` 和 PSI，但它们只能作为旁证，主证据仍应来自 query-level pages/bytes/read-wait/cache-hit。

### Boundary Replication And Duplicate Payload

边界点复制是 SPANN 保 recall 的有效机制，但它把召回问题转化为存储和带宽问题。当前更稳妥的表述是：

- 复制最直接影响 posting size、payload storage、I/O 放大、负载均衡和 cache 效率。
- 是否破坏 head graph 连通性或 routing，需要用 graph connectivity、search path、head degree、miss-case attribution 单独证明，不能直接断言。
- M4 的前置条件不是“副本一定很多”，而是必须量化 duplicate full payload contribution 和 primary payload locality。

### Code-First Building Blocks

`EnableADC`、`Rerank`、`Quantizer` 提供了 code-first/rerank 的部分 building blocks，但不能说明当前 legacy SSD posting 已经是完整 code-first 主路径。Full two-stage 已证明如果 physical payload pages 没有显著下降，compact scan/merge/read-plan/random fetch 会成为净新增成本。

### Routing Margin

“高维必然失败”不是准确表述。更需要关注的是 low cluster margin 与 head routing 区分度：

```text
cluster_margin(q) = distance(q, head_2) - distance(q, head_1)
true_nn_posting_rank = rank of posting containing the true nearest neighbor
heads_needed_for_target_recall = heads needed to reach target recall
```

这些指标用于判断问题是否已超出 posting I/O，变成 first-stage routing 质量问题。目前它们是后续补强项，不是已验证主瓶颈。

### Observability Status

已落地并继续作为主证据的指标：

- `requested_read_bytes_per_query`
- `pages_read_per_query`
- `postings_touched_per_query`
- `posting_elements_raw`
- `distance_eval_ratio`
- `duplicate_vector_read_ratio`
- `per_query_io_wait_time`
- `payload_trace` / `posting_trace`
- `query_io_stats.csv`
- cache hit/saved pages/lock wait

仍需补强的指标：

- duplicate full payload bytes/read-wait contribution
- primary/secondary assignment ratio
- primary payload candidates/page
- primary page cache hit by bytes saved
- routing margin and true-nearest-neighbor posting rank

不再作为 legacy 核心指标的口径：

- `useful_candidate_ratio = rerank_candidate_count / scanned_posting_element_count`。该指标更适合 two-stage/code-first；legacy 路径优先使用 `distance_eval_ratio`、`duplicate_vector_read_ratio` 和 `final_result_ratio`。

## 5. M1 Final Status: Sharded Page Cache

M1 最终方案：

- `ShardedPageCache`
- single-page posting only
- async insert
- no 2-hit admission
- 256MB cache
- cache hit request 直接处理，并跳过 zero-size AIO submission

代码入口：

- [PageCache.h](/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/PageCache.h)
- [PageCache.cpp](/home/ray/code/SPTAG/AnnService/src/Core/SPANN/PageCache.cpp)
- [ExtraStaticSearcher.h](/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraStaticSearcher.h)
- [SSDIndex.h](/home/ray/code/SPTAG/AnnService/inc/SSDServing/SSDIndex.h)
- [AsyncFileReader.cpp](/home/ray/code/SPTAG/AnnService/src/Helper/AsyncFileReader.cpp)

### Final st Sweep

日志：

- `results/io_analysis/m1_test/phase3_sharded_cache/st{1,4,8,16}_run{1,2,3}.log`
- `results/io_analysis/m1_test/phase3_sharded_cache/query_io_stats_st*.csv`

结果：

| st | Baseline QPS | Sharded QPS | Change | Status |
|---:|---:|---:|---:|---|
| 1 | 925.7 | 868.1 | `-6.2%` | Low-concurrency regression |
| 4 | 3630.7 | 3457.8 | `-4.8%` | Near regression threshold |
| 8 | 5827.5 | 6120.0 | `+5.0%` | Mainline success |
| 16 | 5835.5 | 6631.3 | `+13.6%` | Strong high-concurrency gain |

M1 验收结论：

- Recall@10 保持 `0.978319`。
- `st8` 主线超过官方目标线并达成 `+5%` 阶段收益。
- `st16` 高并发吞吐提升明显。
- global cache 引入的 p99.9 长尾已通过 shard lock 基本解决。
- `st1/st4` 回退仍存在，M1 不能作为所有并发场景的默认无条件开启项。

### Important Metrics

| Metric | Global Cache Issue | Sharded Cache Result |
|---|---:|---:|
| st8 QPS | `~5986` | `~6120` |
| st8 p99.9 total | `~10 ms` | `~4-5 ms` |
| st8 p99.9 lock wait | `~2.5 ms` | `~0.7 ms` |
| st8 lock wait total | `~449 ms` | `~60 ms` |

统计口径：

- `lock_wait_us` 包含 `TryGet` 读锁等待和 `Insert` 写锁等待。
- hit rate 只统计 single-page cacheable request，不是全量 posting/page hit rate。
- saved pages 是 cache hit 避免的内部 page read，不等价于 OS physical device read，除非另有系统级 I/O 统计验证。

### M1 Known Limits

- 低并发回退：`st1 -6.2%`，`st4 -4.8%`。
- 低并发下主要成本是 cache lookup、hash lookup、memcpy、async queue 和统计开销，不再是全局锁竞争。
- shard count 当前硬编码为 `16`。
- 分片级 LRU 对当前 256MB single-page cache 足够，但扩展到 1-2 page posting 时需要重新审查容量控制。
- 1-2 page posting cache 直接扩大 admission 已证伪：容量和队列压力会导致 QPS 回退。

## 6. Historical Decisions Kept for Reference

### S0 Diagnosis

S0 的作用是决定 M1/M2-H/M4 优先级。结论是 official legacy baseline 具有强 cross-query page reuse，因此优先做 M1。

关键输出：

- [s0_official_baseline_completion_report_20260502.md](/home/ray/code/SPTAG/results/io_analysis/s0_official_baseline_completion_report_20260502.md)
- `results/io_analysis/s0_baseline_20260502/summary.tsv`

### M1 Ablations

保留结论：

- B1+B2 no admission 优于 B1+B2+B3 2-hit admission。
- 2-hit admission 在当前 SIFT1M single-page working set fits cache 的条件下会降低 hit rate 和 QPS。
- 256MB 已达到 single-page cache hit plateau。
- zero-size request 必须从 AIO submission 中跳过。

已压缩的历史日志：

- `results/io_analysis/m1_test/phase2_cache_sweep/`
- `results/io_analysis/m1_test/phase2_st_sweep/`
- `results/io_analysis/m1_test/ablation_b3/`

### Two-Stage / Reorder

保留结论：

- Full two-stage 不再作为主线。
- Build-side reorder 曾可作为 M4 primary layout 的辅助 locality 工具，但 M4-0 oracle 已证明当前 SIFT1M query path 不值得继续实现该方向。
- Selective bad-posting hybrid 已由 M2-H Phase 1 证伪，不再继续实现 allowlist hybrid，除非未来新数据集重新证明 bad postings 高度集中。

## 7. Future Mainline

### M1 Productization: Concurrency-Gated Cache

Hypothesis:

`ShardedPageCache` 的收益来自高并发跨 query page reuse。低并发没有足够并发复用来抵消 lookup/memcpy/queue 开销。因此应按 `SearchThreadNum` 或运行时观察到的并发度启用 cache。

Plan:

1. 保持当前 `EnablePageCache` 为 bool，不把现有参数改成 `auto`。
2. 新增 `PageCacheMinSearchThreads=8`，或新增独立 `PageCacheMode=off/on/auto`。
3. auto 模式下，`SearchThreadNum >= 8` 启用 `ShardedPageCache`。
4. auto 模式下，`SearchThreadNum < 8` 关闭 page cache。
5. 保留强制开关用于实验和业务覆盖：`PageCacheMode=on/off`，或等价的 `EnablePageCache=true/false`。

Success:

- st1/st4 默认路径不低于 no-cache baseline。
- st8 保持 `>=6120 QPS` 级别。
- st16 保持 `>=6600 QPS` 级别。
- Recall/hash 不变。

Failure:

- auto gating 与 `EnablePageCache` bool 语义冲突，用户无法明确复现实验。
- st8 因判断条件错误未启用 cache。
- st1/st4 仍走 cache 并保留回退。
- 初始化或日志输出无法明确说明当前是否启用 cache。

Ablation expectations:

| Ablation | Expected observation |
|---|---|
| `EnablePageCache=false` | official/no-cache baseline |
| `EnablePageCache=true` | st1/st4 回退，st8/st16 提升 |
| `PageCacheMode=auto` or `PageCacheMinSearchThreads=8` | st1/st4 接近 no-cache，st8/st16 接近 sharded cache |
| `PageCacheMinSearchThreads=4/8/16` | 8 应是当前最合理默认阈值 |

### M2-H: Selective Hybrid Code-First

**Status: STOPPED (2026-05-03)**

Phase 1 Diagnosis Result:

| Metric | Value | Threshold | Pass |
|---|---:|---:|---:|
| Top 10% postings I/O wait contribution | 29.63% | >= 30% | ✗ |
| Top 5% postings I/O wait contribution | 18.74% | - | - |
| Top 1% postings I/O wait contribution | 7.07% | - | - |

Stop Criteria (from plan):

> "Stop M2-H if: selected postings are not concentrated"

**Conclusion: I/O wait distribution is too even for selective hybrid to be viable.**

Phase 1 Trace Files:

- `results/m2h/phase1/m2h_phase1_st8.ini` - Config
- `results/m2h/phase1/posting_trace_st8.csv` - Per-posting trace (636,527 records)
- `results/m2h/phase1/bad_postings.tsv` - Full posting metrics
- `results/m2h/phase1/bad_postings.bad_postings.txt` - Top 10% postings list

Key Insights:

| Page Count | Posting % | I/O Wait % | Avg Access |
|---|---:|---:|---:|
| 1 page | 30.8% | 25.0% | 4.2 |
| 2 pages | 59.8% | 60.1% | 4.5 |
| 3 pages | 8.8% | 13.5% | 6.3 |
| 4 pages | 0.6% | 1.4% | 9.8 |

I/O wait is distributed proportionally to posting count across page sizes. There is no "long tail" of bad postings that dominate I/O wait.

#### 根因分析：高复制 + 均匀分布

M4 诊断揭示了 I/O wait 均匀分布的根因：

```
VID 副本模式：
- 每个 VID 平均在 6.4 个 posting 中（高复制）
- 每个 VID 只跨越 0.0046% 的 posting（均匀分布）

结果：
- 每个 posting 都有 ~6.7 的平均 replica count
- 没有哪个 posting 特别"坏"
- I/O wait 分布均匀
```

详见 M4 章节的 "Replica Distribution Analysis"。

---

~~Hypothesis:~~

~~Full two-stage 失败的原因是所有 posting 都付 compact scan、merge、read-plan 和随机 payload fetch 成本。官方 legacy 路径虽然有读放大，但连续读和 pipeline 很快。更优策略是只对最亏的大 posting、热 posting、高重复 posting 启用 code-first；普通 posting 继续走官方 legacy。~~

Historical feasibility notes:

- 技术上可以复用 two-stage build/search 和 sidecar compact-code，但 mixed legacy/two-stage merge、dedupe、distance scale、TopR recovery 都会引入显著复杂度。
- 当前 trace 不满足继续投入的前置条件，因此这些 feasibility notes 仅作历史记录，不进入 immediate next steps。

Historical ablation expectations, not scheduled:

| Ablation | Expected observation |
|---|---|
| top 5% largest postings hybrid | bytes 小幅下降，CPU 开销最低 |
| top 10% largest postings hybrid | QPS/bytes 出现较好折中 |
| top 20% largest postings hybrid | bytes 继续下降但 CPU/merge 开销上升 |
| top wait-contribution postings hybrid | read-wait 下降应强于 largest-only selection |
| hot posting hybrid | st8/p95 收益应强于 st1 |
| high-duplicate posting hybrid | duplicate VID/page 应下降 |
| TopR 128/256/384 | Recall 上升伴随 payload pages 上升，应找到拐点 |
| hybrid + M1 | read wait 下降应强于单独 hybrid |

### M4: Primary Payload Dedupe / Route-Payload Split

**Status: STOPPED (2026-05-03 M4-0 Oracle FAIL)**

**M4-0 Oracle Result: FAIL**

| Criterion | Threshold | Actual | Status |
|-----------|-----------|--------|--------|
| Duplicate payload bytes in query path | >= 30% | **15.3%** | **FAIL** |
| Primary pages/query reduction | >= 15% | **-959%** (worse) | **FAIL** |

**根因**：Storage-level 复制率 (76.7%) 不等于 query-level duplicate payload/record ratio (15.3%)。

- 全局视角：每个 VID 在 6.4 个 posting 中
- 查询视角：每个查询访问的 64 个 posting 中，duplicate payload/record ratio 只有 15.3%
- 查询访问模式是局部的，全局复制不转化为查询级重复

**结论**：M4 会增加 I/O，不是减少。Legacy `~119 pages/query` vs best simulated M4 primary layout `~1257 pages/query`（约 `10.6x` legacy）。停止 M4。

**Simulation caveat**：M4-0 脚本中的 `VIDOrder`、`PrimaryPostingOrder`、`HotnessOrder` 没有形成严格独立的 page-packing ablation；当前真正有区分度的 primary layout 主要是 `CoHitTraceOrder`。这个脚本限制不改变停止结论，因为 best simulated layout 仍远差于 legacy。

**M4-0 Oracle Report**: `results/m4_oracle/m4_oracle_report_20260503.md`

---

#### Rejected Design Archive

以下内容仅保留为历史设计与反例材料，不再是执行计划。M4-0 oracle 已经证伪该方向在当前 SIFT1M `UInt8 + DEFAULT` query workload 下的 query-I/O 价值；不得继续推进 M4-1 sidecar build 或 M4-2 search path，除非未来新数据集重新满足 M4-0 门槛。

Scope clarification:

M4 不复用 full two-stage posting 作为主线，不依赖 `SSDPostingFormatVersion=2`，不做 compact-code TopR 粗筛，不做 chunk pruning，也不改变 legacy 访问的 posting/VID 候选集合。M4 的首版目标是 exact-safe 的存储结构改造：

```text
legacy: posting record = [VID + full vector payload] copied in every replica
M4:     posting route = [VID] or [VID + lightweight metadata]
        full vector payload = one global primary copy per VID
```

此前 two-stage 的结论仍作为反例有效：如果 payload fetch 变成随机读，或者新增 scan/merge/read-plan 成本超过 page saving，性能会下降。M4-0 的实际结果正是 primary payload layout 无法把去重后的 payload 读保持在足够少的 physical pages/read-wait 内。

Phase 1 Diagnosis Result:

| Metric | Value | Historical implication |
|--------|-------|----------------|
| VIDs in multiple postings | 99.7% | Strong duplicate evidence |
| Average replicas per VID | 6.41 | High replication factor |
| Storage overhead | 84.4% (686 MB -> 160 MB) | Significant storage reduction potential |
| Per-posting duplicate ratio | 99.9% | Almost all VIDs are duplicates |

Historical conclusion before M4-0: storage dedupe looked viable from global replica statistics, but query I/O/QPS benefit was unproven.

Current corrected conclusion after M4-0: storage dedupe alone is not a sufficient optimization target. The measured query path has too little duplicate payload and too poor primary locality, so M4 is stopped for current SIFT1M.

Diagnosis Report: `results/m4_diagnosis/m4_diagnosis_report_20260503.md`

Historical findings:

1. **Storage**: 76.7% reduction potential (526 MB saved)
2. **Rejected I/O hypothesis**: current dedupe happens after disk read, so M4 might reduce read bytes if query-level duplicate payload were high enough
3. **Observed blocker**: primary locality is poor; random primary fetch dominates and erases storage-level savings

#### Code-Level Motivation

当前 legacy static posting 的构建和查询路径说明 M4 的问题定义很清楚：

- `GetPostingListFullData(...)` 写出的每条 posting record 是 `VID + full vector payload`，副本会物理复制 full payload。
- legacy `SearchIndex(...)` 先按 posting 的 aligned page span 读取整段 posting，再解析每条 record。
- `m_deduper.CheckAndSet(vectorID)` 发生在 posting buffer 读入之后，因此只能省 distance compute，不能省重复 payload I/O。
- 已有 two-stage payload planner 是 posting-local payload offset 设计，不能直接解决 global duplicate payload；M4 需要新的 global primary payload planner。

因此，当时 M4 被设计为 route/payload split，而不是继续扩展 two-stage coarse-rerank pipeline。M4-0 之后，该设计被保留为 rejected design，不再进入实现。

#### Replica Distribution Analysis

关键发现：高复制率 + 均匀分布 = 所有 posting 都有 storage 去重空间；但 M4-0 证明当前 workload 下这些空间收益不能转化为 query I/O 收益。

| Posting Size | Count | Avg Replica Count |
|--------------|------|-------------------|
| Small (<50 VIDs) | 104,844 | 6.65 |
| Medium (50-100) | 32,987 | 6.96 |
| Large (>=100) | 418 | 7.07 |

**洞察**：

- 每个 VID 平均在 ~6.4 个 posting 中（高复制）
- 每个 VID 只跨越 0.0046% 的 posting（均匀分布）
- 结果：每个 posting 都有相似的重复 VID 比例

**对 M2-H vs M4 的影响**：

```
高复制 (6.4x) + 均匀分布
    ↓
每个 posting 都有 ~6.7 的平均 replica count
    ↓
没有"特别坏"的 posting
    ↓
M2-H 选择性优化无效 ❌ (I/O wait 分布均匀)
M4 全局 payload 去重 storage 有潜力，但 query I/O 已由 M4-0 证伪
```

这解释了为什么：
- M2-H 证伪：top 10% posting 只贡献 29.63% I/O wait（分布均匀）
- M4 storage 方向只在空间层面成立；query I/O/QPS 已由 M4-0 oracle 证伪

---

#### Rejected Storage Layout

以下 layout 是被 M4-0 证伪后的历史方案，不应在当前 SIFT1M 主线中实现：

```text
ssdIndex                    # existing legacy/fallback index
ssdIndex.m4.meta             # sidecar manifest
ssdIndex.m4.payload          # global primary payload store, one full vector per VID
ssdIndex.m4.loc              # VID -> primary payload location table
ssdIndex.m4.route            # posting route records
ssdIndex.m4.route.dir        # posting -> route byte range
```

`m4.payload`:

```cpp
struct M4PrimaryPayloadRecord {
    ValueType vector[dim];
};
```

`m4.loc` should be a dense array indexed by VID and loaded into memory:

```cpp
struct M4PayloadLocation {
    uint64_t offset;           // offset in m4.payload
    uint32_t bytes;            // dim * sizeof(ValueType)
    uint32_t primaryPostingID; // debug/analysis field
};
```

`m4.route` first version must be exact-safe:

```cpp
struct M4RouteRecordV1 {
    int vid;
};
```

Do not include compact code or TopR in M4 V1. A future `VID + code` route can only be considered after exact-safe M4 proves that primary payload fetch, not route scanning, is the remaining bottleneck.

Route records must be page-packed across postings. If each posting route is independently 4KB-aligned, route page inflation can erase the benefit of replacing full payload with VID-only records.

#### Rejected Query Path

Rejected M4 exact-safe search path:

```text
1. Use the same head search and posting IDs as legacy.
2. Read m4.route byte ranges for those postings.
3. Parse VIDs and apply the same query-local dedupe.
4. Use m4.loc[vid] to build a global primary payload page read plan.
5. Deduplicate, sort, and batch read primary payload pages from m4.payload.
6. Compute exact distance on full payload.
7. Insert result with the same AddPoint semantics as legacy.
```

This path intentionally kept the candidate set unchanged. It is now rejected because primary payload pages/query are too high, not because of recall risk.

#### Primary Layout Simulation Caveat

Primary payload order was the central M4 risk. The M4-0 script attempted these layout ablations:

```ini
M4PrimaryLayout=VIDOrder
M4PrimaryLayout=PrimaryPostingOrder
M4PrimaryLayout=CoHitTraceOrder
M4PrimaryLayout=HotnessOrder
```

- `VIDOrder`: simplest lower bound.
- `PrimaryPostingOrder`: intended to cluster payloads by selected primary posting.
- `CoHitTraceOrder`: the only materially distinct simulated layout in the current script.
- `HotnessOrder`: intended to cluster hot vectors.

Important caveat: current `scripts/m4_oracle_simulation.py` does not implement independent page packing for `PrimaryPostingOrder` and `HotnessOrder`; they collapse to the same page order as `VIDOrder`. This weakens the interpretation of individual layout rows, but not the stop decision, because `CoHitTraceOrder` is the best simulated layout and still needs `~1257 pages/query`.

#### Historical Hypotheses And Outcome

H1 Storage Hypothesis:

Current full payload is physically replicated across postings. Because SIFT1M shows 99.7% replicated VIDs and average replicas 6.41, global primary payload storage should reduce payload storage by `60%~75%`.

Outcome: storage hypothesis remains true as a space-saving observation, but it is not enough for performance.

H2 Query I/O Hypothesis:

Legacy dedupe happens after posting read. If M4 replaces secondary full payload with VID-only route records and reads each deduped VID from a primary payload store, requested bytes/read-wait should decrease when primary layout has enough locality.

Outcome: failed. Query-level duplicate payload/record ratio is only 15.3%, and primary locality is poor.

H3 Locality Hypothesis:

M4 performance is governed by `primary_candidates_per_page`, not by storage saving alone. M4 succeeds only if primary layout and batching keep primary payload pages/query materially below legacy posting pages/query.

Outcome: failed. Best simulated layout still needs `~1257 pages/query` vs legacy `~119 pages/query`.

H4 Cache Interaction Hypothesis:

M4 might combine with M1 because primary payload pages are shared across queries. If high-concurrency primary page reuse existed, M4+M1 could improve st8/st16 more than M4-only.

Outcome: not pursued because M4-0 failed before implementation.

#### Technical Feasibility, Now Irrelevant For Current Mainline

Feasibility is medium and controllable if implemented as sidecar phases:

- Build complexity is manageable: reuse existing `Selection`/posting assignment to write route records; write one dense payload record per VID.
- Load complexity is manageable: load `m4.loc` and route directory; open `m4.payload` as an additional DiskIO file.
- Search complexity is medium-high: requires a new route-read plan and a global primary payload page-read plan.
- Correctness risk is high enough to require checksums/hashes: any VID-to-offset mismatch silently corrupts exact distance.
- Performance risk is dominated by primary random read, not CPU parse cost.

Implementation should not replace legacy files in-place. This remains useful if a future dataset reopens M4, but current mainline must not implement it.

#### Baseline Compatibility, If Reopened On A Future Dataset

Compatibility would be high if M4 were reopened as sidecar:

- Existing legacy index remains loadable and searchable.
- M4 can be enabled only when `ssdIndex.m4.meta` exists and matches dataset dimension/value type/vector count.
- If sidecar load fails, search must either fail loudly or fall back to legacy based on explicit config; silent partial fallback is not allowed for benchmark runs.
- M4 V1 should not require quantizer, `EnableTwoStagePosting`, or `SSDPostingFormatVersion=2`.

#### Rejected Expected Gain

Storage:

- SIFT1M storage oracle: `686.11 MB -> ~159.63 MB`, saving `526.48 MB / 76.7%`.
- This is now only a storage observation, not a performance plan.

Query:

- Rejected by M4-0: best simulated primary layout is `~10.6x` more pages than legacy, so previous QPS targets are invalid.

#### Historical Success Criteria And Observed Failure

M4-0 oracle success:

- pre-dedupe duplicate payload bytes are `>=30%` of legacy logical posting payload bytes.
- best primary layout oracle reduces primary pages/query by `>=15%` versus legacy pages/query.
- primary candidates/page is materially better than VID-order lower bound.
- candidate VID set is identical to legacy before distance computation.

Observed M4-0 failure:

- duplicate payload bytes in query path: `15.3% < 30%`
- best primary pages/query reduction: `-959%`, i.e. much worse than legacy
- do not proceed to M4-1 sidecar build or M4-2 exact-safe search

#### Failure Criteria Triggered

- Sidecar storage reduction is below `40%`, indicating route/pointer/padding overhead has erased storage value.
- M4 exact-safe search changes final hash or exact result set, indicating mapping, offset, parse, or distance-input corruption.
- primary pages/query is not lower than legacy pages/query.
- payload bytes decrease but payload read wait increases, indicating random I/O/IOPS dominates.
- route read pages approach legacy posting read pages, indicating route packing failed.
- M4-0 already shows primary pages/query far above legacy; implementation is unnecessary.

#### Independent Failure Signals

- primary candidates/page below `2`.
- primary page cache hit rate remains low at st8/st16 and does not improve with concurrency.
- primary read p95/p99 latency exceeds legacy posting read p95/p99.
- route read wait exceeds `30%` of total M4 I/O wait.
- payload bytes fall while SSD/device queue latency rises.
- any nonzero `VID -> payload` checksum mismatch.
- final result hash varies across repeated same-thread-count runs.
- route page inflation causes VID-only route bytes to require near-legacy page count.

#### Historical Ablation Expectations And Actual Interpretation

| Ablation | Expected observation |
|---|---|
| M4-0 pre-dedupe trace | duplicate VID/payload bytes should be much higher than post-dedupe trace; below `20%` means weak query-I/O upside |
| `VIDOrder` primary layout | easiest correctness baseline; storage should pass, query locality likely weak |
| `PrimaryPostingOrder` layout | not independently implemented in current oracle script; should not be cited as a separate measured result |
| `CoHitTraceOrder` layout | best simulated layout, but still `~1257 pages/query`, so M4 fails |
| `HotnessOrder` layout | not independently implemented in current oracle script; should not be cited as a separate measured result |
| route record = `VID` | exact-safe, smallest route, highest primary fetch count; must be V1 baseline |
| route record = `VID + location` | fewer loc table lookups but larger route; should be rejected if QPS does not improve |
| page-packed route | route pages should be much lower than legacy posting pages |
| posting-aligned route | expected to fail or underperform due to 4KB inflation |
| M4 without M1 cache | not executed; invalidated before implementation by M4-0 |
| M4 with M1 cache | not executed; invalidated before implementation by M4-0 |
| primary batch pages 1/4/8/16 | not executed; primary page count was already too high in oracle |

#### Final Decision

M4 is stopped for current SIFT1M `UInt8 + DEFAULT`. Do not implement M4 sidecar build, M4 exact-safe search, M4+M1 cache combination, or M4 route-code extension on this workload.

### M3: Exact-Safe Chunk Pruning (Low-Priority Fallback)

Hypothesis:

如果 chunk bound 足够紧且语义安全，可以跳过部分 code/payload，减少 posting 内无效扫描。但历史 chunk-locality 和 heuristic pruning 已显示 recall 风险，因此 M3 不是近期主线，只能在 exact-safe bound 或 truth-pruned attribution 建立后作为 fallback 继续。

Technical Feasibility:

中等，但不适合作为近期主线：
- L2 下可以考虑 centroid-radius lower bound。
- 非 L2 metric 不能直接套用 L2 bound。
- chunk 构造质量决定 prune ratio。

Baseline Compatibility:

低到中：
- 需要 chunked format 或 sidecar directory。
- 必须支持 fallback。
- 默认不能开启 heuristic pruning。

Expected Gain:

不确定：
- 若 bound 紧：payload/code pages 可能下降 `10%~20%`。
- 若 bound 松：收益接近 0。
- 若 heuristic：容易以 recall 损失换性能，不可作为默认。

Success:

- zero-recall-loss pruning。
- truth-pruned attribution 显示被剪 chunk 不含 truth/topK 必要候选。
- code/payload pages 下降 `>=10%`。
- st8 QPS 提升且 result hash 稳定。

Failure:

- prune ratio 上升但 recall 下降。
- 提高 TopR 恢复 recall 后 QPS 下降。
- chunk metadata/code read 成本抵消 pruning。
- chunk radius 普遍过大，prune ratio 很低。

Independent Failure Signals:

- 被 prune 的 chunk 中出现 truth VID。
- query-level result hash 随线程数变化。
- metric 非 L2 时仍启用 L2 bound。
- chunk directory read wait 成为新瓶颈。

Ablation expectations:

| Ablation | Expected observation |
|---|---|
| exact bound off vs on | on 应降 pages 且 recall/hash 不变 |
| chunk size 32/64/128 | 小 chunk prune 多但 metadata/code overhead 高 |
| spatial chunk vs write-order chunk | spatial chunk radius 应更小，prune 更强 |
| M3 + M1 cache | directory/cache 应降低 metadata overhead |

## 8. Stop / Continue Rules

Continue M1 only for productization work if:

- auto concurrency gating removes st1/st4 regression.
- st8/st16 gains remain stable across 3+ runs.
- p99.9 remains close to baseline.

Stop further M1 tuning if:

- proposed change only adjusts admission/cache size without addressing a measured bottleneck.
- st8/st16 QPS no longer improves.
- tail latency or low-concurrency regression worsens.

Continue M2-H only if:

- a new dataset or workload shows bad postings are strongly concentrated by bytes/wait.
- top 10% postings contribute well above the previous stop threshold, not merely around `30%`.
- a selective prototype can reduce payload read without requiring excessive TopR.

Stop M2-H if:

- selected postings are not concentrated.
- code-first overhead dominates.
- recall recovery removes all I/O savings.

Reopen M4 only on a future dataset/workload if:

- duplicate payload/VID evidence is strong.
- M4-0 oracle shows duplicate full-payload contribution in the query path, not only in storage.
- primary payload locality can be preserved by ordering/cache.
- best simulated primary layout reduces pages/query by the planned threshold.
- M4 remains exact-safe until sidecar route/payload correctness is proven.

Stop M4 if:

- primary fetch randomization dominates.
- space savings do not translate into read-wait reduction.
- route page inflation makes VID-only route reads approach legacy posting page reads.
- exact-safe M4 changes final result hash or payload checksum validation fails.
- **M4-0 Oracle FAIL (2026-05-03): Query-level duplicate ratio 15.3% < 30% threshold.**

Continue M3 only if:

- exact-safe bound or truth-pruned attribution can be established.
- zero-recall-loss pruning shows clear page/read wait benefit.

Stop M3 if:

- bound is too loose for meaningful pruning.
- chunk metadata/code read cost offsets pruning.
- recall loss cannot be avoided without heuristic that risks correctness.

## 9. Immediate Next Steps

1. Implement M1 productization gating:
   - Keep `EnablePageCache` bool-compatible.
   - Add `PageCacheMinSearchThreads=8` or a separate `PageCacheMode=off/on/auto`.
   - Default auto behavior: enable page cache only for `SearchThreadNum >= 8`.
   - Log explicit decision at search startup.
2. Re-run final validation:
   - st1/st4/st8/st16, three runs each.
   - Compare QPS, Recall, p95, p99, p99.9, cache hit rate, lock wait.
3. Freeze M1 after gating if results hold.
4. Do not implement M2-H allowlist hybrid on current SIFT1M evidence.
5. ~~Start M4-0 oracle~~ **M4-0 Oracle completed (2026-05-03): FAIL, stop M4.**

### 项目阶段性结论

**在当前 strict SIFT1M/SIFT10M UInt8 + DEFAULT 口径下，legacy SPANN 结构的局部改造空间已基本穷尽。**

M1 是唯一已验证正收益方向（仅限 SIFT1M 小规模数据集），剩余工作是 productization（并发门控）。M2/M2-H/M4 均已通过实验证伪。SIFT10M 验证进一步确认，随着数据集规模扩大，M1 的收益也会消失。如需进一步大幅提升（7000+ QPS），需要换成更大的算法问题（routing 改造、replica budget 重构），这已超出当前"legacy 结构优化"的范围。

Do not continue spending time on:

- 2-hit admission for SIFT1M current cache size.
- Increasing cache size beyond 256MB for single-page cache.
- Full two-stage search.
- Selective bad-posting hybrid on current M2-H trace.
- CoHit weight sweeps as a standalone path.
- Chunk pruning without exact-safe bound or strong truth-pruned attribution.
- **M4 primary-secondary dedupe (query-level duplicate ratio too low).**
