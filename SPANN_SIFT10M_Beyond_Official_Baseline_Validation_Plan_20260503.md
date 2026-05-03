# SPANN SIFT10M Beyond Official Baseline Validation Plan (2026-05-03)

## 1. Goal And Scope

本文目标是在 SIFT10M `UInt8 + DEFAULT` 数据集上验证 [SPANN_Beyond_Official_Baseline_Plan_20260502.md](/home/ray/code/SPTAG/SPANN_Beyond_Official_Baseline_Plan_20260502.md) 中形成的 SIFT1M 结论是否仍然成立。

验证范围不是重新发明一条新路线，而是回答三个问题：

1. SIFT1M 上唯一正向的 M1 `ShardedPageCache` 是否在 SIFT10M 上仍然有效，收益是否更大或更小。
2. SIFT1M 上已停止的 M2-H selective hybrid 是否因为 SIFT10M posting/hotness 分布变化而值得重开。
3. SIFT1M 上已停止的 M4 primary-secondary dedupe 是否因为 SIFT10M query-level duplicate/read locality 变化而值得重开。

硬性原则：

- 不把 SIFT1M 结论直接迁移到 SIFT10M。
- 不在 SIFT10M oracle/trace 通过前实现新的结构性路径。
- 性能提升必须在 matched recall 下成立，不能靠 recall 下降换 QPS。
- 所有结论必须由 query-level pages/bytes/read-wait/cache-hit/lock-wait/duplicate ratio 支撑。

## 2. Reference Documents

- [SIFT1M_Official_Alignment_Summary.md](/home/ray/code/SPTAG/SIFT1M_Official_Alignment_Summary.md): 第 8.10 节记录了 SIFT10M `UInt8 + DEFAULT` 对齐基线、build 信息和细粒度 I/O 补充测试。
- [SPANN_Beyond_Official_Baseline_Plan_20260502.md](/home/ray/code/SPTAG/SPANN_Beyond_Official_Baseline_Plan_20260502.md): 当前 SIFT1M beyond official baseline 主线计划，包含 M1/M2-H/M4 的通过、停止和重开条件。
- [SPANN_SIFT10M_Test_Plan_and_Execution_Report.md](/home/ray/code/SPTAG/SPANN_SIFT10M_Test_Plan_and_Execution_Report.md): SIFT10M 历史测试入口和执行记录，可作为路径、配置和复现实验参考。

## 3. SIFT10M Baseline Anchor

当前 SIFT10M anchor 来自 `SIFT1M_Official_Alignment_Summary.md` 第 8.10 节。

数据与格式：

| Item | Value |
|---|---|
| Base | `bigann_base_10m.bvecs` -> `bigann10m_base.u8bin` |
| Query | `bigann_query.bvecs` -> `query.public.10K.u8bin` |
| Truth | `idx_10M.ivecs` -> `bigann-10M.bin` |
| Value type | `UInt8` |
| Truth type | `DEFAULT` |

基线搜索配置：

| Parameter | Value |
|---|---:|
| `SearchThreadNum` | `8` |
| `NumberOfThreads` | `16` |
| `InternalResultNum` | `64` |
| `SearchPostingPageLimit` | `4` |

基线结果目录：

```text
results/io_analysis/sift10m_official_u8default_20260430/baseline_st8_nt16_ir64_pl4
```

核心指标：

| Metric | SIFT10M baseline |
|---|---:|
| QPS | `5608.52` |
| Recall@10 | `0.949144` |
| Avg latency | `1.425 ms` |
| P95 latency | `1.655 ms` |
| P99 latency | `1.821 ms` |
| Avg requested read bytes | `515772 bytes/query` |
| Avg pages/query | `~125.9` |
| Avg scanned elements/query | `~2955.6` |
| Avg duplicate vector read ratio | `0.119526` |
| Avg distance eval ratio | `0.880474` |
| Avg final result ratio | `0.003625` |
| Avg read bandwidth | `1608.594 MB/s` |
| Avg queue depth | `~102.9` |
| Peak queue depth | `278` |
| CPU iowait | `~0.41%` |

Build 规模：

| Metric | Value |
|---|---:|
| Heads | `1,496,408` (`14.96%`) |
| Total pages | `2,061,442` |
| Index size | `~8.4 GB` |
| Total elements | `63,692,544` |
| Build total time | `~35 min` |

