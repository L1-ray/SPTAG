# P0/P1 优化记忆：UInt8 + DEFAULT 格式

## 数据格式约束

**必须使用 strict `UInt8 + DEFAULT` 格式**，与官方 AE 配置对齐：

- `ValueType=UInt8`
- `VectorType=DEFAULT`
- `QueryType=DEFAULT`
- `TruthType=DEFAULT`

### 数据文件

- Base vectors: `/home/ray/data/sift1m/bigann1m_base.u8bin` (1M x 128 UInt8)
- Query vectors: `/home/ray/data/sift1m/query.public.10K.u8bin` (10K x 128 UInt8)
- Ground truth: `/home/ray/data/sift1m/bigann-1M.bin` (10K x 100)

### 索引目录

- Legacy baseline: `/home/ray/data/sift1m/spann_index_official_u8default_20260430`
- Two-stage 索引需要重新构建（使用 UInt8 数据）

## Legacy Baseline 性能

`SearchThreadNum=8, NumberOfThreads=16, InternalResultNum=64, SearchPostingPageLimit=4`:

- QPS ≈ 5945
- Recall@10 ≈ 0.978
- avg latency ≈ 1.34 ms
- requested bytes ≈ 486 KB/query

## P0: 并发 Recall 复核

### 目标

验证历史 `SearchThreadNum=8` recall 下降现象是否仍存在

### 测试配置

```
ValueType=UInt8
VectorType=DEFAULT
PostingTopRPerPosting=64
PostingTopRGlobal=256
EnableChunkedPosting=false
PostingChunkPruneMode=None
InternalResultNum=64
SearchPostingPageLimit=4
IndexDirectory=/home/ray/data/sift1m/spann_index_official_u8default_20260430
```

### 测试矩阵

SearchThreadNum = 1, 2, 4, 8

### 测试结果 (2026-05-01)

| SearchThreadNum | QPS | Recall@10 |
|---|---|---|
| 1 | 951 | 0.978319 |
| 2 | 1946 | 0.978319 |
| 4 | 3764 | 0.978319 |
| 8 | 5889 | 0.978319 |

### 验收结论

**历史并发 recall 下降现象已不存在！**
- st=1 到 st=8 的 Recall@10 均稳定在 0.978319
- QPS 随并发线性增长，st=8 时达到 ~5.9k QPS
- 并发扩展性良好，无需额外优化

## P1: Two-stage 性能瓶颈校准

### 当前问题

- Two-stage 在 matched recall 下 payload KB 与官方 legacy baseline 接近，但 QPS 仍显著落后。
- 当前主瓶颈已定位到 Payload Read Wait：`Fetch Payload & Rerank` 占 two-stage phase time 的 `73.8%`，其中 `Payload Read Wait` 占 `89.0%`，折合 two-stage 总 phase time 的 `66.8%`；scan/merge/plan CPU overhead 只解释 legacy/two-stage ex-latency gap 的 `27.4%`。
- `PostingTopRGlobal` 能换回 Recall，但会同步增加 payload pages、payload KB 和 latency。
- `PostingTopRPerPosting` 在当前范围不是瓶颈，继续调高没有 ROI。

### 优化方向

1. 以官方 strict `UInt8 + DEFAULT` baseline 作为短期性能主线。
2. 不再优化 payload copy、exact distance 或 result insertion，P2 已证明它们不是主成本。
3. P3 已证明 query co-hit / theoretical best packing 有 `40%~50%` page reduction 上限。
4. P4 same-head CoHit v1 已证明 build-side reorder 在 256/512 有实际收益。
5. P4b page-aware CoHit v2 已完成：256/512 弱正向，`st=8` Recall/pages 稳定，但 768 仍只有 `2.99%` page reduction。
6. P4c query-set/page-objective CoHit 已完成：768 提升到 `14.92%`，但 512 回退到 `15.34%`。
7. P4d layout 策略整合已完成：真实权重 sweep 证明不存在满足 `512>=18%` 且 `768>=10%` 的统一 layout。
8. 当前下一步是产品化决策：默认 TopR=512 选 v1/v2，多 TopR 支持选 v3，极致性能才考虑参数化多索引。
8. 不继续盲扫 chunk size、PQ-code sort、payload copy、exact distance 或 code async。

## 注意事项

- 所有 benchmark 必须使用 UInt8 + DEFAULT
- 不再使用 Float + XVEC 格式
- 每轮测试需要记录 raw log 来源

## P1: Two-Stage Posting 初步测试结果 (2026-05-02)

### 索引构建

成功构建 Two-stage posting 索引：
- IndexDirectory: `/home/ray/data/sift1m/spann_index_m2_u8default`
- Meta: FormatVersion=2, LayoutType=twostage_v1, CodeType=PQ

### 关键发现

**命令行参数格式问题**：
- `ssdserving` 不支持 `-c` 选项，直接传递配置文件路径
- 错误: `./Release/ssdserving -c config.ini` (会把 `-c` 当作文件名)
- 正确: `./Release/ssdserving config.ini`

**INI 注释格式问题**：
- IniReader 只支持 `;` 开头的注释，不支持 `#`
- 使用 `#` 注释会导致 `ReadIni_FailedParseParam` 错误

### 初步测试结果 (st=1)

| 指标 | Two-stage P1 | Legacy P0 |
|------|-------------|-----------|
| QPS | 344 | 951 |
| Recall@10 | 0.962 | 0.978 |
| Payload Physical Bytes | 389 KB/query | 388 KB/query |

### 观察

1. Recall 比 legacy 低约 1.6%，需要分析原因
2. QPS 降低可能与 two-stage 的额外解码开销有关
3. Payload 物理页读取量相近，说明 payload inflation 问题仍存在

### 当时下一步（已完成）

1. 已通过 P2 attribution 和 TopR sweep 分析 Recall 下降原因。
2. 已确认 `PostingTopRGlobal` 能提高 Recall，但会增加 payload pages/KB，不是性能优化。
3. same-head 对照仍是补证项，但当前 matched-recall 结果已经足以说明 two-stage 现有 layout 无性能优势。

### 参数调优结果 (PostingTopRGlobal)

