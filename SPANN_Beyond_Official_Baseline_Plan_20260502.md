# SPANN Beyond Official Baseline Plan (2026-05-02)

## 1. Goal and Baseline

目标不是继续证明 two-stage posting 可以工作，而是在 strict `UInt8 + DEFAULT` 口径下超过官方 baseline。

官方性能目标线：

| Metric | Official baseline |
|---|---:|
| Dataset | SIFT1M UInt8 |
| Recall@10 | `0.978319` |
| Best representative QPS | `~5945` at `SearchThreadNum=8` |
| Avg latency | `~1.34 ms` |
| P95 latency | `~1.60 ms` |
| Requested bytes | `~486 KB/query` |

硬验收标准：

- Recall@10 必须 `>= 0.978319`，或明确作为 low-recall 档位单独标注。
- `SearchThreadNum=8` QPS 必须超过 `5945`；阶段性成功线为 `>=6500`。
- avg latency 不高于 `1.34 ms`，p95 latency 不高于 `1.60 ms`。
- query-level result hash / final recall 必须稳定；任何性能提升不能来自结果语义漂移。
- 性能提升必须能被 pages/bytes/read-wait/cache-hit/coalescing 等指标解释。

## 2. Related Documents

- [SIFT1M_Official_Alignment_Summary.md](/home/ray/code/SPTAG/SIFT1M_Official_Alignment_Summary.md): official strict `UInt8 + DEFAULT` baseline 的对齐记录和 sweep 汇总，是当前 `Recall/QPS/latency/requested bytes` 目标线的来源。
- [SPANN_M2_M3_Code_Plan_Review_20260501.md](/home/ray/code/SPTAG/SPANN_M2_M3_Code_Plan_Review_20260501.md): M2/M3 two-stage 代码审查、阶段性实验、P1-P4d 结论和失败方向整理，用于判断哪些优化已经证伪或降级。
- [SPANN_M2_M3_UInt8_Optimization_Memo.md](/home/ray/code/SPTAG/SPANN_M2_M3_UInt8_Optimization_Memo.md): UInt8 + DEFAULT 口径下的实验备忘录，记录 two-stage、payload attribution、CoHit layout、P4d 权重 sweep 的关键数据和复现实验入口。

## 3. Current Evidence

官方 baseline 的缺陷真实存在，包括 posting 过大、full payload 过重、I/O 读放大、副本空间膨胀、并发拥塞、cache 粒度过粗、重复 VID/page 读取。但当前 all-in two-stage 没有超过官方，原因是它只解决了其中一小部分问题，同时引入了新成本。

已验证事实：

- metadata query 热路径问题已解决；metadata query read 可降到 `0`。
- code physical read 可通过 code cache 降到 `0`。
- 当前 two-stage 主瓶颈是 payload read wait：
  - `Fetch Payload & Rerank` 占 two-stage phase time `73.8%`。
  - `Payload Read Wait` 占该阶段 `89.0%`。
  - payload read wait 折合 two-stage 总 phase time `66.8%`。
- P3 oracle 证明 theoretical page reduction 上限有 `40%~50%`。
- P4/P4b/P4c/P4d 证明 build-side CoHit reorder 有效，但不是足以超过官方的单独主线：
  - 512-only layout: 512 page reduction `18%~19%`，768 only `2%~3%`。
  - combined layout: 512/768 都约 `15%`。
  - 权重 sweep 证明不存在满足 `512>=18%` 且 `768>=10%` 的统一 layout。
- 当前 two-stage matched recall 下 payload KB 与官方接近，且 pipeline/read-wait 开销更高，因此整体 QPS 明显落后。

结论：

继续调 CoHit 权重、chunk-locality、PQ-code sort、payload copy、exact distance 或 code async 都不是超越官方的主线。真正有希望的是在官方 baseline 上叠加兼容的 I/O/cache/coalescing，再选择性引入 code-first 或 primary-secondary 结构。

## 4. Direction Ranking