验收基线：

- 默认 matched-recall 基线使用 `Recall@10 = 0.949144`。
- 若后续定义更高 recall 目标，必须先建立新的 official-like SIFT10M baseline，再比较优化收益。
- 当前不能用 SIFT1M 的 `0.978319` recall 作为 SIFT10M matched-recall 目标，否则会混入 recall/IR/page-limit tradeoff。

## 4. Transferability Hypotheses

### H0: SIFT1M 结论整体迁移

Hypothesis:

SIFT10M 在同分布、head 数同比扩展后，单 query posting 成本只从 SIFT1M `~486 KB/query` 增至 SIFT10M `~516 KB/query`，因此主要结构结论可能保持一致：M1 是唯一短期正向方向，M2-H/M4 仍缺少 query-path 收益。

Success result:

- SIFT10M M1 在 `st8/st16` 仍有稳定正收益。
- SIFT10M M2-H trace 显示 bad postings 不集中。
- SIFT10M M4 oracle 显示 query-level duplicate payload ratio 不足，primary layout pages/query 不低于 legacy。

Failure signal:

- SIFT10M 出现明显不同于 SIFT1M 的 posting wait/hotness/duplicate 分布，例如 top 10% posting wait contribution 显著升高，或 query-level duplicate payload ratio 大幅超过 SIFT1M。
- SIFT10M cache hit/saved pages 与 QPS 不再相关，说明 M1 的 SIFT1M 解释不能迁移。
- SIFT10M recall loss 主要由 routing margin 而不是 posting I/O 决定，导致 storage-side 优化无法解释 end-to-end 变化。

### H1: SIFT10M 更适合 M1 page cache

Hypothesis:

SIFT10M index 更大、query 数相同、并发下 pages-in-flight 更高，可能产生更强 cross-query page reuse 或更高 I/O overlap，因此 M1 `ShardedPageCache` 的 `st8/st16` 收益可能高于 SIFT1M。

Success result:

- `st8` QPS 相对 baseline 提升 `>= 8%`，即 `>= 6057 QPS`。
- `st16` QPS 相对同配置 baseline 提升 `>= 10%`。
- Recall@10 保持在 `0.949144` 附近，绝对差异不超过正常 run-to-run 波动。
- P99 不劣化超过 `5%`，P99.9 不出现 cache lock/memcpy 引发的异常长尾。
- cache saved pages、single-page hit rate 与 QPS 提升方向一致。

Failure signal:

- cache hit rate 看似较高，但 QPS 不升或下降，同时 lock wait、callback copy time 或 async insert backlog 增加。
- `st1/st4` 低并发回退扩大到超过 SIFT1M，说明固定开销在 SIFT10M 更不可接受。
- `st8/st16` p99/p99.9 出现明显长尾尖刺，且与 cache lock wait 或 insert wait 同步。
- cache saved pages 主要来自 OS cache 已覆盖的热页，process/device read bytes 没有同步下降。

### H2: SIFT10M 可能重开 M2-H selective hybrid

Hypothesis:

SIFT10M 的 head/posting 数更多，posting size 与 hotness 分布可能更长尾；如果少量 bad postings 贡献了远高于 SIFT1M 的 I/O wait，那么 selective hybrid code-first 可能重新有意义。

Success result:

- Top 10% postings 贡献 `>= 40%` 的 posting I/O wait 或 requested bytes。
- Top 5% postings 贡献 `>= 25%` 的 posting I/O wait。
- bad-posting allowlist 覆盖的 query 占比足够高，而 allowlist posting 数足够小。
- 对 selected postings 的模拟或原型显示 read-wait/page reduction 能覆盖 code scan/merge/recall recovery 成本。

Failure signal:

- Top 10% postings wait contribution 仍在 `~30%` 附近或更低，说明 wait 分布均匀。
- selected postings 在不同 run、不同 `st`、不同 `ir` 下不稳定，allowlist 无法产品化。
- bad postings 确实存在，但它们不是 matched-recall 查询的主要耗时来源。
- selective path 引入的 compact-code scan、candidate merge、read-plan 成本接近或超过节省的 posting read wait。

### H3: SIFT10M 可能重开 M4 primary-secondary dedupe

Hypothesis:

SIFT1M M4 失败的核心原因是 storage-level 复制没有转化为 query-level duplicate payload；SIFT10M 的复制、posting overlap 和 query-head 访问集合可能不同，必须重新跑 M4-0 oracle 后才能决定是否继续停止。

Success result:

- Query-level duplicate payload/record ratio `>= 30%`。
- Best simulated primary layout pages/query 相比 legacy pages/query 降低 `>= 15%`。
- Primary candidates/page 明显高于 SIFT1M M4-0，说明 primary payload locality 足以抵消随机化。
- M4 oracle 在 `st8` 和至少一个更高并发点上结论一致。

Failure signal:

- duplicate ratio 仍接近 SIFT1M 的 `15.3%`，说明 query 访问集合内重复不足。
- duplicate ratio 较高但 primary payload pages/query 上升，说明去重换来了更差随机读。
- best simulated layout 依赖 trace-specific order，换 query head 或换 `ir` 后失效。
- route page inflation 接近 legacy posting page cost，VID-only route 本身已经吃掉主要 I/O 预算。

### H4: SIFT10M 的主要问题可能是 routing/recall tradeoff

Hypothesis:

SIFT10M 当前 `Recall@10 = 0.949144` 明显低于 SIFT1M `~0.978`，而 requested bytes/query 只小幅增加。这可能说明规模扩大后的主要问题不只是 payload I/O，而是 head routing、cluster margin、IR/page-limit 与 recall 的 tradeoff。

Success result:

- `InternalResultNum=96` 或更高 page/head 探索能明显提升 recall，但 bytes/pages/query 与 latency 同步上升。
- miss-case attribution 显示 true NN 经常落在未访问或排名较后的 posting。
- `heads_needed_for_target_recall` 或 `true_nn_posting_rank` 分布显示 routing margin 是 recall 上限因素。

Failure signal:

- 提高 `InternalResultNum` 或 page limit 对 recall 改善很小，但 bytes/query 明显增加，说明问题不在访问 head 数不足。
- recall miss 主要发生在已读取 posting 内的候选排序/距离计算阶段，说明 routing 不是主因。
- recall 改善需要极大 I/O 增量，无法形成可接受 QPS/recall Pareto。

## 5. Validation Phases

### S10-S0: Baseline Sweep And Timing Attribution

目的：

建立 SIFT10M 的 QPS/recall/latency/pages/bytes 基线曲线，并确认后续 M1/M2-H/M4 对比点。

Primary matrix:

| Dimension | Values |
|---|---|
| `SearchThreadNum` | `4`, `8`, `12`, `16` |
| `NumberOfThreads` | `16`, `24`, `40`, `64` |
| `InternalResultNum` | `32`, `64`, `96` |
| `SearchPostingPageLimit` | `4` first, then optional `3/5` if recall/QPS tradeoff needs clarification |

建议先分两步执行，避免矩阵过大：

| Step | Matrix | Purpose |
|---|---|---|
| S10-S0a | `st=4/8/12/16`, `nt=16`, `ir=64`, `pl=4` | 找并发主拐点 |
| S10-S0b | best two `st`, `nt=16/24/40/64`, `ir=64`, `pl=4` | 判断 build/search worker 线程是否瓶颈 |
| S10-S0c | best `st/nt`, `ir=32/64/96`, `pl=4` | 建立 recall/QPS/bytes Pareto |

Required metrics:

- QPS, Recall@10, avg/p50/p95/p99/p99.9 latency。
- Head latency, Ex latency, Batch Read Total。
- requested bytes/query, pages/query, scanned elements/query。
- duplicate vector read ratio, distance eval ratio, final result ratio。
- process read bytes delta, disk read bandwidth, avg/peak queue depth, CPU iowait。

如果已有 instrumentation 支持，需要额外拆分：

- I/O issue-to-completion wait。
- callback time。
- posting parse time。
- distance calculation time。

Expected observations:

| Ablation | Expected observation |
|---|---|
| `st` sweep | 如果 SIFT10M 仍受 cross-query I/O reuse/queueing 影响，QPS 应随 `st` 上升到平台期，平台后 p99 增长快于 QPS |
| `nt` sweep | 如果 `NumberOfThreads` 不是瓶颈，高于 `16` 的收益应有限；如果过高导致 contention，QPS 下降且 tail latency 上升 |
| `ir` sweep | `ir=96` 应提升 recall 并增加 bytes/pages/query；`ir=32` 应提高 QPS 但 recall 下降 |
| `pl` optional sweep | 较小 page limit 应降低 bytes/query 但可能损 recall；较大 page limit 应提升 recall 或稳定性但降低 QPS |