| TopRGlobal | QPS (st=1) | Recall@10 |
|------------|------------|-----------|
| 256 | 344 | 0.962 |
| 512 | 288 | 0.977 |
| 768 | 266 | 0.980 |

### 并发测试结果 (TopRGlobal=768)

| SearchThreadNum | QPS | Recall@10 |
|-----------------|-----|-----------|
| 1 | 266 | 0.980 |
| 8 | 1850 | 0.980 |

### 与 Legacy Baseline 对比

| 配置 | QPS (st=8) | Recall@10 | Payload Physical Bytes |
|------|------------|-----------|------------------------|
| Legacy (official) | 5889 | 0.978 | 388 KB/query |
| Two-stage (TopRGlobal=768) | 1850 | 0.980 | ~389 KB/query |

### 发现

1. **Recall 可以通过 TopRGlobal 调优** - 增加 TopRGlobal 可以提高 Recall，但会降低 QPS
2. **Two-stage QPS 较低** - 可能由于额外的 code 解码和 rerank 开销
3. **Payload 物理页读取量相似** - 约 389 KB/query，说明 payload inflation 问题仍未解决

## P1.1 Locality Instrumentation 结果 (2026-05-02)

### 新增指标

在 `SearchStats` 和 CSV 输出中添加了以下 payload locality 指标：
- `unique_payload_pages`: 实际读取的唯一物理页数
- `payload_candidates`: 需要读取 payload 的候选数
- `candidates_per_payload_page`: 平均每页覆盖的候选数（核心指标）
- `postings_with_payload`: 涉及的 posting 数量
- `total_payload_page_spans`: 不去重时的页跨度总数

### 测试结果

| TopRGlobal | Unique Pages (avg) | Candidates (fixed) | avg(Candidates/Page) | Postings | Page Spans | Recall |
|------------|-------------------|-------------------|----------------------|----------|------------|--------|
| 256 | 95 | 256 | **2.75** | 55 | 260 | 0.962 |
| 768 | 133 | 768 | **5.94** | 63 | 780 | 0.980 |

**注意**：
- `Candidates` 是固定值（等于 `PostingTopRGlobal`），每个 query 都相同
- `Unique Pages` 是所有 query 的均值
- `avg(Candidates/Page)` 是先计算每个 query 的 `Candidates ÷ Unique Pages`，再取均值
- 因此 `avg(Candidates/Page) ≠ Candidates ÷ avg(Unique Pages)`

### 关键发现

1. **Candidates Per Payload Page 极低**：仅 2.75 ~ 5.94
   - 如果 payload locality 好，应该接近每页的候选容量（可能 32-64）
   - 这验证了 P1.1 的 hypothesis：候选在 payload layout 上过于分散

2. **页去重效果有限**：
   - TopRGlobal=768 时，Page Spans=780，但 Unique Pages=133
   - 说明有一定的页复用，但仍需读取大量物理页

3. **增加 TopRGlobal 提升有限**：
   - TopRGlobal 从 256→768，Candidates/Page 从 2.75→5.94
   - 提升主要来自候选在同一页上的概率增加

### 结论

**Payload physical page inflation 的根因已确认**：
- 候选在 payload layout 上高度分散
- 平均每页只能服务 2-6 个候选
- P3 已证明 theoretical best packing 上限足够，P4 same-head CoHit v1 已证明 build-side `cohit_layout` 在 256/512 有效。

### 当时下一步（已完成）

已完成 P1.2 matched-recall 对照实验。注意：该实验仍是非严格 same-head。

## P1.2 Matched-Recall 对照结果 (2026-05-02)

### 实验设计

对比 legacy 索引和 two-stage 索引在 matched recall 下的性能。

**Head 一致性说明**：
- Legacy 使用 `spann_index_official_u8default_20260430`
- Two-stage 使用 `spann_index_m2_u8default`
- 两个索引目录不同，head build 是否一致尚未验证
- 结论应理解为"matched-recall 对照"而非"same-head matched-recall 对照"

### 测试结果

| 配置 | QPS (st=1) | QPS (st=8) | Recall@10 | Physical Bytes | Pages Read |
|------|------------|------------|-----------|----------------|------------|
| Legacy (official index) | 943 | **5770** | 0.978 | 486 KB | 119 |
| Two-stage (TopRGlobal=512) | 285 | **1966** | 0.977 | 500 KB | 122 |

**注意**：两个索引目录不同，head build 是否一致未验证。

### 关键发现

1. **Legacy QPS 是 Two-stage 的 3 倍**：5770 vs 1966
2. **物理读取量几乎相同**：486 KB vs 500 KB，119 页 vs 122 页
3. **Two-stage 额外开销来自**：
   - Code 解码
   - Coarse candidate 合并/去重
   - Payload read plan 构建

### 结论

**当前 two-stage 实现在 payload 物理读取没有减少的情况下，反而因为额外处理降低了 QPS。**

要使 two-stage 有性能优势，必须：
1. 减少 payload 物理页读取（通过 build-side reorder）
2. 或者减少 coarse 处理开销

### 当时下一步（已完成）

P1.3a posting-order reorder (PQ code sort) 已完成并失败，详见评审文档。

## P1.3a posting-order reorder (PQ code sort) 结果 (2026-05-02)

### 实验设计

在 posting list 构建时，按 PQ code 排序记录顺序，期望同一 posting 内的候选更可能落在相邻页面。

### 测试结果

| 配置 | Unique Pages (avg) | Candidates | avg(Candidates/Page) | Recall@10 |
|------|-------------------|------------|----------------------|-----------|
| Original Two-stage | 122 | 512 | 4.20 | 0.977 |
| Reordered Two-stage (PQ code sort) | 123 | 512 | 4.27 | 0.976 |

### 关键发现

1. **Candidates/Page 仅提升 2.4%**：4.20 → 4.27
2. **Unique Pages 基本不变**：122 → 123
3. **Recall 基本不变**：0.977 → 0.976

### 失败原因

PQ code 是 16 字节的量化码，按 PQ code 字典序排序虽然会改变 posting blob 内 payload 顺序，但这个排序信号与 query topR 候选的 co-hit locality 不匹配，因此几乎没有减少 unique payload pages。

### 后续方向