| Priority | Direction | Feasibility | Baseline compatibility | Complexity | Expected gain |
|---:|---|---|---|---|---|
| S0 | Official baseline trace-only diagnosis | High | High | Low | No direct gain, decides next step |
| M1 | Global I/O broker + page cache + in-flight coalescing | High | High | Medium | QPS `+5%~15%`, p95/p99 `-15%~35%` |
| M2-H | Hybrid selective code-first for bad postings | Medium-high | Medium-high | Medium-high | QPS `+8%~20%` if bad postings dominate |
| M4 | Primary-secondary payload dedupe | Medium | Medium | High | QPS `+10%~25%`, space `-20%~40%` if replica duplication dominates |
| M3 | Exact-safe chunk pruning | Medium | Low-medium | High | Uncertain, recall risk |
| M6 | Routing / graph-assisted / learned router | Low-medium | Low | Very high | Potentially high, not short-term |

Recommended path:

```text
S0 trace-only diagnosis
-> M1 on official baseline
-> M2-H selective hybrid only if trace supports it
-> M4 primary-secondary only if duplicate payload/replica evidence is strong
-> M3/M6 only after above paths are exhausted or trace proves routing/chunk is dominant
```

## 5. S0: Trace-Only Diagnosis Before Code Changes

### Hypothesis

官方 baseline 是否能被超过，取决于真实瓶颈是否集中在可共享、可合并、可缓存、可选择性跳过的 I/O 上。必须先量化 page 热度、跨 query page reuse、duplicate VID/page、posting read wait 贡献和大 posting 长尾。

### Implementation Scope

只做观测，不改搜索语义：

- Per query:
  - posting IDs visited
  - posting logical bytes
  - physical page IDs
  - duplicate page count
  - duplicate VID count if available
  - batch read wait / read latency
- Global summary:
  - top page hotness
  - top posting bytes and wait contribution
  - cross-query page reuse distance
  - duplicate page ratio
  - duplicate VID ratio
  - cacheable bytes by page frequency bucket

### Success Results

- 能明确回答 M1/M2-H/M4 哪个方向最值得先做。
- top hot pages/postings 对 read wait 的贡献足够集中，例如 top 10% postings 贡献 `>=30%` read wait。
- 跨 query page reuse 或 in-flight duplicate 明显，例如 st8 下 `>=10%` physical pages 存在短窗口重复。
- replica/duplicate VID 或 duplicate payload read 有可量化占比。

### Failure Results

- page 热度分布很平，跨 query reuse 很低。
- top postings 对 read wait 贡献不集中。
- duplicate VID/page 占比低到不足以支持 M1/M4。
- trace 口径无法与 raw log 的 requested bytes/read latency 对齐。

### Independent Failure Signals

- summary 中 pages/bytes 与 detailed CSV 或 raw log 差异超过 `2%`。
- 同一 query 重跑 page trace 不稳定。
- page ID key 没有包含 file/blob identity，导致不同 posting/page 被误合并。
- trace 开启后 QPS/latency 大幅变化，观测本身污染性能。

### Ablation Expectations

| Ablation | Expected observation |
|---|---|
| st1 vs st8 trace | st8 应暴露更多 in-flight duplicate 和 queue wait |
| ir32 vs ir64 | ir64 requested bytes/page/duplicate ratio 应高于 ir32 |
| hot page window 1/2/4/8 ms | reuse 应随窗口变大上升，但 benefit/latency relevance 有拐点 |
| posting hotness by bytes vs wait | wait 排名比 bytes 排名更能指导 M1/M2-H |

## 6. M1: Global I/O Broker, Page Cache, In-Flight Coalescing

### Hypothesis

官方 baseline 高并发区间的瓶颈不只是单 query 读放大，还包括跨 query 对相同热 posting/page 的重复读取、SSD 队列拥塞和线程级各自为战。把 page 作为全局资源进行 cache、single-flight 和 admission control，可以降低真实 read wait 与尾延迟，并且不改变索引格式和搜索语义。

### Technical Feasibility