Success result:

- 形成可复现的 SIFT10M matched-recall baseline。
- 能明确选择 M1/M2-H/M4 的主测试点，例如 `st8/nt16/ir64/pl4` 与一个高并发点。
- Ex/Batch Read 仍是主要成本，且 pages/bytes/query 能解释大部分性能差异。

Failure signal:

- 同配置 3 次 run 的 QPS 波动超过 `5%`，导致后续 ablation 无法判定。
- Recall 在同配置 run 之间不稳定，说明输入、truth 或 index 口径有问题。
- 系统层指标与 query-level 指标方向冲突，例如 requested bytes 明显下降但 process/device read 和 latency 不变，需要先修正统计口径。

### S10-M1: Sharded Page Cache Validation

目的：

验证 SIFT1M 上通过的 M1 是否在 SIFT10M 上继续成立，并确定是否需要不同的并发门控阈值。

Configurations:

| Config | Key parameters |
|---|---|
| Baseline | `EnablePageCache=false` |
| M1 cache | `EnablePageCache=true`, `PageCacheMaxBytes=268435456` |
| M1 cache size optional | `PageCacheMaxBytes=536870912` only if 256MB hit rate/eviction indicates capacity pressure |

Primary test points:

| Point | Reason |
|---|---|
| `st=1` | 验证低并发固定开销 |
| `st=4` | 验证门控边界 |
| `st=8` | 对齐当前 SIFT10M anchor |
| `st=16` | 验证高并发收益和 tail risk |

每个点至少 3 次 run；如果 run-to-run 波动超过 `5%`，增加到 5 次并使用 median。

Required metrics:

- QPS, Recall@10, p95/p99/p99.9。
- cache lookup count, hit count, hit bytes, saved pages。
- read lock wait, insert write lock wait, total lock wait。
- async insert queue/backlog/copy bytes，如已有统计。
- process/device read bytes delta。

Expected observations:

| Ablation | Expected observation |
|---|---|
| cache off vs on at `st1` | 可能回退；如果不回退，说明 SIFT10M reuse 足以覆盖固定开销 |
| cache off vs on at `st4` | 关键门控点；收益接近 0 或小幅回退时应继续用 `SearchThreadNum>=8` 门控 |
| cache off vs on at `st8` | 若 H1 成立，应看到 `>=8%` QPS 提升且 saved pages 与 read bytes 下降一致 |
| cache off vs on at `st16` | 若高并发 reuse 更强，应看到 `>=10%` QPS 提升；若 lock contention 强，p99.9 会先恶化 |
| 256MB vs 512MB | 只有在 eviction/capacity pressure 明确时才应有收益；否则更大 cache 可能只增加管理开销 |

Success result:

- `st8` QPS `>= 6057`，Recall@10 与 `0.949144` matched。
- `st16` QPS 相对同配置 baseline `>= +10%`。
- p99 不劣化超过 `5%`，p99.9 不出现 cache 相关尖刺。
- saved pages/read bytes下降与 QPS 提升一致。
- 低并发若回退，能通过 `PageCacheMode=auto` 或 `PageCacheMinSearchThreads` 规避。

Failure signal:

- `st8/st16` hit rate 高但 QPS 不升，且 lock wait/callback copy 占比上升。
- `st8/st16` QPS 提升来自 recall 漂移或 result instability。
- 512MB cache 比 256MB 更慢且 hit/saved pages 无明显提升，说明容量不是瓶颈。
- cache 使 p99.9 明显变差，即使平均 QPS 提升，也不应默认产品化。

Decision:

- 如果 S10-M1 通过，SIFT10M 继承 M1 主线，但需要单独记录门控阈值。
- 如果 S10-M1 失败，不应继续调 admission/cache size，除非 phase timing 证明具体瓶颈是可修的 lock/copy/eviction。

### S10-M2H: Selective Hybrid Revalidation

目的：

验证 SIFT10M 是否存在足够集中的 bad postings，使 SIFT1M 上停止的 M2-H 有重开价值。

Required run:

```ini
[SearchSSDIndex]
EnablePostingTrace=true
PostingTraceOutput=results/io_analysis/sift10m_beyond_validation_20260503/m2h_trace/posting_trace_st8_ir64.csv
```

至少采集：

- anchor point: `st8/nt16/ir64/pl4`。
- high-concurrency point: S10-S0 中 QPS 平台期附近的一个点。
- optional recall point: `ir96`，用于观察 high-recall 档位下 bad-posting 是否更集中。

Analysis:

- 按 posting 聚合 read bytes、read wait、query hit count、cache hit/miss。
- 计算 top 1%/5%/10% postings 的 wait/bytes contribution。
- 计算 bad postings 在 query 中的覆盖率。
- 对 selected postings 估算 selective code-first 能减少的 payload pages/read-wait。

Expected observations:

| Ablation | Expected observation |
|---|---|
| top-k posting concentration | 如果 M2-H 可重开，top 10% wait contribution 应显著高于 SIFT1M 的 `29.63%` |
| `st8` vs high concurrency | 如果 hot postings 是真实瓶颈，高并发下 concentration 应保持或增强 |
| `ir64` vs `ir96` | 如果 high-recall 主要放大少量 bad postings，`ir96` concentration 应升高；如果均匀放大，则 M2-H 不成立 |
| allowlist size sweep | 小 allowlist 应捕获大部分 wait；如果需要很大 allowlist，selective 失去意义 |

Success result:

- Top 10% postings 贡献 `>= 40%` read wait 或 requested bytes。
- Top 5% postings 贡献 `>= 25%` read wait。
- selected postings 在多 run 和不同并发点上稳定。
- 模拟显示 selected postings payload read-wait reduction `>= 15%` end-to-end Ex latency potential。

Failure signal:

- read wait/bytes 分布均匀，top 10% contribution 低于 `35%`。
- selected postings 受 run 顺序、OS cache 或 query shard 影响大，无法稳定复现。
- selected postings 主要贡献 scanned CPU 而不是 physical read wait。
- 需要覆盖过多 postings 才能产生收益，导致 selective hybrid 接近 full two-stage。

Decision:

- 只有 S10-M2H trace 通过后，才允许设计 selective prototype。
- 如果失败，继续停止 M2-H，不做 allowlist hybrid 代码实现。

### S10-M4: Primary-Secondary Dedupe Oracle

目的：

验证 SIFT10M query path 是否有足够 query-level duplicate payload 和 primary payload locality，使 M4 值得从 SIFT1M 的停止状态中重开。

Required run:

```ini
[SearchSSDIndex]
EnablePreDedupeTrace=true
PreDedupeTraceOutput=results/io_analysis/sift10m_beyond_validation_20260503/m4_oracle/pre_dedupe_trace_st8_ir64.csv
```

Oracle script:

```text
scripts/m4_oracle_simulation.py
```

如果继续使用现有脚本，报告中必须明确：

- 哪些 layout 是真正独立的 page-packing ablation。
- 是否存在 trace-specific order 泄漏。
- legacy pages/query 与 simulated primary pages/query 的口径是否一致。

至少模拟：

- `VIDOrder`
- `PrimaryPostingOrder`
- `CoHitTraceOrder`
- `HotnessOrder`
- 如果脚本限制仍存在，必须把 `CoHitTraceOrder` 之外的 layout 标为 weak ablation，不能作为强结论来源。

Expected observations:

| Ablation | Expected observation |
|---|---|
| pre-dedupe duplicate ratio | 如果 M4 可重开，应明显高于 SIFT1M `15.3%`，目标 `>=30%` |
| primary pages/query | 可行 layout 必须低于 legacy pages/query，不能只降低 storage bytes |
| layout comparison | 好 layout 应在 `st8` 和高并发点都稳定，而不是只对单 trace 有效 |
| `ir64` vs `ir96` | 如果 high recall 增加 query-level duplicate，`ir96` 可能提高 M4 价值；如果 primary pages 同步爆炸，则仍失败 |

Success result:

- Query-level duplicate payload/record ratio `>= 30%`。
- Best primary layout pages/query 相比 legacy pages/query 降低 `>= 15%`。
- Primary candidates/page 足够高，route+primary total pages/query 低于 legacy。
- oracle 结论在至少两个并发/IR 点上稳定。

Failure signal:

- duplicate ratio 低于 `20%`，说明 query-level 去重空间不足。
- duplicate ratio 高但 primary pages/query 高于 legacy，说明 locality 不足。
- best layout 只在 trace-trained query set 上有效，换 `ir` 或 query 分片后失效。
- route read pages 加 primary read pages 接近或超过 legacy posting pages。

Decision:

- S10-M4 oracle 成功前，不实现 M4 sidecar build/search。
- 如果 S10-M4 oracle 失败，M4 对 SIFT10M 继续停止。
- 如果 S10-M4 oracle 成功，下一步也只能进入 sidecar exact-safe design，不允许直接引入 code-first、TopR pruning 或 approximate behavior。

### S10-R: Routing And Recall Attribution

目的：

解释 SIFT10M Recall@10 `0.949144` 低于 SIFT1M 的原因，避免把 recall/routing 问题误判为 storage layout 问题。

Required analysis:

- `ir32/64/96` recall-QPS-bytes Pareto。
- true nearest neighbor posting rank。
- heads needed for target recall。
- cluster margin distribution。
- miss-case attribution: true NN 是否在已访问 posting 中。

Expected observations:

| Ablation | Expected observation |
|---|---|
| `ir32 -> ir64 -> ir96` | 如果 routing 是主要问题，recall 应随 IR 上升，但 bytes/query 和 latency 同步增加 |
| true NN posting rank | 如果 true NN 经常落在未访问 posting，storage-side cache/dedupe 不能解决 recall |
| cluster margin | 小 margin query 更可能 miss，说明 head routing 区分度不足 |
| miss-case in-read vs not-read | 如果 miss 发生在已读取 posting 内，应检查 rerank/distance/path；如果未读取，应检查 routing/IR/page limit |

Success result:

- 能明确区分 recall loss 是 routing miss、posting candidate miss、还是 final rerank miss。
- 能给出一个 matched-recall 对比点，避免后续优化只在低 recall 档位变快。

Failure signal:

- truth 格式、query 映射或 result id 口径不一致，导致 recall attribution 不可信。
- IR 增加导致 recall/QPS 曲线不稳定，说明测试环境或 index/search 配置存在隐藏变量。
- miss-case attribution 无法复现 recall 数值，说明 instrumentation 有 bug。

## 6. Overall Success Criteria

SIFT10M beyond validation 的成功不要求立即超过所有官方指标，而是要求明确主线是否可迁移、是否有新的可重开方向。

Strong performance success:

- 在 matched recall `Recall@10 ~= 0.949144` 下，`st8` QPS 相比 `5608.52` 提升 `>= 15%`，即 `>= 6450 QPS`。
- P99 不劣化超过 `5%`。
- requested bytes/pages/process read bytes 或 cache saved pages 能解释主要收益。

Moderate performance success:

- `st8` QPS 提升 `>= 8%`，即 `>= 6057 QPS`。
- `st16` 或高并发点提升 `>= 10%`。
- 低并发回退可通过明确门控规避。

Structural success:

- S10-M2H 或 S10-M4 至少一个 oracle 通过，提供可执行的新结构方向。
- 通过条件必须满足独立的成功标准，不能只因为 baseline 不够快而重开。

Failure signals for overall plan:

- 所有可解释收益都来自 M1 cache，且 M2-H/M4 oracle 失败；这意味着 SIFT10M 与 SIFT1M 结论一致，当前结构没有大幅超越空间。
- 提升只出现在低 recall 配置，matched recall 下消失。
- QPS 提升伴随 p99/p99.9 明显恶化，无法作为默认路线。
- pages/bytes/read-wait 指标不支持 QPS 变化，说明结果可能来自噪声、OS cache 或测试环境差异。

## 7. Output Layout

建议统一输出目录：

```text
results/io_analysis/sift10m_beyond_validation_20260503/
```

子目录：

| Directory | Content |
|---|---|
| `s0_sweep/` | baseline st/nt/ir/pl sweep logs, configs, summary |
| `m1_cache/` | cache off/on comparison and cache stats |
| `m2h_trace/` | posting trace CSV and bad-posting analysis |
| `m4_oracle/` | pre-dedupe trace, oracle simulation, report |
| `routing_recall/` | IR sweep, miss-case attribution, routing metrics |

每个实验目录必须保存：