P1.3b 应改为 **posting-aware payload physical layout**：two-stage payload 已经写在 posting blob 内，P1.3a 失败说明简单按 PQ code 排序不足，不说明 payload 不能重排。

**当前校准**：P2.1/P2.2、P3、P4 v1、P4b v2 与 P4c v3 均已完成。P3 证明 `40%~50%` theoretical page reduction 上限；P4/P4b/P4c 证明 build-side reorder 有效，但不同 TopR 档位存在 locality tradeoff。

补充校准：P4c v3 已把 768 从 `≈3%` 提升到 `14.92%`，但 512 从 v2 的 `19.29%` 回退到 `15.34%`。P4d 真实权重 sweep 已证明该 tradeoff 不能靠 `w=0.75~1.5` 调和；当前下一步是产品化选择，而不是继续调 CoHit 权重。

## 当前状态总结 (2026-05-02)

### 已完成

- P0：官方 strict baseline 并发 recall 稳定性已复核，`st=1..8` Recall 稳定。参数产品化已完成。
- P1：Per-phase cost attribution 已完成，确认 Fetch Payload & Rerank 占 73.8% phase time。
- P1.1：Locality instrumentation 已完成，确认 two-stage candidates/page 很低。
- P1.2：Matched-recall 对照已完成，当前 two-stage 在现有 layout 下无性能优势，same-head 仍待补证。
- P1.3a：PQ code sort 已失败，不能改善 payload pages。
- P1.3b：chunk-locality 原型已失败，`Unique Pages` / `Payload KB` 没有下降。
- P2.1/P2.2：coarse/miss-case attribution 和 TopR sweep 已闭合，`truth_attribution_closure_error=0`。
- P2：Fetch Payload & Rerank 子阶段拆分已完成，确认 Payload Read Wait 占 89% phase time。
- **P3：Query co-hit trace 与 theoretical best packing 已完成，oracle page reduction 达 40%~50%。**
- **P4：CoHit same-head st=1 原型已部分成功，TopR 256/512 actual pages 降 15%~19%，QPS 升 18%~19%；TopR 768 收益弱，仅降 2.92% pages。**
- **P4b：Page-aware CoHit v2 已完成首轮验证，256/512 略优于 v1，但 768 仍只降 2.99%，未达 10%+ 主目标。**
- **P4c：Query-set CoHit v3 已完成，768 page reduction 提升到 14.92%，但 512 回退到 15.34%。**
- **P4d：Layout 策略整合已完成，权重 sweep 证明 tradeoff 不可调和，不存在满足 512>=18% 且 768>=10% 的统一 layout。**

### 最新瓶颈判断

P2 子阶段拆分进一步确认瓶颈层级：

```text
Payload Read Wait 占 89.0% of Fetch Payload & Rerank
+ Fetch Payload & Rerank 占 73.8% of Two-Stage phase time
= Payload Read Wait 占 66.8% of Two-Stage 总 phase time

结论：payload page I/O wait 是绝对主要瓶颈
```

P3 理论最优 packing 验证：

```text
TopRGlobal=256: Oracle page reduction = 41.6%
TopRGlobal=512: Oracle page reduction = 48.1%
TopRGlobal=768: Oracle page reduction = 49.7%
Train/Held-out 方向一致，误差 < 1%

结论：理论上限足够大，应进入 co-hit layout 实现
```

P4 原型代码状态：

- `PostingPayloadLayout=CoHit`：build-side co-hit payload layout 开关。
- `PostingCohitOrderFile`：由 P3 trace 离线生成的 `(posting_id, vector_id, order_rank)` 输入。
- [scripts/build_payload_cohit_order.py](/home/ray/code/SPTAG/scripts/build_payload_cohit_order.py)：从 payload trace 生成 deterministic co-hit order。
- [configs/p4_cohit_build.ini](/home/ray/code/SPTAG/configs/p4_cohit_build.ini)：CoHit build 配置。
- [configs/p4_cohit_st1.ini](/home/ray/code/SPTAG/configs/p4_cohit_st1.ini)：CoHit search/eval 配置。
- [configs/p4_cohit_build_samehead.ini](/home/ray/code/SPTAG/configs/p4_cohit_build_samehead.ini)：same-head CoHit build 配置，避免 P3 trace/order 与重新生成 head 后的 postingID 不匹配。
- [configs/p4_cohit_samehead_st1.ini](/home/ray/code/SPTAG/configs/p4_cohit_samehead_st1.ini)：same-head CoHit st=1 eval 配置。

P4 same-head 有效结果：

| TopRGlobal | Baseline pages | CoHit pages | Page reduction | Baseline payload KB | CoHit payload KB | QPS uplift | Recall delta |
|------------|----------------|-------------|----------------|---------------------|------------------|------------|--------------|
| 256 | 94.9602 | 80.5127 | **15.22%** | 379.8408 | 322.0508 | **19.28%** | +0.00010 |
| 512 | 121.9773 | 99.3466 | **18.55%** | 487.9092 | 397.3864 | **17.57%** | +0.00010 |
| 768 | 132.7546 | 128.8844 | **2.92%** | 531.0184 | 515.5376 | **4.58%** | -0.00028 |

解释：

- P4 first CoHit build 重新生成了 head，不能用于 layout 结论；same-head CoHit 才是有效对照。
- CoHit v1 已证明 build-side payload reorder 能实际降低 payload pages，并把 page reduction 转化为 QPS uplift。
- `TopRGlobal=768` 收益弱，说明当前 offline trace order 没有充分优化高 TopR 下的 page fanout；下一步应改 order objective，而不是回到 CPU/copy/exact distance 优化。

### 剔除或降级方向

- 不继续盲扫 `PostingChunkTargetSize`，chunk-locality 已失败。
- 不继续堆 `PostingTopRPerPosting`，P2.2 显示 Recall 基本不变且 CPU 成本上升。
- 不把高 `PostingTopRGlobal` 当性能优化，它只是用更多 payload read 换 Recall。
- 不把 PQ code sort 作为 locality 方向，P1.3a 已失败。
- 不把 payload merge、code async、dynamic/KV 放进当前主线。

## 下一步执行计划（2026-05-02 最新）

### 当前优先级

**P4d: Layout 策略整合（已完成）**