可行性高：

- 不需要重建索引。
- 不需要改变 posting format。
- 可以通过 feature flag 开关。
- 搜索结果应保持完全等价。

主要技术风险：

- page cache key 必须包含 file identity、offset/page ID、page size。
- in-flight coalescing 需要正确管理 buffer 生命周期。
- cache lock contention 不能超过 saved I/O time。
- admission control 不能引入 head-of-line blocking。

### Baseline Compatibility

兼容性高：

- 默认关闭时等价官方 baseline。
- 开启后只是改变 read scheduling 和 cache 命中，不改变 candidate/result 语义。
- 可以优先只覆盖 payload/posting page，不碰 higher-level search logic。

### Expected Gain

- 保守：st8 QPS `+5%`，p95 `-10%`。
- 合理：st8 QPS `+8%~15%`，p95/p99 `-15%~35%`。
- 强信号：st8 QPS `>=6500`，p95 不高于 baseline。

### Success Results

- Recall@10 保持 `>=0.978319`，query-level hash 稳定。
- OS physical reads 或 internal physical page reads 下降 `>=10%`。
- read wait 或 batch read latency 下降 `>=10%`.
- st8 QPS `>=6500`，或 QPS `+5%` 同时 p95/p99 `-20%`。
- st1 回退不超过 `3%`。

### Failure Results

- QPS、read wait、p95/p99 都基本不变，说明缺少可利用复用或 coalescing。
- cache/coalescing 只改善 metadata/code，不改善 payload/posting page。
- st1/st4 回退明显，调度开销超过收益。
- QPS 上升但 read wait/pages/cache metrics 没有解释力。

### Independent Failure Signals

- cache lock wait 超过 saved I/O time。
- in-flight map 项长时间积压或 future/promise 泄漏。
- cache hit ratio 高但 benefit per cached byte 低。
- tail latency 出现新长尾，说明 admission control 或 eviction 引入排队。
- result hash 或 Recall 变化。
- 同一 physical page 被重复 miss，说明 key 规范化或并发状态错误。

### Ablation Expectations

| Ablation | Expected observation |
|---|---|
| Page cache only | 热 page hit 上升，st8 p95 下降，st1 小幅变化 |
| In-flight coalescing only | st8 duplicate physical reads 下降，st1 几乎无变化 |
| Admission/QD limiter only | QPS 未必大涨，但 p95/p99 应下降 |
| Priority scheduling | small/header reads wait 下降，payload 大读不能饥饿 |
| Cache size sweep | hit/benefit 随容量先升后平，出现 benefit-per-byte 拐点 |
| Payload-only cache | 若收益集中在 payload/posting page，则优于 metadata/code-only cache |

## 7. M2-H: Hybrid Selective Code-First Path

### Hypothesis

all-in two-stage 失败的原因是所有 posting 都付 compact scan、merge、read-plan 和随机 payload fetch 成本。官方 legacy 路径虽然有读放大，但连续读和 pipeline 很快。更优策略是只对最亏的大 posting、热 posting、高重复 posting 启用 code-first；普通 posting 继续走官方 legacy。

### Technical Feasibility

中高可行：

- 已有 two-stage build/search 代码可复用。
- 可先用 sidecar compact-code 文件，不破坏 legacy SSD index。
- 可按 posting allowlist 开启 hybrid。

主要技术风险：

- mixed legacy/two-stage result merge 与 dedupe。
- selected posting 的 code cache miss 不能重新成为瓶颈。
- TopR 不可为恢复 recall 放大到接近原 full posting。

### Baseline Compatibility

中高：

- 未命中的 posting 仍走官方 legacy。
- sidecar 缺失或 feature flag 关闭时回退官方路径。
- 需要新增配置指定 hybrid posting selection policy。

### Expected Gain

- 如果 top 10% bad postings 贡献 `>=30%` read wait：st8 QPS `+8%~20%`。
- 如果 bad postings 不集中：收益可能只有 `0%~5%`。

### Success Results

