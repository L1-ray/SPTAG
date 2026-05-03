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
- 对默认高并发场景，`st8` 必须超过官方目标线；阶段性强成功线仍是 `st8 QPS >= 6500`。

## 2. Related Documents

- [SIFT1M_Official_Alignment_Summary.md](/home/ray/code/SPTAG/SIFT1M_Official_Alignment_Summary.md): official strict `UInt8 + DEFAULT` baseline、sweep 结果和当前目标线来源。
- [SPANN_M2_M3_Code_Plan_Review_20260501.md](/home/ray/code/SPTAG/SPANN_M2_M3_Code_Plan_Review_20260501.md): two-stage、P1-P4d、payload attribution、CoHit layout 的审查和结论。
- [SPANN_M2_M3_UInt8_Optimization_Memo.md](/home/ray/code/SPTAG/SPANN_M2_M3_UInt8_Optimization_Memo.md): UInt8 + DEFAULT 口径下的实验备忘录和复现实验入口。

## 3. Executive Summary

当前结论：

- Full two-stage posting 已降级，不再作为超过官方 baseline 的主线。
- CoHit/reorder 对 page reduction 有效，但单独不足以超过官方 baseline。
- M1 page cache 路线已完成核心验证：`ShardedPageCache + single-page cache + no admission` 在 `st8/st16` 有稳定收益。
- M1 不适合低并发默认开启：`st1/st4` 下 cache 固定开销超过 I/O 节省。
- 下一阶段主线应从“继续调 page cache/admission”转向“高并发策略化启用 + 更结构性的 I/O 调度/混合读取路径”。

当前推荐配置：

```ini
[SearchSSDIndex]
EnablePageCache=true
PageCacheMaxBytes=268435456
```

使用条件：

- `SearchThreadNum >= 8`: 推荐启用 `ShardedPageCache`。
- `SearchThreadNum < 8`: 推荐默认关闭 cache，除非业务明确吞吐优先且接受低并发回退。

## 4. Evidence So Far

已验证事实：

- 官方 baseline 的缺陷真实存在：posting 过大、full payload 过重、I/O 读放大、副本空间膨胀、并发拥塞、cache 粒度粗、重复 VID/page 读取。
- All-in two-stage 失败原因明确：它降低了部分 payload/page 读取，但引入 compact scan、merge、read-plan、random payload fetch 等额外成本，matched recall 下整体 QPS 明显落后官方。
- P1/P2 attribution 证明 two-stage 主要瓶颈是 payload I/O wait，而不是 compact-code CPU pipeline。
- P3 oracle 显示理论 page reduction 上限约 `40%~50%`，说明 locality 优化有空间，但需要避免 two-stage 的结构性开销。
- P4/P4b/P4c/P4d 证明 build-side reorder 有效但有 TopR trade-off：
  - 512-only layout: 512 page reduction `18%~19%`，768 仅 `2%~3%`。
  - combined layout: 512/768 均约 `15%`。
  - 权重 sweep 证明不存在统一 Pareto layout。
- S0 official legacy diagnosis 显示 cross-query page reuse 极高，支撑 M1 page cache 方向。

因此，历史上继续投入以下方向的优先级应降低：

- 继续调 CoHit 权重。
- 全量 two-stage posting。
- chunk-locality / heuristic chunk pruning。
- PQ-code sort、payload copy、exact distance 微调。
- 单独 code async/cache，除非它成为新瓶颈。

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
- Build-side reorder 仍可作为未来 M2-H/M4 的辅助 locality 工具。
- 如果未来做 selective hybrid，只允许对 trace 证明的 bad postings 使用 code-first，不再全量启用。

## 7. Future Mainline

### M1 Productization: Concurrency-Gated Cache

Hypothesis:

`ShardedPageCache` 的收益来自高并发跨 query page reuse。低并发没有足够并发复用来抵消 lookup/memcpy/queue 开销。因此应按 `SearchThreadNum` 或运行时观察到的并发度启用 cache。

Plan:

1. 增加或复用配置策略：`EnablePageCache=auto`，或新增 `PageCacheMinSearchThreads=8`。
2. `SearchThreadNum >= 8` 自动启用 `ShardedPageCache`。
3. `SearchThreadNum < 8` 默认关闭 page cache。
4. 保留强制开关：`EnablePageCache=true/false` 用于实验和业务覆盖。

Success:

- st1/st4 默认路径不低于 no-cache baseline。
- st8 保持 `>=6120 QPS` 级别。
- st16 保持 `>=6600 QPS` 级别。
- Recall/hash 不变。