P4c 已证明 query-set objective 可以解决 768 的弱点，但代价是 512 的回退。P4d 真实 same-head 权重 sweep 进一步证明：不同 TopR 档位的 locality 最优 layout 不兼容，且无法通过简单权重参数调和。

可能方向：
1. **参数化 layout**：根据 runtime TopR 选择不同 layout（需要构建多个索引版本或多 layout 分区）
2. **统一 layout**：寻找在所有 TopR 档位都有合理表现的目标函数（需要更复杂的 multi-objective optimization）
3. **默认 TopR 选择**：如果系统有主要使用的 TopR 档位，可以针对该档位优化（需要产品决策）

**结论**：P4 系列已完成。build-side payload reorder 是有效的，但不存在当前定义下的统一 Pareto layout。P4d 的离线 evaluator 已证明不能准确预测真实 pages，只能作为覆盖率诊断工具；真实 same-head build/search 权重 sweep 已证明 tradeoff 不可调和。

#### Hypothesis

P4c 的 512 回退主要来自 768 权重过强，而不是 query-set objective 方向错误。通过离线评估不同 512/768 trace 权重和 coverage scoring，应能找到一个 Pareto layout：512 保持接近 v1/v2，768 明显优于 v1/v2。

#### 当前实现状态

- [scripts/build_payload_cohit_order.py](/home/ray/code/SPTAG/scripts/build_payload_cohit_order.py) 已新增 `--strategy page-aware`；旧默认 `adjacent` 行为保持不变。
- `page-aware` 策略按 posting 贪心填充 4KB page，优先把 query co-hit 的 vector 放入同一 page。
- 已新增 [configs/p4_cohit_pageaware_build_samehead.ini](/home/ray/code/SPTAG/configs/p4_cohit_pageaware_build_samehead.ini) 和 [configs/p4_cohit_pageaware_samehead_st1.ini](/home/ray/code/SPTAG/configs/p4_cohit_pageaware_samehead_st1.ini)。
- P4b v2 已验证完毕，结果说明 page-fill 贪心不是最终目标函数。
- P4c query-set 已实现并验证：768 提升到 `14.92%`，但 512 回退到 `15.34%`。
- P4d 已完成离线 evaluator 开发，但 evaluator 预测误差 `16%~32%`，不能作为准确筛选器。
- evaluator 的有效用途是诊断 order 覆盖率：v1 只覆盖 `78.6%` 的 768 trace vectors，而 v3 覆盖 `100%`。
- 已完成 768 权重为 `0.75/1.0/1.25/1.5` 的真实 same-head build/search 验证。

#### 验收判定

P4d 判为 **完成，统一 layout 失败**：

- 权重参数影响极小：`w0.75~w1.5` 差异在 `1%` 以内，无单调趋势。
- Trace coverage 是主导因素：512-only trace 得到 `512=18%~19%, 768=2%~3%`；combined trace 得到 `512≈15%, 768≈15%`。
- 不存在满足 `512>=18%` 且 `768>=10%` 的统一 layout。
- 已明确产品化选择：512-optimized、combined balanced、或参数化多索引。

#### 产品化选择

| 方案 | 512 表现 | 768 表现 | 适用场景 |
|------|----------|----------|----------|
| v1/v2 (512-only) | `18%~19%` | `2%~3%` | 默认 TopR=512 |
| v3 (combined) | `≈15%` | `≈15%` | 需要支持多 TopR |
| 参数化多索引 | 各自最优 | 各自最优 | 运维成本高 |

#### 失败信号

- 同一 `(postingID, vectorID)` 在 order file、build log、trace 中映射不一致。
- `PagesRead` 下降但 `PayloadPhysicalBytesRead` 不降，且不是由 tail-page padding 或统计口径解释。
- `payloadPageHash` 或 final result hash 在 same config 重跑间不稳定。
- `candidates_per_payload_page` 不升但 pages 下降，说明统计或 page dedupe 口径可能错误。
- 离线预测 pages 下降，但真实 `unique_payload_pages` 不降。
- 权重 sweep 的结果不单调且无法由 trace 覆盖差异解释。
- 某个候选 order 对目标 TopR trace vector 覆盖率低于 `95%`，但仍被用于真实 build/search。

#### 后续方向

- 不继续调 CoHit 权重。
- 默认 `TopRGlobal=512` 时，推荐 v1/v2。
- 需要同时支持 512/768 时，推荐 v3 combined，接受 512 回退。
- 只有在必须同时拿到各自最优时，才考虑参数化多索引，并单独评估磁盘/构建/运维成本。

#### P4b 已执行命令（历史复现）

```bash
scripts/build_payload_cohit_order.py \
  --trace /tmp/p3_cohit_trace/payload_trace.csv \
  --output /tmp/p4_cohit_layout_pageaware/cohit_order.tsv \
  --strategy page-aware \
  --page-size 4096

./Release/ssdserving configs/p4_cohit_pageaware_build_samehead.ini

scripts/run_spann_p2_topr_sweep.sh \
  -c configs/p4_cohit_pageaware_samehead_st1.ini \
  -o /tmp/p4_cohit_layout_pageaware_samehead/st1_eval \
  -r 64 \
  -g 256,512,768 \
  -t 1 \
  --no-monitor

scripts/run_spann_p2_topr_sweep.sh \
  -c configs/p4_cohit_pageaware_samehead_st1.ini \
  -o /tmp/p4_cohit_layout_pageaware_samehead/st8_eval \
  -r 64 \
  -g 256,512,768 \
  -t 8 \
  --no-monitor
```

#### P4b v2 首轮结果

page-aware v2 已完成 order 生成、same-head build、`st=1` 和 `st=8` sweep。build 日志确认加载 `2,706,632` rows、`128,814` postings。

| TopRGlobal | Baseline pages | CoHit v1 pages | Page-aware v2 pages | v2 vs baseline | v2 vs v1 | v2 QPS uplift vs baseline | Recall |
|------------|----------------|----------------|---------------------|----------------|----------|---------------------------|--------|
| 256 | 94.9602 | 80.5127 | 79.5762 | **16.20%** | 1.16% | **19.72%** | 0.96204 |
| 512 | 121.9773 | 99.3466 | 98.4516 | **19.29%** | 0.90% | **20.40%** | 0.97678 |
| 768 | 132.7546 | 128.8844 | 128.7897 | **2.99%** | 0.07% | **4.94%** | 0.97897 |