- Recall@10 `>=0.978319`。
- selected postings 的 payload bytes/read wait 下降 `>=25%`。
- total requested/physical bytes 下降 `>=10%`。
- st8 QPS `>=6500`，且 p95 不劣化。
- hybrid selected ratio 有明确拐点，不是越多越好。

### Failure Results

- 为恢复 recall，TopR 被迫增大，最终 payload pages 接近 legacy。
- scan/merge/dedupe/read-plan 开销抵消 selected posting I/O 收益。
- hybrid 在 st1 有收益，但 st8 收益消失。
- selected postings 并不是真实 wait 主贡献者。

### Independent Failure Signals

- code physical read 重新变成主瓶颈。
- selected posting 的 payload candidates/page 仍低于 `5~6`。
- miss attribution 显示 recall 损失来自 posting coverage，不是 local TopR。
- mixed path 去重后 result hash 不稳定。
- legacy and two-stage distance scale 不一致，导致 merge ranking 异常。

### Ablation Expectations

| Ablation | Expected observation |
|---|---|
| top 5% largest postings hybrid | bytes 小幅下降，CPU 开销最低 |
| top 10% largest postings hybrid | QPS/bytes 出现较好折中 |
| top 20% largest postings hybrid | bytes 继续下降但 CPU/merge 开销上升 |
| hot posting hybrid | st8/p95 收益应强于 st1 |
| high-duplicate posting hybrid | duplicate VID/page 应下降 |
| TopR 128/256/384 | Recall 上升伴随 payload pages 上升，应找到拐点 |
| hybrid + M1 | read wait 下降应强于单独 hybrid |

## 8. M4: Primary-Secondary Payload Dedupe

### Hypothesis

官方副本机制通过 full payload 多份复制维持 recall，导致空间膨胀和重复 full payload 读取。若 secondary posting 只存 VID、compact code、primary pointer，full payload 只在 primary store 存一份，则可以保留多路路由，同时减少重复 payload storage/read。

### Technical Feasibility

中等：

- 需要新格式或 sidecar primary payload store。
- 需要 VID -> primary location 映射。
- 需要 primary page read batching 和 cache。
- 需要 build/search/hash 校验，防止 pointer 错位导致 exact distance 错误。

### Baseline Compatibility

中等：

- 可作为 experimental sidecar path。
- legacy index 保留 fallback。
- 不能无缝替换旧格式；需要明确 FormatVersion 或 sidecar manifest。

### Expected Gain

- Index space `-20%~40%`，取决于 replica/full payload duplication。
- Matched recall 下 payload bytes `-10%~30%`。
- st8 QPS `+10%~25%`，但依赖 primary payload locality 和 cache。

### Success Results

- Recall@10 `>=0.978319`。
- full payload storage 明显下降，目标 `>=20%`。
- duplicate full payload read 明显下降。
- primary payload physical bytes 下降 `>=15%`。
- 与 M1 cache/coalescing 组合后 st8 QPS `>=6800~7200`。

### Failure Results

- primary fetch 过于随机，空间下降但 read wait 不降。
- secondary compact code 质量不足，导致 primary fetch 数过多。
- primary pointer/code overhead 抵消 secondary 变轻收益。
- build/load/search 一致性成本过高，影响可维护性。

### Independent Failure Signals

- primary candidates/page 低于 legacy 或 two-stage CoHit。
- primary page cache hit 很低。
- duplicate VID rate 低于预期，说明 M4 目标不成立。
- payload bytes 下降但 payload read wait 不下降。
- payload byte hash、exact distance 或 final result hash 不一致。

### Ablation Expectations

| Ablation | Expected observation |
|---|---|
| primary ordered by VID | 实现简单，但 query locality 可能弱 |
| primary ordered by CoHit trace | primary pages 应低于 VID order |
| primary ordered by hotness | st8/p95 应优于 st1 |
| secondary code-only | 空间最低，但 coarse recall 风险最高 |
| secondary code + bound info | 空间略增，但 primary fetch 数应下降 |
| M4 without M1 cache | 空间收益可见，但 QPS 收益可能有限 |
| M4 with M1 cache | read wait/QPS 应明显优于 M4-only |