Failure:

- auto gating 导致配置语义混乱，用户无法明确复现实验。
- st8 因判断条件错误未启用 cache。
- st1/st4 仍走 cache 并保留回退。
- 初始化或日志输出无法明确说明当前是否启用 cache。

Ablation expectations:

| Ablation | Expected observation |
|---|---|
| `EnablePageCache=false` | official/no-cache baseline |
| `EnablePageCache=true` | st1/st4 回退，st8/st16 提升 |
| `EnablePageCache=auto` | st1/st4 接近 no-cache，st8/st16 接近 sharded cache |
| `PageCacheMinSearchThreads=4/8/16` | 8 应是当前最合理默认阈值 |

### M2-H: Selective Hybrid Code-First

Hypothesis:

Full two-stage 失败，但 selective code-first 可能在 bad postings 上有效。只有当少量 postings 对 bytes/read-wait 贡献集中时，hybrid 才值得做。

Compatibility:

- legacy path 保留默认。
- selected postings 使用 sidecar compact-code/read-plan。
- 未命中的 posting 回退 legacy。

Success:

- selected postings payload/read wait 下降 `>=25%`。
- total requested/physical bytes 下降 `>=10%`。
- st8 QPS 达到 `>=6500`，且 p95 不劣化。
- selected ratio 出现明确拐点，不是越多越好。

Failure:

- TopR recovery 吃掉 payload savings。
- selected postings 不是真实 wait 主贡献者。
- code scan/merge/read-plan 成为新瓶颈。
- mixed path result hash 不稳定。

Ablation expectations:

| Ablation | Expected observation |
|---|---|
| top 5% bad postings | CPU overhead low, bytes modestly lower |
| top 10% bad postings | best expected trade-off |
| top 20% bad postings | more bytes saved, risk CPU/merge overhead |
| M2-H only | localized gain if bad postings concentrated |
| M2-H + M1 | should beat either alone if read wait remains dominant |

### M4: Primary-Secondary Payload Dedupe

Hypothesis:

Replica/full payload duplication creates space and read amplification. A primary-secondary payload layout can reduce storage and duplicate payload reads while preserving routing diversity.

Compatibility:

- Requires sidecar or new format.
- Must keep legacy fallback.
- Needs robust VID -> primary payload pointer validation.

Success:

- full payload storage drops `>=20%`.
- duplicate full payload reads drop.
- primary payload physical bytes drop `>=15%`.
- with M1, st8 QPS reaches `>=6800` if primary locality is good.

Failure:

- primary fetch becomes too random.
- pointer/code overhead offsets savings.
- duplicate VID/full-payload ratio is lower than expected.
- payload bytes drop but read wait does not.

Ablation expectations:

| Ablation | Expected observation |
|---|---|
| primary ordered by VID | simple baseline, locality may be weak |
| primary ordered by CoHit trace | fewer primary pages if query locality transfers |
| secondary code-only | lowest space, highest recall risk |
| secondary code + bound info | more space, fewer primary fetches |
| M4 only | space gain first, QPS gain uncertain |
| M4 + M1 | strongest expected read-wait reduction |

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

- trace shows bad postings are concentrated by bytes/wait.
- selective prototype reduces payload read without requiring excessive TopR.

Stop M2-H if:

- selected postings are not concentrated.
- code-first overhead dominates.
- recall recovery removes all I/O savings.

Continue M4 only if:

- duplicate payload/VID evidence is strong.
- primary payload locality can be preserved by ordering/cache.

Stop M4 if:

- primary fetch randomization dominates.
- space savings do not translate into read-wait reduction.

## 9. Immediate Next Steps

1. Implement M1 productization gating:
   - Add auto/threshold semantics or equivalent config.
   - Default: enable page cache only for `SearchThreadNum >= 8`.
   - Log explicit decision at search startup.
2. Re-run final validation:
   - st1/st4/st8/st16, three runs each.
   - Compare QPS, Recall, p95, p99, p99.9, cache hit rate, lock wait.
3. Freeze M1 after gating if results hold.
4. Start M2-H trace selection only after identifying bad postings by bytes/wait contribution.
5. Keep M4 as second structural path if duplicate payload evidence is strong.

Do not continue spending time on:

- 2-hit admission for SIFT1M current cache size.
- Increasing cache size beyond 256MB for single-page cache.
- Full two-stage search.
- CoHit weight sweeps as a standalone path.
- Chunk pruning without exact-safe bound or strong truth-pruned attribution.