判定：

- v2 没有达到主要成功标准：`TopRGlobal=768` 仍未提升到 `10%+` page reduction。
- v2 是弱正向 ablation：256/512 小幅优于 v1，Recall 稳定，说明实现没有破坏语义。
- `st=8` 下 Recall 与 pages/KB 稳定，未见并发语义漂移；但该 summary 的 `approx_single_thread_qps` 不应当作总吞吐。

下一步：

- 不继续微调当前 page-fill 贪心。
- 改为 query-set/page-objective：每个 page 选择一组 vector 时最大化 query coverage，而不是只看局部 pair adjacency。
- 对 512/768 使用混合 trace 权重，明确把 high-TopR fanout 纳入目标函数。
- 验收标准不变：512 不低于 v1，768 page reduction 达到 `10%+`，Recall/hash 稳定。

### 2. Two-stage per-phase cost attribution

#### Hypothesis

Two-stage 在 payload KB 接近官方 baseline 时仍慢很多。原始假设是差距可能主要来自 compact-code scan、candidate merge/dedupe、payload read-plan 等 CPU pipeline overhead；P1 attribution 用于验证该假设并定位主耗时阶段。

#### 成功结果

- 阶段耗时或 cycles 合计尽量解释端到端 latency；原目标为 `90%+`，本轮实际闭合 `83.8%`，但足以定位主阶段。
- 能定位主要成本阶段。
- matched recall 点上能量化 two-stage 相比 legacy 的额外成本。

#### 失败结果

- 阶段耗时合计与端到端 latency 不闭合。
- 主要耗时落在未归类区间。
- instrumentation 本身改变 Recall、hash 或 QPS。

#### 失败信号

- `batch_read_total_ms` 与 payload read 子阶段不一致。
- `DistanceEvaluatedCount` 固定但 rerank 时间大幅漂移。
- `PostingTopRPerPosting` 上升但 scan/merge 时间不变，说明统计没覆盖正确路径。

#### Ablation 预期

- `PostingTopRGlobal=128 -> 256 -> 512 -> 768`：payload read-plan、read wait、exact rerank 时间上升。
- `PostingTopRPerPosting=32 -> 64 -> 128`：coarse scan/merge 成本上升，Recall 基本不变。
- `SearchThreadNum=1 -> 8`：per-query CPU 阶段分布大体相近，I/O wait 和调度竞争可能变化。

### 3. Fetch Payload & Rerank 子阶段拆分

#### Hypothesis

`FetchPayloadPagesAndRerank` 的主成本来自 payload page read wait 和低 candidates/page 造成的 page fanout；payload copy 与 exact distance 是次要成本。

#### 成功结果

- 将 `Fetch Payload & Rerank = 2.120 ms` 拆成 read wait、payload copy、exact distance、result insertion 等子阶段。
- 子阶段耗时合计解释父阶段的 `90%+`。
- pages/KB 上升时，payload read wait 同步上升。

#### 失败结果

- 子阶段合计与父阶段不闭合。
- exact distance 成为主成本，但 `DistanceEvaluatedCount` 与耗时不相关。
- payload read wait 不随 pages/KB 变化。

#### 失败信号

- 子阶段耗时之和大于父阶段耗时。
- `unique_payload_pages` 下降但 payload read wait 不降。
- 同 query 重复运行时子阶段比例大幅漂移。
- instrumentation 改变 final hash 或 Recall。

#### Ablation 预期

- `TopRGlobal=256 -> 512 -> 768`：read wait、exact distance、result insertion 上升，其中 read wait 与 pages/KB 最相关。
- `PostingPayloadBatchPages=1 -> 4 -> 8`：read wait 或 batch overhead 改变，但 Recall 和 candidate hash 不变。
- single-page payload fast path on/off：payload copy 时间变化；若 QPS 不变，copy 不是主瓶颈。
- `SearchThreadNum=1 -> 8`：read wait 受并发和设备队列影响，exact distance per query 基本稳定。

### 4. Query co-hit trace / theoretical best packing

#### Hypothesis

只有 query-time co-hit 才可能解释 payload page locality。若 theoretical best packing 也不能把 `unique_payload_pages` 降低 `10%~15%`，则 payload layout 路线应停止。

#### 成功结果

- 导出 global TopR 后每 query 的 `(queryID, postingID, vectorID, payloadPageID)`。
- `TopRGlobal=512` 或 matched recall 点的 theoretical best packing 显示 `10%~15%+` page reduction。
- train 与 held-out query 方向一致。
- trace 重算的 current pages 与 detailed CSV 的 `unique_payload_pages` 差异不超过 `1%~2%`。

#### 失败结果

- 理论最优 packing 只能带来小于 `10%` 的 page reduction。
- 收益只在训练 query 上出现，held-out query 无收益。
- 大部分 page cost 来自跨 posting fanout，posting 内重排触达不了。

#### 失败信号

- trace 反算 current pages 与 detailed CSV 差异超过 `1%~2%`。
- 同一 `(postingID, vectorID)` 映射到多个 payload offset 且无法解释。
- best packing 显示大幅改善但 candidates/page 不升。

#### Ablation 预期

- `TopRGlobal=256 -> 512 -> 768`：current pages 上升；有效 co-hit 信号应让 best/current page ratio 保持或改善。
- per-query oracle best packing：如果乐观上限也小于 `10%`，停止 layout 实现。
- per-posting best packing：若收益小，说明主要问题是跨 posting fanout。
- train/held-out split：有效信号应在 held-out 上仍降低 pages。

### 5. Co-hit layout 原型（条件执行）

#### Hypothesis

如果 theoretical best packing 有足够上限，则按 query co-hit 重排 posting blob 内 payload 可以降低 actual pages，并在不改变 coarse/topR/rerank 语义时提升 matched recall QPS。

#### 当前实现状态

P4 条件已满足，当前实现采用 offline trace order：