## 9. M3: Exact-Safe Chunk Pruning

### Hypothesis

如果 chunk bound 足够紧且语义安全，可以跳过部分 code/payload，减少 posting 内无效扫描。但历史 chunk-locality 和 heuristic pruning 已显示 recall 风险，因此 M3 只能在 exact-safe bound 或 truth-pruned attribution 建立后继续。

### Technical Feasibility

中等，但不适合作为近期主线：

- L2 下可以考虑 centroid-radius lower bound。
- 非 L2 metric 不能直接套用 L2 bound。
- chunk 构造质量决定 prune ratio。

### Baseline Compatibility

低到中：

- 需要 chunked format 或 sidecar directory。
- 必须支持 fallback。
- 默认不能开启 heuristic pruning。

### Expected Gain

不确定：

- 若 bound 紧：payload/code pages 可能下降 `10%~20%`。
- 若 bound 松：收益接近 0。
- 若 heuristic：容易以 recall 损失换性能，不可作为默认。

### Success Results

- zero-recall-loss pruning。
- truth-pruned attribution 显示被剪 chunk 不含 truth/topK 必要候选。
- code/payload pages 下降 `>=10%`。
- st8 QPS 提升且 result hash 稳定。

### Failure Results

- prune ratio 上升但 recall 下降。
- 提高 TopR 恢复 recall 后 QPS 下降。
- chunk metadata/code read 成本抵消 pruning。
- chunk radius 普遍过大，prune ratio 很低。

### Independent Failure Signals

- 被 prune 的 chunk 中出现 truth VID。
- query-level result hash 随线程数变化。
- metric 非 L2 时仍启用 L2 bound。
- chunk directory read wait 成为新瓶颈。

### Ablation Expectations

| Ablation | Expected observation |
|---|---|
| exact bound off vs on | on 应降 pages 且 recall/hash 不变 |
| chunk size 32/64/128 | 小 chunk prune 多但 metadata/code overhead 高 |
| spatial chunk vs write-order chunk | spatial chunk radius 应更小，prune 更强 |
| M3 + M1 cache | directory/cache 应降低 metadata overhead |

## 10. Go / No-Go Criteria

### Continue M1 if

- S0 显示跨 query page reuse、hot page、in-flight duplicate 或 queue wait 明显。
- M1 first prototype 在 recall/hash 不变下带来 read wait 或 p95 下降。

### Stop M1 if

- trace 显示 page reuse 很弱且 queue wait 不是主因。
- cache/coalescing prototype 无法解释任何 QPS/p95 改善。

### Continue M2-H if

- top bad postings 对 bytes/wait 贡献集中。
- selected posting code-first 能把 payload read 降下来，并且不需要过大 TopR 恢复 recall。

### Stop M2-H if

- selected postings 不集中。
- TopR recovery 吃掉所有 payload savings。
- mixed-path CPU/merge 成为新主瓶颈。

### Continue M4 if

- duplicate VID/full payload duplication 对空间和 read wait 贡献足够大。
- primary payload locality 可以通过 ordering/cache 保持。

### Stop M4 if

- duplicate payload 占比低。
- primary fetch randomization 导致 read wait 不降反升。

### Continue M3 only if

- exact-safe bound 或 truth-pruned attribution 可建立。
- zero-recall-loss pruning 有明确 page/read wait 收益。

## 11. Immediate Next Steps

1. 实现或补齐 S0 trace-only 诊断，优先在官方 strict baseline `ir64/st1/st8` 上跑。
2. 根据 S0 输出判断：
   - page reuse / in-flight duplicate / queue wait 强：先做 M1。
   - bad postings 读量集中：准备 M2-H allowlist prototype。
   - duplicate VID/full payload 强：准备 M4 sidecar design。