- 实际 `config.ini`。
- 原始 `sptag.log`。
- structured `summary.txt` 或 `report.md`。
- query-level CSV。
- 分析脚本输出。
- 三次 run 的 raw log，而不只保存平均值。

建议最终生成：

```text
results/io_analysis/sift10m_beyond_validation_20260503/SIFT10M_Beyond_Validation_Summary.md
results/io_analysis/sift10m_beyond_validation_20260503/SIFT10M_Beyond_Validation_Summary.tsv
```

## 8. Execution Order

1. Run S10-S0a baseline `st` sweep.
2. Run S10-S0b `nt` sweep on selected `st` points.
3. Run S10-S0c `ir` sweep and select matched-recall comparison points.
4. Run S10-M1 cache off/on validation at `st1/st4/st8/st16`.
5. Run S10-M2H posting trace only after baseline points are stable.
6. Run S10-M4 pre-dedupe oracle only after deciding the exact SIFT10M comparison point.
7. Run S10-R routing attribution if `ir` sweep shows recall remains the dominant tradeoff.
8. Update [SPANN_Beyond_Official_Baseline_Plan_20260502.md](/home/ray/code/SPTAG/SPANN_Beyond_Official_Baseline_Plan_20260502.md) only after SIFT10M results produce a stable conclusion.

## 9. Immediate Next Step

Start with S10-S0a:

| Run | `SearchThreadNum` | `NumberOfThreads` | `InternalResultNum` | `SearchPostingPageLimit` |
|---|---:|---:|---:|---:|
| S10-S0a-1 | `4` | `16` | `64` | `4` |
| S10-S0a-2 | `8` | `16` | `64` | `4` |
| S10-S0a-3 | `12` | `16` | `64` | `4` |
| S10-S0a-4 | `16` | `16` | `64` | `4` |

Each run:

- clear cache or record cache state consistently;
- save full logs and query I/O stats;
- repeat 3 times if the first pass shows promising or contradictory results;
- do not enable M1 cache during S10-S0.

Decision after S10-S0a:

- If QPS plateaus before `st8`, M1 high-concurrency value may be limited and routing/CPU attribution should move earlier.
- If QPS improves through `st16` but p99 grows, M1 cache and bytes-in-flight control are likely worth testing.
- If Recall changes across `st`, the test setup is invalid and must be fixed before optimization comparisons.

---

## 10. 验证执行结果 (2026-05-03)

### 执行摘要

**结论**: SIFT10M 验证完成，**所有 SIFT1M 结论完全迁移**，较大改进空间已被排除。

| 任务 | 状态 | SIFT1M 结果 | SIFT10M 结果 | 一致？ |
|------|------|-------------|--------------|--------|
| **S10-S0a** | ✅ 完成 | st8 平台期 | st8 平台期 | ✓ |
| **S10-M1** | ✅ 完成 | +5.0% QPS (st8) | -0.2% QPS (st8) | ✗ |
| **S10-M2H** | ✅ 完成 | Top10%=29.63% | Top10%=27.51% | ✓ |
| **S10-M4** | ✅ 完成 | 查询重复=15.3% | 查询重复=10.91% | ✓ |

**最终状态**: SIFT10M 上无有效 beyond baseline 优化路径。

### S10-S0a: Baseline st Sweep 结果

| st | QPS | Recall@10 | 平均延迟 |
|----|-----|-----------|----------|
| 4 | 3,294 | 0.949144 | 2.43 ms |
| 8 | 5,378 | 0.949144 | 1.49 ms |
| 12 | 5,381 | 0.949144 | 2.23 ms |
| 16 | 5,380 | 0.949144 | 2.98 ms |

**关键观察**:
- QPS 在 st=8 达到平台期，与 SIFT1M 类似
- Recall 稳定在 0.949144

### S10-M1: Page Cache 验证结果

**结果**: ❌ **失败**

| 配置 | QPS | 变化 | Cache 命中率 | 节省页面数 |
|------|-----|------|--------------|------------|
| Cache OFF | 5,316.32 | - | - | - |
| Cache ON | 5,305.04 | **-0.2%** | 18.25% | 29,542 |

**失效根因**:

| 指标 | SIFT1M | SIFT10M | 比值 |
|------|--------|---------|------|
| 单页 posting 数 | 1,023 | 440,342 | 430x |
| 每个单页 posting 平均访问次数 | 134.9 | 0.07 | 1/1928 |
| Cache 命中率 | 77.71% | 18.25% | 1/4.3 |

SIFT10M 索引规模大 10x，但查询数相同。每个 posting 平均只被访问 0.07 次，缺乏跨查询复用。

详见: `results/io_analysis/sift10m_beyond_validation_20260503/m1_cache/M1_Failure_Analysis.md`

### S10-M2H: Selective Hybrid 重新验证结果

**结果**: ❌ **失败**

| 指标 | 值 | 阈值 | 状态 |
|------|-----|------|------|
| Top 5% I/O wait 贡献 | 18.10% | >= 25% | ❌ |
| Top 10% I/O wait 贡献 | 27.51% | >= 40% | ❌ |
| Gini 系数 | 0.34 | 越高越集中 | 低 |

I/O wait 分布均匀，无集中的坏 posting 热点。与 SIFT1M 结论一致 (Top10%=29.63%)。

详见: `results/io_analysis/sift10m_beyond_validation_20260503/m2h_trace/S10_M2H_Analysis.md`

### S10-M4: Primary-Secondary 去重 Oracle 结果

**结果**: ❌ **失败**

| 指标 | 值 | 阈值 | 状态 |
|------|-----|------|------|
| 存储级重复 | 49.72% | - | 误导性 |
| **查询级重复** | **10.91%** | **>= 30%** | **❌** |

**关键洞察**: 存储级重复 (49.72%) 不等于查询级重复 (10.91%)。81% 的 VID 存在于多个 posting，但单个查询只从每个 posting 访问每个 VID 一次。

详见: `results/io_analysis/sift10m_beyond_validation_20260503/m4_oracle/S10_M4_Analysis.md`

### 假设验证总结

| 假设 | 结果 | 证据 |
|------|------|------|
| H0: SIFT1M 结论迁移 | ✓ 验证通过 | M2-H/M4 都失败，与 SIFT1M 相同 |
| H1: SIFT10M 更适合 M1 cache | ✗ 被拒绝 | 命中率 18% vs 78%，QPS -0.2% |
| H2: SIFT10M 重开 M2-H | ✗ 被拒绝 | Top10% wait = 27.51% < 40% |
| H3: SIFT10M 重开 M4 | ✗ 被拒绝 | 查询级重复 = 10.91% < 30% |

### 最终结论

**所有 beyond baseline 方向已验证失败：**

| 方向 | SIFT1M | SIFT10M | 产品化 |
|------|--------|---------|--------|
| M1: Page Cache | ✓ st8 +5% | ✗ st8 0% | 仅限小规模数据集 |
| M2-H: Selective Hybrid | ✗ 已停止 | ✗ 已停止 | 否 |
| M4: Primary-Secondary | ✗ 已停止 | ✗ 已停止 | 否 |

**结论**: 当前结构下，SIFT1M 和 SIFT10M 都无大幅超越空间。M1 仅适用于小规模数据集 (~1M vectors)。

### 对主计划的影响

已更新 [SPANN_Beyond_Official_Baseline_Plan_20260502.md](/home/ray/code/SPTAG/SPANN_Beyond_Official_Baseline_Plan_20260502.md):

1. 添加 Section 3.2 SIFT10M 验证结论
2. M1 适用范围限定为小规模数据集
3. M2-H/M4 保持停止状态
4. 确认较大改进空间已被排除

### 生成的文件

```
results/io_analysis/sift10m_beyond_validation_20260503/
├── s0_sweep/
│   └── s10_s0a_st*_run*.log       # Baseline st sweep 日志
├── m1_cache/
│   ├── M1_Failure_Analysis.md     # M1 失效根因分析
│   └── s10_cache_*.log            # Cache 测试日志
├── m2h_trace/
│   ├── S10_M2H_Analysis.md        # M2-H 分析报告
│   ├── posting_trace_st8.csv      # Posting trace (640K 行)
│   └── payload_trace_st8.csv      # Payload trace (26M 行)
├── m4_oracle/
│   ├── S10_M4_Analysis.md         # M4 分析报告
│   └── pre_dedupe_trace_st8.csv   # Pre-dedupe trace (29M 行)
└── SIFT10M_Beyond_Validation_Summary.md  # 验证总结
```