- 输入：P3 payload trace。
- 离线生成：每个 posting 内的 vector order rank。
- Build：`PostingPayloadLayout=CoHit` 读取 `PostingCohitOrderFile` 并重排 posting blob 内 code/payload record。
- Search：不改变 coarse scan、TopR、exact rerank 语义，只观察 actual pages/KB/QPS 是否改善。

#### 成功结果

- `PagesRead` 或 `PayloadPhysicalBytesRead` 下降 `10%~15%+`。
- `avg(Candidates/Page)` 相对提升 `20%+`。
- matched recall 下 QPS 提升 `10%+`。
- Recall 下降不超过 `0.001~0.002`，payload byte hash 与 exact distance 校验一致。

#### 失败结果

- layout 后 pages/KB 基本不变。
- pages 降了但 QPS 不升，说明瓶颈转向 CPU/调度。
- Recall 出现不可解释下降。
- held-out query 没有 locality 收益。

#### 失败信号

- payload byte hash 或 exact distance 校验不一致。
- final result hash 异常漂移。
- 单线程有效但 `st=8` 收益消失。
- 重复构建同一 layout 的 record order 不稳定。

#### Ablation 预期

- `baseline_layout -> pq_code_layout`：仍接近 P1.3a，locality 几乎不变。
- `baseline_layout -> chunk_locality_layout`：根据当前结果，预期 pages/KB 不改善。
- `baseline_layout -> cohit_layout`：pages/KB 下降，candidates/page 上升。
- `TopRGlobal=256/512/768`：有效 layout 应在多个 TopR 档位方向一致。

### 6. Same-head matched 对照

#### Hypothesis

Same-head 对照可以补强质量归因，但不会改变当前“two-stage 在现有 layout 下无性能优势”的主结论。

#### 成功结果

- legacy 与 two-stage 从同一 head 派生，或有 meta/checksum 证明 head 一致。
- same-head 下仍复现 payload/read 成本相近但 two-stage QPS 更低。

#### 失败结果

- 无法构造 same-head 对照。
- same-head 后结论方向改变，说明此前混入 head build 差异。

#### 失败信号

- 两个目录不同且没有 head checksum/build log/meta 证据。
- same-head 与非 same-head 的 query-level head result hash 方向相反。

#### Ablation 预期

- same-head legacy `ir64` vs two-stage `TopRGlobal=512/768`：Recall 接近时 payload pages 仍接近，two-stage QPS 仍低。
- same-head two-stage TopR sweep：`TopRGlobal` 上升带来 Recall 上升和 payload cost 上升。
- 如果 same-head 消除大部分 Recall gap：P2 not-observed bucket 应下降，但 payload locality 不应自动改善。

## P1: Two-Stage Per-Phase Cost Attribution 结果 (2026-05-02)

### 实验设计

在 Two-Stage 搜索 pipeline 的 5 个阶段添加计时：
1. ReadPostingHeaderAndDirectory
2. ScanCompactCodes
3. MergeCoarseCandidates
4. BuildPayloadReadPlan
5. FetchPayloadPagesAndRerank

对比 Legacy baseline 与 Two-stage 在 `st=1, ir=64` 下的性能差异。

### 测试结果

| 指标 | Legacy | Two-Stage | Ratio |
|------|--------|-----------|-------|
| Total Latency (ms) | 1.063 | 3.837 | 3.61x |
| Ex Latency (ms) | 0.720 | 3.430 | **4.76x** |
| Batch Read Total (ms) | 0.698 | 2.002 | 2.87x |

### Per-Phase Timing Breakdown (Two-Stage)

| 阶段 | 耗时 (ms) | 占 Ex 比例 | 占 Phase 比例 |
|------|-----------|------------|---------------|
| Read Header & Directory | 0.011 | 0.3% | 0.4% |
| Scan Compact Codes | 0.368 | 10.7% | 12.8% |
| Merge Coarse Candidates | 0.328 | 9.6% | 11.4% |
| Build Payload Read Plan | 0.046 | 1.3% | 1.6% |
| Fetch Payload & Rerank | 2.120 | **61.8%** | **73.8%** |
| **Phase Time Total** | 2.873 | 83.8% | 100% |

### Overhead 分析

- Two-Stage CPU Overhead (scan+merge+plan): **0.742 ms**
- Legacy vs Two-Stage Ex Latency Gap: **2.710 ms**
- CPU Overhead 解释了 latency gap 的 **27.4%**

### 关键发现

1. **Fetch Payload & Rerank 是主要瓶颈**：占 Two-Stage phase time 的 73.8%
2. **Two-Stage CPU overhead 不是主要问题**：仅解释 27% 的 latency gap
3. **剩余 73% 的 latency gap 来自 Fetch Payload 阶段**：
   - Two-Stage: 2.12 ms
   - Legacy Batch Read: 0.70 ms
   - 差异可能来自 payload page inflation 和额外的 I/O 等待

### 结论

Two-stage pipeline 的 CPU 开销（scan/merge/plan）不是性能瓶颈的主因。主要瓶颈仍然是：
- **Fetch Payload & Rerank 阶段的 payload page I/O**
- 这与之前的 locality 分析一致：candidates/page 很低导致需要读取大量物理页

### 验收判断

P1 per-phase attribution **成功**：
- 阶段耗时合计解释了端到端 latency 的 83.8%（超过 90% 目标未达到，但已足够定位瓶颈）
- 明确定位主要耗时阶段为 Fetch Payload & Rerank
- 证明了 CPU pipeline overhead 不是主要问题

## P2: Fetch Payload & Rerank 子阶段拆分结果 (2026-05-02)

### 实验设计

将 `FetchPayloadPagesAndRerank` 阶段进一步拆分为：
1. Payload Read Wait - I/O 等待时间
2. Payload Copy - 多页 payload buffer 拷贝
3. Exact Distance - 精确距离计算
4. Result Insertion - 结果插入

### 测试结果

| 子阶段 | 耗时 (ms) | 占父阶段比例 |
|--------|-----------|--------------|
| Payload Read Wait | 2.014 | **89.0%** |
| Exact Distance | 0.106 | 4.7% |
| Result Insertion | 0.016 | 0.7% |
| Payload Copy | 0.014 | 0.6% |
| **子阶段合计** | 2.150 | **95.0%** |

### 关键发现