3. 不继续投入 CoHit 权重、chunk-locality、PQ-code sort、payload copy 或 code async。
4. 所有后续代码改动必须先证明：
   - hypothesis 对应的 trace 信号存在；
   - 成功/失败结果可观测；
   - 失败信号独立于“没达到成功线”。

## 12. Execution Update (2026-05-02)

已完成：

- 新增 S0 诊断脚本 [scripts/analyze_spann_s0_signals.py](/home/ray/code/SPTAG/scripts/analyze_spann_s0_signals.py)，支持：
  - 从 `query_io_stats.csv` 提取 latency/bytes/pages/concentration 信号；
  - 可选读取 `payload_trace.csv` 或 `io_request_trace.csv`（若后续可提供）补充 page/posting 热度与复用；
  - 输出 JSON + Markdown 诊断结果。
- 新增批量执行脚本 [scripts/run_s0_baseline_diagnosis.sh](/home/ray/code/SPTAG/scripts/run_s0_baseline_diagnosis.sh)，已对官方 strict sweep 历史结果批量生成诊断。
- 新增汇总脚本 [scripts/summarize_s0_diagnosis.py](/home/ray/code/SPTAG/scripts/summarize_s0_diagnosis.py)，已生成：
  - [summary.tsv](/home/ray/code/SPTAG/results/io_analysis/s0_baseline_20260502/summary.tsv)
  - 18 个 run 的 `_s0_summary.json` 与 `_s0_report.md` 文件。

当前结论（基于已有历史 CSV）：

- 官方 legacy 历史 `query_io_stats.csv` 中缺少 two-stage payload-locality/payload-wait 细粒度口径，S0 工具已自动标注 `N/A` 并避免误判。
- 现有历史数据可用于吞吐/延迟/requested-bytes 的宏观比较，但不足以直接做 M1/M2-H/M4 的 page/posting 级优先级决策。

**Bug 修复 (2026-05-02)**：

- 修复了 `ExtraStaticSearcher.h` 第 793 行的 null pointer dereference bug：
  - 原代码：`if (truth && truth->count(vectorID)) (*found)[curPostingID].insert(vectorID);`
  - 问题：当 `truth` 非空但 `found` 为空时，解引用 `(*found)` 导致 SEGFAULT
  - 修复：添加 `&& found` 检查
- 现在 `ssdserving` 可以正常运行官方 legacy 索引并生成 trace 数据

**S0 诊断结果 (2026-05-02)**：

已完成 S0 trace-only diagnosis，采集了 **官方 legacy baseline** 的 st1/st8 trace 数据：

| Metric | Legacy st1 | Legacy st8 | Interpretation |
|--------|------------|------------|----------------|
| Top10 page-hit share | 28.6% | 28.6% | 热点集中度中等 |
| Top10 posting-hit share | 33.5% | 33.5% | 热点 posting 集中度中等 |
| Cross-query reuse ratio | 92.4% | 92.4% | **跨查询复用极高！** |
| Avg latency (ms) | 1.160 | 1.434 | |
| QPS (approx) | 862 | 697 | |

**Direction Ranking**:

| Direction | Score | Reason |
|-----------|-------|--------|
| M1 (Global I/O broker + page cache) | 2 | cross-query page reuse = 92.4% |
| M2-H (Hybrid selective code-first) | 2 | N/A for legacy format |
| M4 (Primary-secondary dedupe) | 2 | fallback |

**关键发现**：

- **Cross-query page reuse = 92.4%** - 几乎所有 unique pages 被多个 query 访问，M1 应该非常有效
- Legacy QPS (697) 是 Two-stage (227) 的 **3倍**
- **结论**: M1 是最优先方向，应在 legacy 上叠加 global page cache

**重要修正**: 此前错误地在 two-stage 格式上进行了 S0 诊断。two-stage 方向已被证明行不通，正确的诊断对象是官方 legacy baseline。

详细报告见 [s0_official_baseline_completion_report_20260502.md](/home/ray/code/SPTAG/results/io_analysis/s0_official_baseline_completion_report_20260502.md)