1. **Payload Read Wait 占主导地位**：占 Fetch Payload & Rerank 的 89.0%
2. **CPU 成本是次要的**：
   - Exact Distance: 仅 4.7%
   - Payload Copy: 仅 0.6%
   - Result Insertion: 仅 0.7%
3. **闭合度良好**：子阶段解释了父阶段的 95.0%
4. **综合影响**：
   - Payload Read Wait = 66.8% 的 Two-Stage 总 phase time
   - 这验证了 payload page I/O 是绝对主要瓶颈

### 结论

P2 子阶段拆分 **成功**：
- 明确证明了 Payload Read Wait 是主要成本（89% of phase, 67% of total）
- 验证了 hypothesis：主成本来自 payload page I/O wait，而非 CPU
- 排除了 exact distance、payload copy、result insertion 作为优化目标
- 后续应专注于减少 payload pages；P3 已证明可以进入 co-hit layout，P4 v1 已证明方向有效

### 下一步

P2 已确认主瓶颈是 Payload Read Wait。P3/P4 已经把”是否值得做 layout”变成”如何做更好的 layout”：
- P3 已证明 theoretical best packing 上限为 `40%~50%`
- P4 same-head CoHit v1 已证明 256/512 有实际 page/QPS 收益
- P4b page-aware CoHit v2 已完成，证明 page-fill 贪心不足
- P4c query-set CoHit v3 已完成，768 显著改善但 512 回退
- 下一步需要设计统一 layout 或参数化 layout 策略

## P4c: Query-set/Page-objective CoHit v3 结果 (2026-05-02)

### 实验设计

在 P4 v1 (adjacent pair co-hit) 和 P4b v2 (page-aware greedy fill) 基础上，实现 P4c v3 (query-set coverage objective)：
- 目标函数：最大化每个 payload page 覆盖的 query co-hit 集合
- 使用 256/512/768 合并 trace，并给 768 赋予 1.5x 权重
- same-head 对照，与 baseline、v1、v2 对比

### 测试结果

| TopRGlobal | Layout | Unique Pages | Payload KB | Page Reduction | Cand/Page |
|------------|--------|--------------|------------|----------------|-----------|
| 256 | Baseline | 94.96 | 379.84 | - | 2.75 |
| 256 | v1 (adjacent) | 80.51 | 322.05 | 15.22% | 3.20 |
| 256 | v2 (page-aware) | 80.04 | 320.18 | 15.71% | 3.21 |
| 256 | v3 (query-set) | 80.11 | 320.43 | **15.64%** | 3.20 |
| 512 | Baseline | 121.98 | 487.91 | - | 4.30 |
| 512 | v1 (adjacent) | 99.35 | 397.39 | 18.55% | 5.19 |
| 512 | v2 (page-aware) | 98.50 | 394.02 | 19.29% | 5.22 |
| 512 | v3 (query-set) | 103.27 | 413.07 | **15.34%** | 4.96 |
| 768 | Baseline | 132.75 | 531.02 | - | 5.94 |
| 768 | v1 (adjacent) | 128.88 | 515.54 | 2.92% | 6.01 |
| 768 | v2 (page-aware) | 128.78 | 515.15 | 2.99% | 6.02 |
| 768 | v3 (query-set) | 112.95 | 451.79 | **14.92%** | 6.80 |

### 关键发现

1. **v3 在 768 上取得突破**：
   - Page reduction 从 v1/v2 的 ~3% 提升到 **14.92%**
   - 这是 5 倍的改进！
   - Payload KB 从 531 降到 452，节省约 80KB

2. **v3 在 512 上反而退步**：
   - Page reduction 从 v2 的 19.29% 降到 15.34%
   - 多读了约 5 个 pages，约 20KB

3. **v3 在 256 上持平**：
   - Page reduction 约 15.6%，与 v1/v2 相当

### 收益/代价分析

v3 的 query-set objective 更适合高 TopR 场景的原因：
- 高 TopR 时，候选更多，更容易找到能”完全覆盖”某个 query 的 page
- 给 768 赋予更高权重，使 layout 更偏向高 TopR 的 co-hit 模式
- 但这牺牲了中等 TopR (512) 的 locality

### 验收结论

**P4c 部分成功**：
- ✅ 768 page reduction 从 ~3% 提升到 ~15%，达到 10%+ 目标
- ❌ 512 page reduction 从 19% 回退到 15%
- ✅ 256 基本持平
- ⚠️ 存在 TopR-level tradeoff

### 后续方向

当前发现的核心问题是：不同 TopR 档位的 locality 最优 layout 不兼容。

可能方向：
1. **参数化 layout**：根据 runtime TopR 选择不同 layout（需要构建多个索引版本）
2. **统一 layout**：寻找在所有 TopR 档位都有合理表现的目标函数
3. **默认 TopR 选择**：如果系统有主要使用的 TopR 档位，可以针对该档位优化

需要根据实际应用场景决定。

## P4d: Layout 策略整合进展 (2026-05-02)

### 离线评估器开发

已实现 `scripts/evaluate_payload_order.py` 和 `scripts/evaluate_payload_order_v2.py`，但发现评估器存在以下问题：

1. **预测误差大**：绝对 page 数误差 16%~32%，远超 1%~2% 目标
2. **排名预测错误**：对 768 的 layout 排名预测与实际相反
3. **模型缺失因素**：
   - Query-level co-hit pattern 没有被正确模拟
   - Order 文件与 trace 的覆盖率差异没有被考虑

### 关键发现：Order 文件覆盖率

| Order | 512 Trace Coverage | 768 Trace Coverage |
|-------|--------------------|--------------------|
| v1 (adjacent) | 100% | 78.6% |
| v3 (query-set, w=1.5) | 100% | 100% |

**这解释了为什么 v3 在 768 上效果更好**：v1 order 是从 512 trace 生成的，只覆盖 78.6% 的 768 trace vectors，剩下 21.4% 使用默认顺序导致 locality 差。

### 权重 Sweep 准备

已生成不同权重的 order 文件：
- `cohit_order_w0.75.tsv`：768 权重 0.75
- `cohit_order_w1.0.tsv`：768 权重 1.0
- `cohit_order_w1.25.tsv`：768 权重 1.25
- `cohit_order_w1.5.tsv`（即 v3）：768 权重 1.5

### 权重 Sweep 结果 (2026-05-02)

已对 w=0.75/1.0/1.25/1.5 四个权重点进行 same-head build/search 验证：

| Weight | TopR=256 | TopR=512 | TopR=768 |
|--------|----------|----------|----------|
| w0.75 | 17.63% | 15.89% | 14.17% |
| w1.0 | 16.42% | 15.51% | 14.55% |
| w1.25 | 15.67% | 15.34% | 14.91% |
| w1.5 (v3) | 15.64% | 15.34% | 14.92% |

### 关键发现

1. **权重参数影响极小**：
   - w0.75 vs w1.5 的差异：512 仅差 0.5%，768 仅差 0.7%
   - 没有观察到预期的单调趋势

2. **Trace Coverage 是主导因素**：
   - v1/v2 (512-only trace): 512=18~19%, 768=2~3%
   - All combined trace: 512=15~16%, 768=14~15%

3. **Tradeoff 不可调和**：
   - **不存在**满足 512>=18% 且 768>=10% 的统一 layout
   - 这是 trace coverage 的固有问题，不是权重参数能解决的

### 最终结论

**P4d 失败**：未找到满足条件的 Pareto layout。

原因分析：
- 使用 combined trace（包含 256+512+768）时，layout 会同时优化所有 TopR，导致各档位表现均衡但都不突出
- 使用单一 TopR trace 时，只优化该档位，导致其他档位表现差
- 这是 co-hit locality 的本质属性，不是算法问题

### 产品化选择

不存在统一 layout，需要根据产品需求选择：

| 方案 | 512 表现 | 768 表现 | 适用场景 |
|------|----------|----------|----------|
| A) v1/v2 (512-only) | 18~19% | 2~3% | 默认 TopR=512 |
| B) v3 (combined) | 15% | 15% | 需要支持多 TopR |
| C) 参数化多索引 | 各自最优 | 各自最优 | 运维成本高 |
| D) 默认 TopR 选择 | 取决于选择 | 取决于选择 | 需产品决策 |

**推荐**：如果系统有明确的默认 TopR 档位，使用该档位对应的 trace 生成 order。

## P3: Query Co-Hit Trace 与 Theoretical Best Packing 结果 (2026-05-02)

### 实验设计

1. 导出全部 10K queries 的 `(queryID, postingID, vectorID, payloadPageID, payloadBytes)` trace
2. 计算每个 query 的当前 pages vs oracle best-packing pages
3. 测试 TopRGlobal=256/512/768 三个配置
4. 使用 80/20 train/held-out split 验证结果稳定性

### Oracle Best Packing 定义

对于每个 query，计算 per-posting oracle lower bound：
- 将每个 posting 内请求的 payload bytes 紧密打包进 4KB 页
- 求和所有 posting 的页数
- 这是理论上不可实现的 optimistic lower bound，用于判断 layout 优化是否有足够 headroom

### 完整测试结果 (10K queries)

| TopRGlobal | Candidates | Current Pages | Oracle Pages | Page Reduction | Cand/Page (Current) | Cand/Page (Oracle) | Recall@10 |
|------------|------------|---------------|--------------|----------------|---------------------|--------------------|-----------|
| 256 | 256 | 94.96 | 54.56 | **41.6%** | 2.75 | 4.71 | 0.962 |
| 512 | 512 | 121.98 | 61.88 | **48.1%** | 4.30 | 8.28 | 0.977 |
| 768 | 768 | 132.75 | 65.01 | **49.7%** | 5.94 | 11.82 | 0.980 |

### Train/Held-out Split 验证

| TopRGlobal | Split | Queries | Current Pages | Oracle Pages | Page Reduction |
|------------|-------|---------|---------------|--------------|----------------|
| 256 | train | 8000 | 94.84 | 54.59 | 41.46% |
| 256 | heldout | 2000 | 95.42 | 54.46 | **42.09%** |
| 512 | train | 8000 | 121.76 | 61.89 | 47.96% |
| 512 | heldout | 2000 | 122.84 | 61.82 | **48.66%** |
| 768 | train | 8000 | 132.52 | 65.01 | 49.59% |
| 768 | heldout | 2000 | 133.70 | 65.00 | **50.25%** |

### Trace/Stats 闭合验证

- 所有配置 `trace_stats_mismatch_queries = 0`
- trace 反算 current pages 与 detailed CSV 的 `unique_payload_pages` 误差 < 0.02%

### 关键发现

1. **理论 page reduction 上限远超阈值**：
   - 所有 TopRGlobal 配置的 oracle page reduction 都超过 40%
   - P95 page reduction 达到 50%~60%
   - 这证明 payload layout 有巨大的优化空间

2. **Train/Held-out 一致性完美**：
   - held-out 的 page reduction 甚至略高于 train
   - 说明 co-hit signal 在 queries 间具有良好泛化性

3. **Candidates/Page 提升潜力大**：
   - 当前 2.75 ~ 5.94 candidates/page
   - Oracle 可达 4.71 ~ 11.82 candidates/page
   - 约 2x 提升，意味着 payload I/O 可以减半

### 验收结论

**P3 实验成功**：
- ✅ 理论最优 packing 显示 >40% page reduction（远超 10%~15% 阈值）
- ✅ Train 与 held-out 方向一致
- ✅ Trace 与 detailed stats 完全闭合
- ✅ 所有 TopRGlobal 档位方向一致

### 后续决定

**P4/P4b/P4c/P4d 已完成，进入产品化选择**
- P4 v1 same-head st=1 已验证：`TopRGlobal=256/512` actual page reduction 为 `15.22%/18.55%`，QPS uplift 为 `19.28%/17.57%`
- `TopRGlobal=768` actual page reduction 只有 `2.92%`，说明 v1 order objective 对高 TopR fanout 不足
- P4b page-aware CoHit v2 已验证：512 达到 `19.29%`，但 768 仍只有 `2.99%`
- P4c query-set CoHit v3 已验证：768 达到 `14.92%`，但 512 回退到 `15.34%`
- P4d 已验证：权重 sweep 不存在统一解
- 下一步目标：按产品默认 TopR 选择 v1/v2、v3 combined 或参数化多索引
