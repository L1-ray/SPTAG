# SPANN M2/M3 代码与验证方案审查记录（2026-05-01）

## 结论

当前 M2/M3 two-stage 方向只能作为研究线继续，不能再按”性能主线已经接近成功”来描述。更准确的状态是：

1. **格式与搜索闭环已经基本打通**：new-format static posting 能 build / load / search，M2/Chunked-M2 已不再是早期 `distanceEvaluated=0` 的失真状态。
2. **metadata query 热路径问题已经阶段性解决**：metadata 已能在 `LoadIndex` 阶段预解析并缓存，query 阶段 `MetadataBytesRead=0` 的结果证明这一步有效。
3. **code physical read inflation 已通过 code cache 路线显著压掉**：code-cache 档位下 `CodePhysicalBytesRead=0`，QPS 从 metadata-cache 档约 `1.07k` 提升到约 `2.93k`。
4. **当前主瓶颈已定位到 Payload Read Wait**：P1 per-phase attribution 显示 `Fetch Payload & Rerank` 占 two-stage phase time 的 `73.8%`；P2 子阶段拆分显示 `Payload Read Wait` 占该阶段 `89.0%`，折合 two-stage 总 phase time 的 `66.8%`。因此当前主要问题不是 compact-code pipeline CPU、payload copy 或 exact rerank，而是 payload page fanout 导致的 I/O wait。
5. **P3 已证明 payload layout 有足够理论上限**：query co-hit trace 与 theoretical best packing 显示 `40%~50%` page reduction 上限，远超进入 layout 原型的 `10%~15%` 阈值，且 train/held-out 方向一致。
6. **P4/P4b/P4c CoHit same-head 原型已证明 build-side reorder 有效**：在 same-head 对照下，`TopRGlobal=256/512` 可实现 15%~19% page reduction；v3 把 768 从 ~3% 提升到 ~15%，但代价是 512 从 19% 回退到 15%。
7. **P4d 权重 sweep 已完成，证明 tradeoff 不可调和**：权重参数 (0.75~1.5) 对结果影响极小（差异<1%），不存在满足 `512>=18%` 且 `768>=10%` 的统一 layout。这是 trace coverage 的固有问题，不是算法参数能解决的。
8. **下一步需要产品决策**：选择 512-optimized layout (v1/v2)、768-balanced layout (v3)、参数化多索引，或按默认 TopR 固化单一 layout。
9. **历史 benchmark 曾出现高并发 recall 下降，但在当前代码上尚未被单测复现**：旧日志里 `SearchThreadNum=8` 的 recall 低于 `SearchThreadNum=1`，因此仍需继续复核；但当前代码已经补上 query 级 `coarse/payload/final` hash 和并发确定性回归测试，最新 `SPANNPostingFormatTest` 可稳定通过，所以这项目前更准确地说是”待 canonical benchmark 复核的风险”，而不是”当前已确认仍存在的 bug”。
10. **M3 pruning 当前只能按 heuristic 处理**：更细 chunk 能提高 prune ratio，但当前 threshold/chunk/topR 组合会先伤 recall，不能作为默认 safe pruning 主线。
11. **本轮代码已经完成一批基础修正与热路径优化**：包括 `SearchIndex(...)` 失败路径 workspace 归还、static two-stage workspace 固定 async channel、code bytes 统计纠正、workspace 级 buffer/hash-map 复用、payload read plan 页键缓存与 rerank 内环减扫，这些改动已通过 `SPANNPostingFormatTest` 回归。

因此，下一阶段不应继续围绕”是否实现 M2/M3”讨论，也不应继续盲目扩大 layout/chunk/topR sweep，而应围绕以下问题收敛：

- **统一 layout 不存在，需要产品决策**：选择 512-optimized / 768-balanced / 参数化多索引；
- coarse candidate recall 损失来自哪里，是否值得继续用更大 `PostingTopRGlobal` 换 recall；
- M3 pruning 是否有 exact upper bound 支撑，否则只能作为 heuristic 实验；
- benchmark 是否始终使用同一 canonical raw log 口径。

---

## 1. 当前代码整体判断

### 1.1 已经做对的部分

M2 query pipeline 已经符合目标架构：

- `ReadPostingHeaderAndDirectory`
- `ReadChunkCodeBlocks`
- `ScanCompactCodes`
- `MergeCoarseCandidates`
- `BuildPayloadReadPlan`
- `FetchPayloadPagesAndRerank`

入口位于：

- `AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:761`

这个 pipeline 的价值是把 legacy 的“读整 posting + full vector scan”拆成：

```text
metadata / directory
→ compact code coarse scoring
→ candidate merge / dedupe
→ selected payload page fetch
→ exact rerank
```

对应 `SPANN_Problem_old_20260430.md` 中 M2/M3 的目标语义。

Two-stage static 相关 guardrail 已经存在：

- experimental posting format 只允许 `Storage::Static`：`AnnService/src/Core/SPANN/SPANNIndex.cpp:166`
- `EnableChunkedPosting` 要求 `EnableTwoStagePosting`：`AnnService/src/Core/SPANN/SPANNIndex.cpp:173`
- `PostingChunkPruneMode` 非 None 时要求 L2，且明确 warning 当前不是 exact safe prune：`AnnService/src/Core/SPANN/SPANNIndex.cpp:202`
- experimental format 要求 `SSDPostingFormatVersion >= 2`：`AnnService/src/Core/SPANN/SPANNIndex.cpp:214`
- delta / rearrange / compression 与 Phase 1 two-stage 暂不混用：`AnnService/src/Core/SPANN/SPANNIndex.cpp:228`

统计拆分也已经比早期清楚：

- `m_metadataBytesRead`
- `m_codePhysicalBytesRead`
- `m_codeBytesRead`
- `m_payloadLogicalBytesRead`
- `m_payloadBytesRead`
- `m_readPages`
- `m_diskIOCount`

定义与聚合位于：

- `AnnService/inc/Core/SPANN/IExtraSearcher.h:26`

这些指标是后续判断瓶颈的基础，不应再只看总 `requestedReadBytes` 或最终 QPS。

另外，当前代码还新增了两类对后续排障很关键的基础设施：

- query 级阶段哈希：`m_coarseCandidateHash`、`m_payloadPageHash`、`m_finalResultHash`，并已导出到 detailed stats CSV：`AnnService/inc/SSDServing/SSDIndex.h`
- chunked two-stage 并发确定性回归：`Test/src/SPANNPostingFormatTest.cpp`

这意味着后续如果 benchmark 再出现 `st=1`/`st=8` 结果差异，已经可以直接定位是 coarse candidate、payload read plan 还是 final rerank 开始分叉。

### 1.3 对评审文档准确性的补充说明

这份评审文档整体方向是准确的，尤其是以下几条判断已经被代码和测试进一步坐实：

- static two-stage / chunked two-stage 搜索主路径已经成型，不再是“格式生成完但搜索链路未闭环”的早期状态；
- metadata cache 与 code cache 确实是有效优化，而不是偶然 benchmark 噪声；
- `PostingChunkPruneMode` 当前确实只能明确标成 heuristic；
- payload physical pages 仍然是 two-stage 内部最明确的 I/O 问题；P1 attribution 进一步显示主要耗时集中在 Fetch Payload & Rerank，而不是 scan/merge/plan CPU overhead。

但有一条需要降级为“合理怀疑而非已证实结论”：

- 文档中把高并发 recall 下降直接归因到 workspace/request 生命周期问题，这个因果链目前并没有被代码单独证明。

更准确的表述应当是：

- 历史 benchmark 观察到了高并发 recall 下降；
- 当前代码已修掉若干确实可能影响并发稳定性的点，并新增了并发确定性测试；
- 最新单测未复现该问题，因此后续要用 canonical benchmark 和 query-level hash 继续复核，而不是直接把根因定性为 workspace 生命周期污染。

### 1.2 已经过时、需要从旧结论中移除的点

#### metadata `O_DIRECT` buffer 对齐风险不再是当前最高优先级

旧版审查认为 `ReadPostingHeaderAndDirectory(...)` 用 `std::vector<char>` 接 direct I/O，存在 `O_DIRECT` buffer alignment 风险。当前代码已经改为 workspace/page-aligned buffer：

- `EnsurePostingMetadataBufferCapacity(...)` 后从 `m_postingMetadataBuffer` 取 buffer；
- fallback 的 load-time metadata cache 也使用 `Helper::PageBuffer<std::uint8_t>`。

对应位置：

- query metadata path：`AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:1309`
- load-time metadata cache：`AnnService/inc/Core/SPANN/ExtraStaticSearcher.h:2837`

所以这项不应继续作为 P0 correctness blocker。后续只需要保留回归检查：任何 direct I/O 读都必须满足 offset / size / buffer 三者对齐。

#### “report 与计划 6.6 QPS 冲突”不再作为主阻塞项

旧审查中提到某些保存的 `report.md` 中 new-format QPS 异常高，与计划 6.6 的 raw log 不一致。后续计划已经把 canonical 运行口径拆到 raw benchmark log 和分项统计上，并且 M2 结果已经经历 metadata cache / code cache 多轮复测。

因此这项不应继续排在优化优先级前列。保留的原则是：

- QPS / Recall 以同一轮 raw search log 为准；
- `report.md` 可用于系统摘要，但不能替代 raw log；
- 每个结论必须写清配置、索引目录、结果目录、commit/二进制来源。

---

## 2. 现有测试结果的当前解读

### 2.1 官方对齐 legacy baseline 是强基线

strict `UInt8 + DEFAULT`、同盘、官方算法参数对齐、本机线程优化后，代表性 legacy baseline 为：

| 配置 | QPS | Recall@10 | avg latency | requested bytes |
|---|---:|---:|---:|---:|
| `st8_nt16_ir64_pl4` | `≈5945` | `≈0.978319` | `≈1.34 ms` | `≈486KB/query` |

这个 baseline 的特点是：

- `SearchThreadNum=8` 已接近当前 `ir64` 下吞吐饱和点；
- `NumberOfThreads=16~40` 对 QPS 影响很小；
- `InternalResultNum` 是 legacy 中 recall / I/O / latency 的主 tradeoff 旋钮；
- legacy 虽然有读放大和重复读，但 I/O 路径成熟，批量读和顺序扫描效率高。

因此，M2/M3 要证明性能收益，不能只证明 `distanceEvaluatedCount` 下降；必须在 matched recall 下同时压低 physical bytes/pages 或明显改善尾延迟/QPS。

### 2.2 初始 M2/M3 可搜索，但性能失败

早期修复 header/layout/O_DIRECT 后，M2/M3 已能稳定搜索：

| 方案 | QPS | Recall@10 | DistanceEvaluated | 结论 |
|---|---:|---:|---:|---|
| legacy | `≈5928` | `≈0.9783` | `≈2314` | 强基线 |
| M2 | `≈715` | `≈0.9575` | `256` | correctness 基本闭环，性能失败 |
| Chunked-M2 | `≈698` | `≈0.9585` | `256` | M3 path 可跑，prune 收益弱 |

这组结果说明：

- M2 成功减少 exact rerank 数；
- 但减少计算没有转化成端到端性能；
- 原因是 new-format 引入 metadata/code/payload 多阶段 I/O 和 page inflation。

### 2.3 metadata cache 已验证有效，但不是唯一瓶颈

metadata cache 后：

| 方案 | QPS | Recall@10 | MetadataBytesRead | CodePhysicalBytesRead | PayloadPhysicalBytesRead |
|---|---:|---:|---:|---:|---:|
| M2(metadata cache) | `≈1072~1086` | `≈0.955~0.956` | `0` | `≈339KB` | `≈388KB` |

结论：

- metadata 是真实固定成本；
- 把 metadata 移出 query 热路径带来约 48% QPS 提升；
- 但 QPS 仍远低于 legacy，说明 code/payload physical read inflation 仍是大头。

### 2.4 code cache 显著改善 I/O，但历史并发 recall 现象仍需在当前代码上复核

code cache 后：

| 配置 | QPS | Recall@10 | CodePhysicalBytesRead | PayloadPhysicalBytesRead | PagesRead |
|---|---:|---:|---:|---:|---:|
| `SearchThreadNum=1` | `≈433` | `≈0.9627` | `0` | `≈388KB` | 未作为高并发基线 |
| `SearchThreadNum=8` | `≈2929` | `≈0.941` | `0` | `≈388KB` | `≈94.7` |

这组结果很关键，但需要注意“结果观测日期”和“当前代码状态”不能混写：

1. code cache 的结构性收益是真实的：`CodePhysicalBytesRead=0`，`RequestedBytesRead` 从约 `727KB` 收敛到约 `388KB`。
2. 剩余 I/O 大头集中在 payload page：payload physical bytes/pages 仍高；P1 attribution 表明端到端差距主要集中在 Fetch Payload & Rerank 阶段，scan/merge/plan CPU overhead 不是主因。
3. 历史 benchmark 中 `SearchThreadNum=8` 的 recall 曾低于 `SearchThreadNum=1`，因此并发语义稳定性仍需继续复核。
4. 但当前代码已经新增 query-level hash trace 和 `ChunkedTwoStageConcurrentSearchDeterministic` 回归测试，且最新测试通过，因此现阶段不能再把“并发 recall 下降”表述成一个仍在当前代码中稳定复现的已证实 bug。

### 2.5 payload merge 试验不能进入主线

payload 连续页合并读取试验曾观察到：

- QPS 有提升；
- `diskIOCount` 和 batch latency 有下降；
- 但 `Recall@10` 稳定下降；
- `RequestedBytesRead` / `PayloadPhysicalBytesRead` 基本没降。

这说明该试验目前更像是改变了调度形态或引入了 page-to-buffer 映射风险，而不是解决 payload physical inflation。它不能作为默认主线。

保留的有效结论是：

- 减少请求数确实可能提升 batch latency；
- 但 payload 是 exact rerank 输入，任何 page mapping / offset / tail-page 错误都会直接污染 recall；
- payload 优化必须先证明 physical pages/bytes 下降和 recall 语义等价。

### 2.6 M3 t32 说明 chunk 更细有效，但当前 pruning/topR 组合不安全

`PostingChunkTargetSize=32` 的结果：

- prune ratio 明显提高；
- code/payload bytes 有下降；
- 但 `Recall@10` 从约 `0.958` 降到约 `0.927`；
- 提高 topR 后 recall 只部分恢复，QPS 进一步下降。

结论：

- 更细 chunk 可以改善可裁剪性，这是正信号；
- 但当前 chunk 划分、lower-bound threshold、topR 截断组合不适合作为默认路径；
- M3 当前应作为 heuristic 实验项，而不是主性能来源。

---

## 3. 当前结论与执行优先级（2026-05-02 校准）

### 3.1 官方 strict baseline 是性能主线

官方对齐 `UInt8 + DEFAULT` baseline 已经给出清晰性能边界：

| 配置 | QPS | Recall@10 | avg latency | requested bytes |
|---|---:|---:|---:|---:|
| `st1_nt40_ir64_pl4` | `1271.13` | `0.978319` | `0.787 ms` | `486076.826` |
| `st4_nt40_ir64_pl4` | `4746.08` | `0.978319` | `0.842 ms` | `486076.826` |
| `st8_nt16_ir64_pl4` | `5945.30` | `0.978319` | `1.344 ms` | `486076.826` |

阶段性判断：

- 默认性能方案应优先沿官方 strict baseline 调参，而不是把当前 two-stage 作为默认替代。
- `SearchThreadNum=4~8` 是吞吐有效区间；`st=16/20` 不再提升 `ir64` QPS，只拉高 latency。
- `InternalResultNum` 是官方 baseline 中唯一同时影响 Recall、QPS、latency 和 read bytes 的主旋钮。
- `SearchPostingPageLimit=4 -> 8` 基本无收益，不应继续作为主要优化变量。

### 3.2 Two-stage 当前失败点

Two-stage 已经完成 build/load/search correctness 闭环，但 matched recall 下没有性能优势：

| 方案 | Recall@10 | payload/read 成本 | QPS 结论 |
|---|---:|---:|---|
| Official legacy `ir64` | `0.978319` | `≈486KB/query` | `st8 ≈ 5.9k` |
| Two-stage `TopRGlobal=512` | `0.97678` | `≈488KB/query` | `st1 approx ≈242` |
| Two-stage `TopRGlobal=768` | `0.97925~0.980` | `≈531KB/query` 或旧 st8 `≈389KB/query` 口径 | `st8 ≈1.85k` |

这说明当前瓶颈不是“payload physical bytes 一项”能解释的。更准确的瓶颈链条是：

```text
TopRGlobal 为了补 recall 被迫放大
→ payload candidates 分散在很多 page
→ payload physical pages 没有低于 legacy
→ Fetch Payload & Rerank 阶段占 two-stage phase time 的 73.8%
→ scan/merge/plan CPU overhead 只解释 27.4% ex-latency gap
→ matched recall 下端到端 QPS 明显落后
```

### 3.3 当前保留方向

| 优先级 | 方向 | 状态 | 目的 | 继续条件 |
|---:|---|---|---|---|
| P0 | 官方 strict baseline 参数产品化 | **已完成** | 先拿到可用性能档位 | 固定 `UInt8 + DEFAULT`，围绕 `InternalResultNum` 和 `SearchThreadNum` 做受控选择 |
| P1 | two-stage per-phase cost attribution | **已完成** | 已定位 Fetch Payload & Rerank 为主耗时阶段 | 已进入 P2 子阶段拆分 |
| P2 | Fetch Payload & Rerank 子阶段拆分 | **已完成** | 确认 Payload Read Wait 占 89% phase time | 子阶段合计解释父阶段 `95%` ✓ |
| P3 | query co-hit trace 与 theoretical best packing | **已完成** | 证明理论 page reduction 上限 40%~50% | Train/held-out 一致，远超 10%~15% 阈值 ✓ |
| P4 | co-hit layout v1 (adjacent) | **已完成** | TopR 256/512 pages 降 15%~19% | high-TopR 收益衰减 |
| P4b | co-hit layout v2 (page-aware) | **已完成** | v2 在 256/512 弱正向 | 768 仍仅降 2.99% |
| P4c | co-hit layout v3 (query-set) | **已完成** | 768 提升到 14.92%，但 512 回退到 15.34% | 发现 TopR-level tradeoff |
| P4d | Layout 策略整合 | **已完成-失败** | 权重 sweep 证明 tradeoff 不可调和 | 无统一 layout，需产品决策 |
| P5 | same-head matched 对照 | **部分完成** | P4 same-head 已避免 head 差异污染 layout 结论 | legacy/two-stage 更完整 same-head 仍可补证 |
| P6 | M3 pruning | 暂缓 | 仅在 payload/co-hit 和 attribution 清楚后评估 | 必须有 truth-pruned attribution 或 exact upper-bound 语义 |

### 3.4 剔除或降级方向

- 不再继续盲扫 `PostingChunkTargetSize`。P1.3b chunk-locality 已显示 `unique pages` 和 `payload KB` 反而略升，说明静态 chunk/residual locality 不是有效信号。
- 不再继续调高 `PostingTopRPerPosting`。P2.2 显示 `32 -> 64 -> 96 -> 128` 几乎没有 Recall 收益，只增加 coarse candidate 和 CPU 成本。
- 不把高 `PostingTopRGlobal` 当性能优化。它能提高 Recall，但代价是更多 payload pages 和更低 QPS。
- 不把 payload merge 作为主线。已有结果显示 QPS 变化没有伴随 physical bytes 下降，且有 recall 下降风险。
- 不立即进入 dynamic/KV 路径。static two-stage 的性能瓶颈尚未解释清楚，扩大范围会污染归因。
- 不回到 code async 主线。code physical read 已由 code cache 清零，当前最高价值不在 code read path。

## 4. 下一步实验计划与预期

### 4.1 P0：官方 strict baseline 参数产品化

#### Hypothesis

在当前代码和数据口径下，官方 strict `UInt8 + DEFAULT` baseline 已经是短期最优性能主线；通过 `InternalResultNum` 和 `SearchThreadNum` 可以得到清晰的产品档位，而无需改 two-stage 代码。

#### 成功结果

- `ir64/st4~8` 稳定保持 `Recall@10≈0.978319`，QPS 明显高于 two-stage matched recall 点。
- `ir32` 形成明确低 recall / 高 QPS 档，`ir96` 形成高 recall / 低 QPS 档。
- `NumberOfThreads=16~40` 与 `SearchPostingPageLimit=4~8` 的收益被证明很小，默认配置可以简化。

#### 失败结果

- strict baseline 重跑结果与历史 sweep 大幅漂移，且无法由 cold cache、binary、index 或 config 差异解释。
- `InternalResultNum` 与 Recall/QPS 不再呈单调关系，说明实验口径或配置解析有问题。
- `SearchThreadNum=8` 出现 Recall 漂移或 query-level hash 不稳定。

#### 失败信号

- 同一 run 的 raw log、summary、detailed CSV 中 QPS/Recall 不一致。
- `ir64/pl4` 与 `ir64/pl8` 的 requested bytes 不同但 Recall 完全相同，需要先查 page-limit clamp 或 config 覆盖。
- `st` 变化导致 per-query result hash 改变，但 Recall 表面不变。

#### Ablation 预期

- `InternalResultNum=32 -> 64 -> 96 -> 128`：预期 Recall 上升，QPS 下降，requested bytes 近似线性上升。
- `SearchThreadNum=1 -> 4 -> 8 -> 16`：预期 QPS 到 `st=8` 前上升，`st>=16` QPS 饱和且 latency 恶化。
- `SearchPostingPageLimit=4 -> 8`：预期 Recall/QPS/read bytes 基本不变；若变化明显，优先排查 clamp 或配置口径。

#### P0 验收结论 (2026-05-02)

**成功完成**。参数与指标关系已清晰：
- `st=4~8` 是吞吐有效区间，QPS 可达 `4.7k~5.9k`
- `ir=64~96` 是 Recall/QPS 平衡区间
- `nt` 和 `pl` 不是主要影响因素

详细结果见 `SIFT1M_Official_Alignment_Summary.md`。

### 4.2 P1：two-stage per-phase cost attribution

#### Hypothesis

Two-stage 在 payload KB 接近官方 baseline 时仍慢很多。原始假设是差距可能主要来自 `ScanCompactCodes`、`MergeCoarseCandidates`、`BuildPayloadReadPlan` 等 CPU pipeline overhead；P1 attribution 的目标是验证这个假设是否成立，并定位主要耗时阶段。

#### 成功结果

- 每 query 阶段耗时或 cycles 合计能解释端到端 latency 的 `90%+`。
- 能指出 `ScanCompactCodes`、merge/dedupe、payload read-plan、payload read wait、exact rerank 中的主要耗时阶段。
- matched recall 点上，能量化 two-stage 相比 legacy 的额外成本是否超过 payload I/O 可优化空间。

#### P1 验收结论 (2026-05-02)

**成功完成**。阶段耗时合计解释了 two-stage `Ex Latency` 的 `83.8%`，未达到原先 `90%+` 闭合目标，但已经足够定位主耗时阶段。

| 指标 | Legacy | Two-Stage | Ratio |
|------|--------|-----------|-------|
| Total Latency (ms) | 1.063 | 3.837 | 3.61x |
| Ex Latency (ms) | 0.720 | 3.430 | **4.76x** |
| Batch Read Total (ms) | 0.698 | 2.002 | 2.87x |

**Per-Phase Timing Breakdown:**

| 阶段 | 耗时 (ms) | 占 Phase 比例 |
|------|-----------|---------------|
| Read Header & Directory | 0.011 | 0.4% |
| Scan Compact Codes | 0.368 | 12.8% |
| Merge Coarse Candidates | 0.328 | 11.4% |
| Build Payload Read Plan | 0.046 | 1.6% |
| **Fetch Payload & Rerank** | **2.120** | **73.8%** |

**关键发现:**
1. Fetch Payload & Rerank 是主要瓶颈（占 73.8% phase time）
2. Two-Stage CPU overhead (scan+merge+plan) 仅解释 27% latency gap
3. 原始 hypothesis 被部分否定：CPU overhead 存在但不是主瓶颈
4. 主要瓶颈在 Fetch Payload & Rerank 阶段，需要继续拆分 payload page I/O、I/O wait、payload copy 和 exact distance
5. 下一步 P2 子阶段拆分必要，因为 Fetch Payload & Rerank 的高占比与 candidates/page 低、payload page locality 差一致

#### 失败结果

- 阶段耗时合计与端到端 latency 长期不闭合。
- 主要耗时落在未归类区间，无法指导后续优化。
- 开启 instrumentation 后 Recall、candidate hash 或 QPS 大幅改变，说明测量污染热路径。

#### 失败信号

- `batch_read_total_ms` 小于 payload read 子阶段但总 `ex_latency_ms` 无法解释。
- `DistanceEvaluatedCount` 固定时 exact rerank 时间大幅漂移。
- `PostingTopRPerPosting` 增大导致 coarse candidate 数上升，但 scan/merge 阶段耗时不变，说明统计未覆盖正确路径。

#### Ablation 预期

- `PostingTopRGlobal=128 -> 256 -> 512 -> 768`：预期 payload read-plan、payload read wait、exact rerank 时间上升。
- `PostingTopRPerPosting=32 -> 64 -> 128`：预期 coarse scan/merge 候选处理成本上升，但 Recall 几乎不变。
- `SearchThreadNum=1 -> 8`：预期 per-query CPU 阶段分布相近，I/O wait 和调度竞争可能变化。

### 4.3 P2：Fetch Payload & Rerank 子阶段拆分

#### Hypothesis

`FetchPayloadPagesAndRerank` 内部主成本来自 payload page read wait 和低 candidates/page 造成的 page fanout；payload copy 与 exact distance 是次要成本。只有先拆清这个阶段，才能判断后续应优先做 page locality、I/O batching、copy avoidance 还是 distance kernel 优化。

#### 成功结果

- 能把 `Fetch Payload & Rerank = 2.120 ms` 拆成 payload page read wait、payload buffer/copy、exact distance、result insertion 等子阶段。
- 子阶段耗时合计能解释 `Fetch Payload & Rerank` 的 `90%+`。
- 主要子阶段与 P1.1 locality 指标方向一致，例如 pages 越多，payload read wait 越高。

#### 失败结果

- 子阶段合计与 `Fetch Payload & Rerank` 不闭合，说明计时范围或 async wait 口径错误。
- exact distance 成为主成本，但 `DistanceEvaluatedCount` 与耗时不相关，说明统计或 kernel 路径混乱。
- payload read wait 不随 pages/KB 变化，说明 page locality 不是当前阶段的主导因素。

#### 失败信号

- `payload_read_wait_ms + payload_copy_ms + exact_distance_ms` 大于父阶段耗时。
- `unique_payload_pages` 下降但 payload read wait 不降。
- same query 重复运行时子阶段比例大幅漂移。
- 打开子阶段 instrumentation 后 final result hash 或 Recall 改变。

#### Ablation 预期

- `TopRGlobal=256 -> 512 -> 768`：预期 payload read wait、exact distance 和 result insertion 都上升，其中 read wait 应与 pages/KB 最相关。
- `PostingPayloadBatchPages=1 -> 4 -> 8`：预期 read wait 或 batch overhead 改变，但 Recall 和 candidate hash 不变。
- single-page payload fast path on/off：预期 payload copy 时间变化，若 QPS 不变，说明 copy 不是主瓶颈。
- `SearchThreadNum=1 -> 8`：预期 read wait 受并发和设备队列影响，exact distance per query 基本稳定。

#### P2 验收结论 (2026-05-02)

**成功完成**。子阶段合计解释父阶段 `95.0%`，超过 `90%` 目标。

| 子阶段 | 耗时 (ms) | 占父阶段比例 |
|--------|-----------|--------------|
| **Payload Read Wait** | 2.014 | **89.0%** |
| Exact Distance | 0.106 | 4.7% |
| Result Insertion | 0.016 | 0.7% |
| Payload Copy | 0.014 | 0.6% |

**关键发现:**
1. Payload Read Wait 占 89.0% of Fetch Payload & Rerank
2. Payload Read Wait = 66.8% of Two-Stage 总 phase time
3. Hypothesis 成立：主成本来自 payload page I/O wait，而非 CPU
4. 排除了 exact distance、payload copy、result insertion 作为优化目标
5. 后续应专注于减少 payload pages；P3 已证明 co-hit layout 上限足够，P4/P4b 已进入实际 layout objective 迭代

### 4.4 P3：query co-hit trace 与 theoretical best packing

#### Hypothesis

只有 query-time co-hit 才可能解释 payload page locality。先离线计算理论最优 packing 上限，如果最优情况下 `unique_payload_pages` 也不能下降 `10%~15%`，则 payload layout 路线不值得继续。

#### 当前代码状态 (2026-05-02)

- 已新增 `EnablePayloadTrace`、`PayloadTraceOutput`、`PayloadTraceSampleRate` 三个搜索配置项，默认关闭。
- 已新增 [configs/p3_cohit_trace.ini](/home/ray/code/SPTAG/configs/p3_cohit_trace.ini)，用于在 `TopRPerPosting=64 / TopRGlobal=512 / st=1 / ir=64` 下导出 P3 trace。
- 已新增 [scripts/analyze_payload_cohit_trace.py](/home/ray/code/SPTAG/scripts/analyze_payload_cohit_trace.py)，用于从 trace 计算 current pages、per-query oracle best-packing lower bound、page reduction，以及 trace 与 detailed CSV 的 page-count mismatch。
- 当前 P3 代码只做测量与上限分析，不改变 search 语义，不应改变 Recall、candidate hash 或 final result hash。

#### P3 smoke 结果 (2026-05-02, 20 queries)

`configs/p3_cohit_trace.ini` 小样本 smoke 已跑通，输出路径为 `/tmp/p3_trace_smoke_20260502` 与 `/tmp/p3_cohit_trace/payload_trace.csv`。

| 指标 | 结果 |
|------|------|
| queries | 20 |
| avg candidates | 512.00 |
| avg current pages | 129.55 |
| avg oracle best pages | 61.90 |
| avg oracle page reduction | 51.69% |
| trace/stats mismatch queries | 0 |

解释：这是 per-query oracle lower bound，只证明“若存在足够好的 packing 信号，理论 page reduction 上限很大”；它不是可直接实现的全局 layout 收益。完整 P3 后续已验证 `40%~50%` theoretical page reduction，并触发 P4。

#### P3 完整验收结论 (2026-05-02)

**成功完成**。完整 P3 验证显示 theoretical page reduction 上限为 `40%~50%`，远超 `10%~15%` 进入 P4 的阈值。Train/held-out 方向一致，trace 重算 current pages 与 detailed CSV 口径闭合，因此 P4 `cohit_layout` 原型条件成立。

P3 的结论只能作为上限判断，不能直接等价为实际 layout 收益。P4 same-head 结果已经证明这一点：actual CoHit v1 能稳定吃到一部分 oracle 空间，但不是 per-query oracle 的全局最优实现。

#### 成功结果

- 能导出 global TopR 后每个 query 的 `(queryID, postingID, vectorID, payloadPageID)`。
- 离线 best packing 在 `TopRGlobal=512` 或 matched recall 点显示 `unique_payload_pages` 理论可降至少 `10%~15%`。
- theoretical best 的改善在训练 query 和 held-out query 上方向一致。
- trace 重算的 `current_pages` 与 detailed CSV 的 `unique_payload_pages` 差异不超过 `1%~2%`。

#### 失败结果

- 理论最优 packing 只能带来小于 `10%` 的 page reduction。
- co-hit 关系高度 query-specific，训练 query 有收益但 held-out query 无收益。
- 大部分 page cost 来自跨 posting 分散，posting 内重排无法触达。

#### 失败信号

- trace 中同一 `(postingID, vectorID)` 映射到多个 payload offset 且无法解释。
- current pages 与由 trace 重算的 pages 差异超过 `1%~2%`。
- best packing 显示 pages 可降很多，但 candidates/page 没有同步上升，说明模型或统计有误。

#### Ablation 预期

- `TopRGlobal=256 -> 512 -> 768`：预期 current pages 上升；如果 co-hit 有价值，best/current page ratio 应保持或改善。
- per-query oracle best packing：预期给出 payload layout 的乐观上限；若该上限也小于 `10%`，停止 layout 实现。
- per-posting best packing：预期只能改善同 posting 内分散；若收益很小，说明主要问题是跨 posting fanout。
- train/held-out split：预期有效 co-hit 信号应在 held-out 上仍有 page reduction；若只在 train 有效，判为过拟合。

### 4.5 P4：co-hit layout 原型（有条件执行）

#### Hypothesis

如果 theoretical best packing 有足够空间，则按 query co-hit 重排 posting blob 内 payload 可以降低 actual `unique_payload_pages`，并在不改变 coarse/topR/rerank 语义的情况下提升 matched recall QPS。

#### 当前实现状态 (2026-05-02)

- 已新增 `PostingPayloadLayout=CoHit`，metadata layout name 为 `cohit_payload_v1`。
- 已新增 `PostingCohitOrderFile`，build 阶段读取 offline order file，按 `(postingID, vectorID) -> order_rank` 对 posting 内 payload/code record 重排。
- 已新增 [scripts/build_payload_cohit_order.py](/home/ray/code/SPTAG/scripts/build_payload_cohit_order.py)，从 P3 payload trace 生成 deterministic co-hit order。
- 已新增 [configs/p4_cohit_build.ini](/home/ray/code/SPTAG/configs/p4_cohit_build.ini) 与 [configs/p4_cohit_st1.ini](/home/ray/code/SPTAG/configs/p4_cohit_st1.ini)。
- 已新增 same-head 配置 [configs/p4_cohit_build_samehead.ini](/home/ray/code/SPTAG/configs/p4_cohit_build_samehead.ini) 与 [configs/p4_cohit_samehead_st1.ini](/home/ray/code/SPTAG/configs/p4_cohit_samehead_st1.ini)，用于避免重新 build head 后 postingID/vectorID 与 P3 trace/order 不匹配。
- 当前 P4 原型只改变 posting blob 内物理顺序，不改变 coarse scan、TopR、rerank 或 exact distance 语义。

#### P4 same-head 验收结果 (2026-05-02)

第一次 CoHit build 重新生成了 head，导致 P3 trace/order 的 postingID 对照不再严格匹配，因此不能用于 layout 收益结论。有效结论必须使用 same-head CoHit：从 `/home/ray/data/sift1m/spann_index_m2_u8default` 复制原始 head 到 CoHit 目录，只重建 SSD posting。

有效结果文件：

- baseline: [summary_metrics.tsv](/home/ray/code/SPTAG/results/io_analysis/p2_topr_sweep_20260502_topk_v2/summary_metrics.tsv)
- same-head CoHit: `/tmp/p4_cohit_layout_samehead/st1_eval/summary_metrics.tsv`

| TopRGlobal | Baseline pages | CoHit pages | Page reduction | Baseline payload KB | CoHit payload KB | QPS uplift | Recall delta |
|------------|----------------|-------------|----------------|---------------------|------------------|------------|--------------|
| 256 | 94.9602 | 80.5127 | **15.22%** | 379.8408 | 322.0508 | **19.28%** | +0.00010 |
| 512 | 121.9773 | 99.3466 | **18.55%** | 487.9092 | 397.3864 | **17.57%** | +0.00010 |
| 768 | 132.7546 | 128.8844 | **2.92%** | 531.0184 | 515.5376 | **4.58%** | -0.00028 |

结论：

- P4 v1 在 `TopRGlobal=256/512` **成功**：pages/KB 明确下降超过 `10%~15%` 阈值，QPS 提升方向与 page reduction 一致，Recall 未出现实质下降。
- P4 v1 在 `TopRGlobal=768` **弱成功/不充分**：方向仍为正，但 page reduction 只有 `2.92%`，不能解释 P3 oracle 的 `49.7%` 上限。
- 当前瓶颈仍是 payload page fanout，但现在已经证明 build-side payload reorder 能有效降低实际 I/O；下一步不应回到 CPU/copy/exact distance，而应改进 CoHit order 目标函数并做并发验证。
- P4 actual 收益低于 P3 oracle 是预期现象：P3 是 per-query oracle lower bound，P4 v1 是单一全局 order，且当前 order 更接近局部相邻 co-hit，不是 page-aware bin packing。

#### 成功结果

- `PayloadPhysicalBytesRead` 或 `PagesRead` 相比 baseline layout 下降至少 `10%~15%`。
- `avg(Candidates/Page)` 至少相对提升 `20%`。
- matched recall 下 QPS 至少提升 `10%+`，且提升方向与 pages/KB 下降一致。
- Recall 绝对下降不超过 `0.001~0.002`，query-level payload hash 和 exact distance 校验一致。

#### 失败结果

- layout 重排后 pages/KB 基本不变。
- pages 降了但 QPS 不升，说明瓶颈转向 coarse scan、merge/dedupe、rerank 或调度。
- Recall 出现不可解释下降，说明 page contract 或 payload mapping 被破坏。
- held-out query 没有 locality 收益，说明 co-hit layout 过拟合。

#### 失败信号

- payload byte hash 或 exact distance 校验不一致。
- query-level payloadPageHash 稳定但 final result hash 异常漂移。
- 单线程收益存在但 `st=8` 收益消失，说明并发调度或 I/O 饱和吞掉收益。
- 重复构建同一 layout 的 record order 不稳定。

#### Ablation 预期

- `baseline_layout -> pq_code_layout`：预期仍接近 P1.3a，locality 几乎不变；若突然大幅改善，优先查是否混入其他参数。
- `baseline_layout -> chunk_locality_layout`：根据已完成结果，预期不会改善 pages/KB；若改善，需要用 same-head 复测确认。
- `baseline_layout -> cohit_layout`：预期 pages/KB 下降，candidates/page 上升；若只 QPS 上升但 pages 不降，说明不是 layout 收益。
- `TopRGlobal=256/512/768`：P4 v1 实测在 256/512 有效、768 收益衰减；P4b 预期应改善 768，而不是只强化低 TopR。

#### P4 执行命令

```bash
scripts/build_payload_cohit_order.py \
  --trace /tmp/p3_cohit_trace/payload_trace.csv \
  --output /tmp/p4_cohit_layout/cohit_order.tsv

./Release/ssdserving configs/p4_cohit_build.ini

scripts/run_spann_p2_topr_sweep.sh \
  -c configs/p4_cohit_st1.ini \
  -o /tmp/p4_cohit_layout/st1_eval \
  -r 64 \
  -g 256,512,768 \
  -t 1 \
  --no-monitor
```

#### P4b 已完成：page-aware CoHit v2 与并发验证

##### Hypothesis

CoHit v1 的主要限制不是 payload reorder 方向错误，而是 order 目标函数太弱：它把 co-hit 关系转成单一排序，但没有显式优化 4KB page bin 内的 query co-hit 覆盖率。高 `TopRGlobal=768` 下候选 fanout 更宽，局部相邻 co-hit 排序被更多长尾候选稀释，因此 actual page reduction 从 `18.55%` 下降到 `2.92%`。

##### 当前实现状态

- [scripts/build_payload_cohit_order.py](/home/ray/code/SPTAG/scripts/build_payload_cohit_order.py) 已新增 `--strategy page-aware`，旧默认 `adjacent` 策略保持不变。
- `page-aware` 策略按 posting 维护 query co-hit 集合，并贪心填充 4KB page，使同一 page 内优先放入与已选记录共同出现的 vector。
- 已新增 [configs/p4_cohit_pageaware_build_samehead.ini](/home/ray/code/SPTAG/configs/p4_cohit_pageaware_build_samehead.ini) 与 [configs/p4_cohit_pageaware_samehead_st1.ini](/home/ray/code/SPTAG/configs/p4_cohit_pageaware_samehead_st1.ini)。
- 该实现只改变 order file 生成，不改变 search、TopR、payload fetch 或 exact rerank 语义。

##### 成功结果

- `TopRGlobal=512` page reduction 保持不低于 P4 v1 的 `18%`，QPS uplift 保持 `15%+`。
- `TopRGlobal=768` page reduction 从 `2.92%` 提升到至少 `10%`，stretch target 为 `15%+`。
- `candidates_per_payload_page` 在 512/768 同步上升，且 `PagesRead`、`payload_kb`、`batch_read_total_ms` 同方向下降。
- `st=8` 下 Recall 与 query-level hashes 稳定，QPS uplift 仍能观察到；若设备队列饱和导致 uplift 缩小，也应看到 per-query pages/KB 继续下降。

##### 失败结果

- page-aware order 在 512/768 的 pages/KB 与 CoHit v1 基本相同，说明当前全局 reorder 已接近可实现上限。
- 768 pages 降低但 Recall 或 final hash 异常漂移，说明 build-side record mapping 或 payload offset contract 被破坏。
- 512 收益下降而 768 收益上升，说明目标函数过度偏向高 TopR，不能作为默认 layout。
- st=1 有 page reduction，但 st=8 的 payload read wait 不降且 disk/batch wait 上升，说明并发下 I/O 调度或设备饱和吞掉收益。

##### 失败信号

- 同一 `(postingID, vectorID)` 在 order file、build log、trace 中映射不一致。
- `PagesRead` 下降但 `PayloadPhysicalBytesRead` 不降，且不是由 tail-page padding 或统计口径解释。
- `payloadPageHash` 或 final result hash 在 same config 重跑间不稳定。
- `candidates_per_payload_page` 不升但 pages 下降，说明统计或 page dedupe 口径可能错误。
- `TopRGlobal=768` 的 `total_payload_page_spans` 上升幅度显著超过 candidates 增幅，说明 page-aware packing 产生了新的碎片。

##### Ablation 预期

- `cohit_v1 -> page_aware_cohit_v2`：预期 512 持平或小幅改善，768 明显改善；若 256 改善而 768 不变，说明仍在优化短 query 局部性。
- `train_order -> heldout_eval`：预期 held-out pages/KB 仍下降；若只在 train 有效，判为 trace overfit。
- `page_capacity=4KB effective payload slots` 不同 packing penalty：预期更强 page-fill penalty 会提升 candidates/page，但过强可能伤 512 locality。
- `TopRGlobal=256/512/768`：预期 256 收益不应明显回退，512 是主验收点，768 是判断 v2 是否解决 fanout 饱和的关键点。
- `SearchThreadNum=1 -> 8`：预期 Recall/hash 不变；payload pages 是 query-level 指标，应基本不随并发变化，QPS uplift 可能因设备饱和小于 st=1。

##### P4b 已执行命令（历史复现）

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

##### P4b v2 首轮结果 (2026-05-02)

page-aware v2 已完成 full trace order 生成、same-head SSD-only build、`st=1` 和 `st=8` sweep。build 侧成功加载 `/tmp/p4_cohit_layout_pageaware/cohit_order.tsv`，共 `2,706,632` rows、`128,814` postings。

`st=1` 对比结果：

| TopRGlobal | Baseline pages | CoHit v1 pages | Page-aware v2 pages | v2 vs baseline | v2 vs v1 | v2 QPS uplift vs baseline | v2 QPS uplift vs v1 | Recall |
|------------|----------------|----------------|---------------------|----------------|----------|---------------------------|--------------------|--------|
| 256 | 94.9602 | 80.5127 | 79.5762 | **16.20%** | 1.16% | **19.72%** | 0.37% | 0.96204 |
| 512 | 121.9773 | 99.3466 | 98.4516 | **19.29%** | 0.90% | **20.40%** | 2.41% | 0.97678 |
| 768 | 132.7546 | 128.8844 | 128.7897 | **2.99%** | 0.07% | **4.94%** | 0.35% | 0.97897 |

`st=8` 复核：

- Recall 与 `st=1` 一致，未复现并发 recall 漂移。
- query-level pages/KB 与 `st=1` 完全一致，说明 layout 指标不受并发影响。
- `approx_single_thread_qps` 在 `st=8` 下降，不能直接当总吞吐结论；该 sweep 主要用于验证并发语义与 page 指标稳定。

判定：

- P4b v2 **未达到主要成功标准**：`TopRGlobal=768` page reduction 仍只有 `2.99%`，没有达到 `10%+`。
- P4b v2 **保留为弱正向 ablation**：256/512 相比 v1 小幅改善，且没有破坏 Recall 或 page/hash 口径。
- 失败信号不是 mapping/correctness，而是 objective 不足：简单 page-fill 贪心仍没有解决 high-TopR fanout。

下一步不应继续微调当前 page-fill 贪心，而应改为 query-set/page-objective：

- 以 query coverage 为目标，而不是只累计与当前 page 已选 vector 的局部 pair co-hit。
- 对每个 posting 构造 query bitset 或压缩 query-id 列表，选择一页 vector 时最大化该 page 覆盖的 query-hit weight。
- 对 `TopRGlobal=768` 单独优化或混合 512/768 trace 权重，避免目标函数只适配低 TopR。
- 成功标准保持不变：512 不低于 v1，768 page reduction 达到 `10%+`，Recall/hash 稳定。

#### P4c 已完成：query-set/page-objective CoHit

##### 当前进度位置

现在已经完成从 P1 到 P4c 的诊断闭环：

- P1/P2 已证明主瓶颈是 payload page I/O wait，不是 CPU/copy/exact rerank。
- P3 已证明理论 layout headroom 足够，oracle page reduction 为 `40%~50%`。
- P4 v1 已证明 build-side reorder 可落地产生实际收益，但 768 收益弱。
- P4b v2 已证明简单 page-fill 贪心不能解决 768 fanout；并发语义稳定，不是当前 blocker。
- P4c v3 已证明 query-set objective 能把 768 page reduction 提升到 `14.92%`，但会把 512 从 `19.29%` 拉回到 `15.34%`。

因此当前所处阶段已经不是 P4c 实现前，而是 **P4d layout 策略整合前**。接下来重点不是继续证明 query-set objective 是否有效，而是解决不同 TopR 档位的 layout tradeoff。

##### Hypothesis

CoHit v1/v2 在 768 失败的原因是目标函数过局部：v1 最大化相邻 pair co-hit，v2 贪心填充当前 page，但都没有直接优化“一个 page 能服务多少 query 的 TopR 命中集合”。如果改为 query-set/page-objective，即每次构造 page 时最大化该 page 对 query 集合的覆盖权重，则 high-TopR 下分散候选应能更集中到少数 page。

##### 成功结果

- `TopRGlobal=512`：原目标是不低于当前 v2 的 `19.29%`，或至少不低于 v1 的 `18.55%`；实际为 `15.34%`，未达标。
- `TopRGlobal=768`：原目标是从 `2.99%` 提升到 `10%+`；实际为 `14.92%`，达标。
- Recall 绝对变化不超过 `0.001`，query-level payload/final hash 无异常漂移。
- `candidates_per_payload_page` 在 768 明显上升，且 `PagesRead`、`payload_kb`、`batch_read_total_ms` 同方向下降。
- 若 `st=8` QPS uplift 受设备队列影响缩小，pages/KB 仍必须保持与 `st=1` 一致下降。

##### P4c 判定

P4c **部分成功**：

- 成功点：query-set objective 证明 high-TopR fanout 不是不可优化，768 从 `≈3%` 提升到 `14.92%`。
- 失败点：同一 layout 对 512 明显回退，说明当前 768-heavy objective 不能直接作为默认 layout。
- 核心发现：不同 `TopRGlobal` 档位的最优 locality layout 不完全兼容，后续问题从“如何改善 768”转为“如何在 512/768 之间找到 Pareto 最优或产品化选择”。

##### 失败结果

- 512 和 768 的 pages/KB 与 v2 基本相同，说明当前可实现全局 order 已接近上限，P3 oracle 主要不可实现。
- 768 改善但 512 明显回退，说明目标函数过度偏向 high-TopR，不能作为默认 layout。
- query-set objective 在 train trace 上有效但 held-out 或 full query eval 无效，说明 co-hit 关系过拟合。
- pages 降低但 batch read wait 不降，说明 I/O 调度或 page request 形态成为新瓶颈。

##### 独立失败信号

- order file 中同一 posting 的 vector 数与 P3/P4 记录数不一致。
- `PagesRead` 下降但 `payload_kb` 不降，且无法由 tail padding 或统计口径解释。
- `payloadPageHash` 稳定但 final hash 或 Recall 异常漂移。
- `total_payload_page_spans` 明显上升，说明 query-set packing 产生新的跨页碎片。
- 768 的 `candidates_per_payload_page` 不升反降，即使 pages 表面下降，也应先排查统计或映射。

##### Ablation 预期

- `cohit_v2 -> query_set_v3`：预期 512 持平，768 明显改善；如果 256/512 改善而 768 不变，目标仍未覆盖 high-TopR fanout。
- `512_trace_order -> 768_eval`：预期 768 改善有限，用于证明单一低 TopR trace 不够。
- `768_trace_order -> 512_eval`：预期 768 改善，但可能伤 512，用于观察目标偏置。
- `mixed_512_768_weight`：预期能在 512 不回退的前提下改善 768，是最可能成为默认的策略。
- `train -> heldout/full eval`：预期 held-out 仍有 page reduction；若收益只在 train 出现，停止该目标函数。

#### P4d 已完成：layout 策略整合与 Pareto 选择

##### Hypothesis

P4c 的 tradeoff 最初假设来自 768 权重过强，但 P4d 真实 same-head 权重 sweep 已否定这一点：`w=0.75~1.5` 的差异在 `1%` 以内且无单调趋势。更准确的结论是，trace coverage 是主导因素，而不是权重参数本身。

##### 为什么先做离线 evaluator

每次 same-head SSD rebuild 成本高，因此 P4d 先尝试离线 evaluator。但 evaluator 预测误差 `16%~32%`，不能替代真实 build/search；最终 P4d 判定来自真实 same-head build/search 权重 sweep。

##### P4d 当前进展 (2026-05-02)

已完成：

- 已实现 [scripts/evaluate_payload_order.py](/home/ray/code/SPTAG/scripts/evaluate_payload_order.py) 和 [scripts/evaluate_payload_order_v2.py](/home/ray/code/SPTAG/scripts/evaluate_payload_order_v2.py)。
- 已发现离线 evaluator 对真实 page 指标预测误差为 `16%~32%`，无法达到原定 `1%~2%` 误差目标。
- evaluator 对 768 的 layout 排名预测错误：预测方向与真实 search 不一致，不能作为 Pareto 筛选器。
- evaluator 揭示了关键覆盖率差异：v1 order 只覆盖 `78.6%` 的 768 trace vectors，而 v3 order 覆盖 `100%`。
- 已生成并验证 768 权重为 `0.75/1.0/1.25/1.5` 的 query-set order 文件。

当前判定：

- 离线 evaluator **不能用于准确预测真实 pages**，也不能作为是否 build 的筛选器。
- 离线 evaluator **仍有诊断价值**：它能暴露 order 文件覆盖率、trace 来源差异、以及某个 layout 是否根本没有覆盖高 TopR 命中的 vectors。
- v3 在 768 上的改进主要来自使用包含 768 的 combined trace，而不应全部归因于 query-set objective 本身。
- 因此 P4d 最终判定必须来自真实 same-head build/search；离线工具只用于覆盖率诊断和 sanity check。

##### P4d 权重 sweep 最终结果 (2026-05-02)

| Layout / Trace | 512 Page Reduction | 768 Page Reduction | 结论 |
|----------------|--------------------|--------------------|------|
| v1/v2, 512-only trace | `18%~19%` | `2%~3%` | 适合默认 TopR=512 |
| v3 / combined trace | `≈15%` | `≈15%` | 适合需要支持多 TopR |
| w0.75~w1.5 combined trace | `≈15%~16%` | `≈14%~15%` | 权重影响小，无 Pareto 点 |

关键发现：

- 权重参数影响极小：`w0.75~w1.5` 的差异在 `1%` 以内，没有单调趋势。
- Trace coverage 是主导因素：512-only trace 对 768 覆盖不足，因此 768 只有 `2%~3%`；combined trace 覆盖 768，因此 768 可到 `≈15%`。
- 不存在统一 Pareto layout：当前证据下无法同时满足 `512>=18%` 和 `768>=10%`。

##### P4 系列最终状态

- P4 v1：证明 build-side reorder 在 512 有效。
- P4b v2：验证 page-aware 策略，差异小。
- P4c v3：改善 768 但导致 512 回退，发现 TopR tradeoff。
- P4d：通过真实权重 sweep 证明 tradeoff 不可调和，统一 layout 失败。

##### 验收判定

P4d 判为 **完成，统一 layout 失败**：

- 成功完成真实 same-head build/search 权重 sweep。
- 成功证明权重参数不是解决 tradeoff 的有效旋钮。
- 成功给出产品化选择边界。
- 未找到同时满足 `512>=18%` 和 `768>=10%` 的 unified layout。

##### 产品化选择

| 方案 | 512 Page Reduction | 768 Page Reduction | 适用场景 |
|------|--------------------|--------------------|----------|
| v1/v2 (512-only) | `18%~19%` | `2%~3%` | 默认 TopR=512 |
| v3 (combined) | `≈15%` | `≈15%` | 需要支持多 TopR |
| 参数化多索引 | 各自最优 | 各自最优 | 运维成本高 |

##### 独立失败信号

- 同一个 order file 离线预测 pages 下降，但真实 `unique_payload_pages` 不降。
- 512/768 的 Recall 或 final hash 出现与 layout 无关的漂移。
- `PayloadPhysicalBytesRead` 与 `unique_payload_pages` 不再保持 4KB/page 的一致关系。
- 权重 sweep 的结果不单调且无法由 trace 覆盖差异解释。
- 某个候选 order 对目标 TopR trace vector 覆盖率明显低于 `95%`，但仍被用于真实 build/search。

##### 后续方向

- 不继续调 CoHit 权重或 page-fill objective。
- 如果产品默认 TopR=512，选择 v1/v2 作为默认 layout。
- 如果必须支持 512/768 混合 TopR，选择 v3 combined layout，接受 512 从 `19%` 回退到 `15%`。
- 如果必须同时拿到各自最优，只能考虑参数化多索引，并单独评估磁盘、构建和运维成本。

### 4.6 P5：same-head matched 对照

#### Hypothesis

当前 P1.2 结论已经足以说明 two-stage 无性能优势，但 same-head 对照可以进一步区分 head build 差异与 posting pipeline 差异。

#### 成功结果

- legacy 与 two-stage 能从同一 head build 派生，或有 sidecar/meta 证明 head 完全一致。
- same-head 下仍复现“payload/read 成本相近但 two-stage QPS 明显更低”的结论。
- Recall gap 能由 TopR/PQ attribution 或 head contribution 解释。

#### 失败结果

- 无法构造 same-head 对照，导致质量归因只能停留在 matched-recall 层面。
- same-head 后结论方向明显改变，说明此前混入了 head build 差异。
- same-head 构建改变了其他参数，使对照不再单变量。

#### 失败信号

- 两个 index 目录不同且没有 head checksum/build log/meta 证据。
- same-head 与非 same-head 的 query-level head result hash 方向相反。
- matched recall 点上 payload/page 指标反直觉变化但没有配置差异解释。

#### Ablation 预期

- same-head legacy `ir64` vs two-stage `TopRGlobal=512/768`：预期 Recall 接近时 payload pages 仍接近，two-stage QPS 仍低。
- same-head two-stage TopR sweep：预期 `TopRGlobal` 上升带来 Recall 上升和 payload cost 上升。
- 如果 same-head 消除大部分 Recall gap：预期 P2 not-observed bucket 下降，但不应自动改善 payload locality。

### 4.7 P6：M3 pruning 重做（暂缓）

#### Hypothesis

M3 pruning 只有在 payload layout、co-hit 上限和质量 attribution 都清楚后才值得继续；否则 chunk/pruning 会把 recall 损失、page locality 和 coarse quality 混在一起。

#### 成功结果

- `PostingChunkPruneMode=None` 下先有稳定 chunk baseline。
- heuristic pruning 能输出 truth-pruned attribution。
- safe pruning 必须使用 exact rerank upper bound，而不是 head result threshold。

#### 失败结果

- pruning 降低 logical work 但不降低 physical pages。
- pruning 提升 QPS 但 truth-pruned attribution 显示不可接受的 recall 损失。
- chunk radius/centroid 指标与 recall/QPS 无相关性。

#### 失败信号

- prune ratio 上升但 payload pages 不降。
- Recall 下降但无法定位被 prune 的 truth chunk。
- 同一 query 在 `None` 与 pruning 模式下 final hash 漂移无法解释。

#### Ablation 预期

- `PostingChunkPruneMode=None -> HeuristicL2`：预期 pages 或 candidates 下降，但 Recall 有可观风险。
- chunk target size sweep：预期 chunk 更细可能提高 prune ratio，但也增加 metadata/chunk management 和 recall 风险。
- safe upper-bound pruning：预期 Recall 不降；若 QPS 没收益，说明可安全剪掉的 chunk 太少。

## 5. 独立的全局失败信号

- 同一实验的 raw log、summary、detailed CSV 指标来源不一致。
- query-level hash 不稳定，但 Recall/QPS 表面正常。
- `PayloadPhysicalBytesRead`、`PagesRead`、`unique_payload_pages` 三者方向不一致且没有 page-limit/clamp 解释。
- 某个 ablation 的 QPS 提升没有伴随任何可解释的 bytes/pages、CPU/cycles 或 scheduling 指标变化。
- Recall 下降被归因于“参数 tradeoff”，但 P2 attribution bucket 没有支持该解释。
- payload reorder 后 exact distance 或 payload byte hash 不一致。

## 6. 总体判断

当前推荐路线是：

```text
短期性能主线：official strict UInt8+DEFAULT baseline
  → 用 InternalResultNum 选择 Recall/QPS 档位
  → 用 SearchThreadNum=4~8 选择吞吐/延迟档位

two-stage 研究线：先诊断，再改 layout
  → per-phase cost attribution（已完成：Fetch Payload & Rerank 主导）
  → Fetch Payload & Rerank 子阶段拆分（已完成：Payload Read Wait 主导）
  → query co-hit trace / theoretical best packing（已完成：理论上限 40%~50%）
  → co-hit layout 原型（st=1 same-head 部分成功：TopR 256/512 有效，768 弱）
  → page-aware CoHit v2 + st=8 验证（已完成：弱正向但 768 未达标）
  → query-set/page-objective CoHit v3（已完成：768 达标但 512 回退）
  → layout 策略整合 / Pareto 选择（已完成：无统一 layout）
  → 产品化选择（当前下一步）

暂缓/剔除：
  → 不盲扫 chunk size
  → 不继续堆 PostingTopRPerPosting
  → 不把高 PostingTopRGlobal 当性能优化
  → 不把 payload merge、code async、dynamic/KV 放入当前主线
```

---

## P1 Two-Stage Posting 测试结果 (2026-05-02)

### 构建问题解决

**问题 1：命令行参数格式错误**
- `ssdserving` 不支持 `-c` 选项，直接传递配置文件路径
- 错误用法：`./Release/ssdserving -c config.ini`
- 正确用法：`./Release/ssdserving config.ini`

**问题 2：INI 注释格式错误**
- IniReader 只支持 `;` 开头的注释，不支持 `#`
- 使用 `#` 注释会导致 `ReadIni_FailedParseParam` 错误

### 索引构建成功

```
IndexDirectory: /home/ray/data/sift1m/spann_index_m2_u8default
Meta: FormatVersion=2, LayoutType=twostage_v1, CodeType=PQ
```

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

### 关键发现

1. **Recall 可通过 TopRGlobal 调优** - 增加 TopRGlobal 可以提高 Recall，但会降低 QPS
2. **Two-stage QPS 较低** - QPS 约 1850，比 legacy 5889 低约 68%
3. **Payload 物理页读取量相似** - 约 389 KB/query，与 legacy 相当
4. **并发稳定性良好** - st=1 和 st=8 Recall 完全一致 (0.980)

### 修正后的阶段判断

这组结果说明：

- **two-stage build/load/search 闭环已经打通**；
- **P0 并发稳定性问题不是当前主因**；
- **P1 还不能标记为完成**，因为 P1 的目标不是“能构建 two-stage 且 Recall 可调上来”，而是“在 matched recall 下真正压低 payload physical pages，并把这部分下降转化为 QPS 提升”；
- 当前结果更准确地说是：**P1 blocker 已被定位，但 P1 主优化尚未发生。**

决定性证据是：

- two-stage 在 `TopRGlobal=768` 时 Recall 已接近/略高于 legacy；
- 但 `PayloadPhysicalBytesRead` 仍约 `389 KB/query`，与 legacy 基本相同；
- 因此当前 QPS 差距不能只归因于 metadata/code 路径，也不能只归因于 payload bytes；P1 attribution 已进一步定位：**主要耗时集中在 Fetch Payload & Rerank，scan/merge/plan CPU overhead 是次要项**。

### 为什么 P1 现在性能不理想

#### 原因 1：payload physical I/O 根本没有降下来

当前观察到的是：

- legacy: `PayloadPhysicalBytesRead ≈ 388 KB/query`
- two-stage: `PayloadPhysicalBytesRead ≈ 389 KB/query`

这意味着 two-stage 虽然减少了部分逻辑候选和 exact rerank 规模，但并没有减少最终必须触达的 payload 物理页数。P1 计划里定义的主目标其实还没被击中。

#### 原因 2：two-stage 额外增加了一整段查询流水线成本

当前 two-stage 路径不是 legacy 的轻量替换，而是额外引入了：

- compact code 扫描；
- coarse candidate merge/dedupe；
- payload read plan 构建与排序；
- payload page fetch；
- exact rerank。

如果 payload 物理页没有减少，这些阶段就会变成纯新增开销，因此即使 Recall 达标，QPS 仍会明显低于 legacy。

#### 原因 3：当前 Recall 是靠放大 `PostingTopRGlobal` 换回来的

从 sweep 结果看：

- `TopRGlobal=256` 时 `Recall@10=0.962`，`QPS=344`
- `TopRGlobal=512` 时 `Recall@10=0.977`，`QPS=288`
- `TopRGlobal=768` 时 `Recall@10=0.980`，`QPS=266`

这表明 coarse 阶段当前还不够“便宜且准”。为了补 Recall，只能扩大 rerank 输入集合，而这会进一步推高 payload 读取和最终计算成本。

#### 原因 4：当前做掉的是 query-side 常数项优化，不是 build-side locality 改造

本轮代码修复和优化已经有效，但主要集中在：

- workspace/buffer/hash-map 复用；
- payload request span 缓存；
- single-page payload 免 scratch 拷贝；
- stats/hash/并发确定性补强。

这些改动可以降低实现噪声和常数项成本，但不会改变“topR 候选分布在多少个 payload page 上”这个决定性问题。只要 layout 不变，payload physical page inflation 就大概率不会本质改善。

#### 原因 5：legacy/two-stage 质量归因仍缺少完整 same-head matched-recall 对照

现在 two-stage 与 legacy 的 Recall/QPS 对比已经足以证明“P1 尚未完成”，但要解释 Recall gap 来自 PQ/topR 还是 head build 差异，仍需要相同 head index、相同查询集、matched recall 的严格对照。这个问题会影响质量归因，但不会推翻“payload 物理页没降，所以 QPS 很难上去”这个主结论。

补充：P4 CoHit layout 结论已经使用 same-head 方式修正，避免了 P3 trace/order 与重新生成 head 后 postingID 不匹配的问题；这里剩余的是 legacy 与 two-stage 之间更完整的质量归因补证。

### 接下来该做什么

截至当前结果，P1.1、P1.2、P1.3a、P1.3b chunk-locality、P1 per-phase attribution、P2.1、P2.2、P3、P4 v1、P4b v2 和 P4c v3 均已有结论。下一步不再重复这些阶段，而是按以下顺序推进：

1. **官方 strict baseline 参数产品化**：先固定可用性能档位，避免把 two-stage 研究线误当默认性能线。
2. **P4d layout 策略整合**：当前主线，先做离线 evaluator 和 512/768 权重 Pareto sweep。
3. **same-head CoHit st=8 复核**：只对 P4d 筛出的 Pareto 候选做确认性复核。
4. **legacy/two-stage same-head 质量补证**：补强 P1.2 归因，但不作为 P4 layout 有效性的前置条件。
5. **M3 pruning 重做（暂缓）**：只有在 payload layout 与 quality attribution 都可解释后再推进。

### P1.1 locality instrumentation

#### Hypothesis

当前 QPS 低的主因不是 metadata/code，也不是并发问题，而是 coarse topR 命中的候选在 payload layout 上过于分散，导致：

- `uniquePayloadPages/query` 偏高；
- `candidatesPerPayloadPage` 偏低；
- `payloadPagesPerPosting` 偏高；
- `payloadTailWasteBytes` 偏高。

#### 要新增/导出的指标

- `uniquePayloadPages/query`
- `payloadCandidates/query`
- `candidatesPerPayloadPage`
- `payloadPageReuseRatio`
- `payloadPagesPerPosting`
- `payloadTailWasteBytes`
- 每 query top postings by payload pages
- 每 posting 的 page span 连续性摘要

#### 成功结果

- 能稳定证明 physical bytes 大主要来自 page 分散，而不是统计口径或重复读计数错误；
- same config 下，query-level 指标与 `PayloadPhysicalBytesRead/PagesRead` 的变化方向一致；
- 能从日志中明确看到哪些 posting/chunk 在拖高 payload page 数。

#### 实验结果 (2026-05-02)

| TopRGlobal | Unique Pages (avg) | Candidates (fixed) | avg(Candidates/Page) | Postings | Page Spans | Recall |
|------------|-------------------|-------------------|----------------------|----------|------------|--------|
| 256 | 95 | 256 | **2.75** | 55 | 260 | 0.962 |
| 768 | 133 | 768 | **5.94** | 63 | 780 | 0.980 |

**注意**：
- `Candidates` 是固定值（等于 `PostingTopRGlobal`），每个 query 都相同
- `Unique Pages` 是所有 query 的均值
- `avg(Candidates/Page)` 是先计算每个 query 的 `Candidates ÷ Unique Pages`，再取均值
- 因此 `avg(Candidates/Page) ≠ Candidates ÷ avg(Unique Pages)`
- 例如 TopRGlobal=256 时：`256 ÷ 95 = 2.69`，但 `avg(Candidates/Page) = 2.75`

**关键发现**：
- `Candidates Per Payload Page` 仅 2.75 ~ 5.94，远低于理论最优值
- 这验证了 hypothesis：候选在 payload layout 上高度分散
- 增加 TopRGlobal 时 `Candidates/Page` 略有提升，但仍远低于目标

**结论**：P1.1 **成功**，已确认 payload physical page inflation 的根因是 locality 差

#### 失败结果

- 指标表明 page 分散并不严重，`candidatesPerPayloadPage` 已经很高，但 physical bytes 仍然很大；
- 或者发现主要问题不是 page 分散，而是大量跨页 payload、重复 page 去重失效、或请求批次/对齐策略本身带来的额外成本。

#### 失败信号

- 新增指标在多轮 benchmark 间剧烈抖动，无法稳定复现；
- `uniquePayloadPages/query` 与 `PagesRead` 明显不一致；
- query hash 稳定，但 locality 指标与最终物理读量没有相关性；
- 指标导出后发现统计定义自相矛盾，不能支持后续决策。

#### Ablation 预期

- `PostingTopRGlobal` 从 `256 -> 512 -> 768`：
  - 预期 `payloadCandidates/query` 上升；
  - 预期 `uniquePayloadPages/query` 上升或至少不降；
  - 若 `candidatesPerPayloadPage` 基本不变，说明增加的候选仍然是分散命中；
  - 若 `candidatesPerPayloadPage` 上升明显但 `PagesRead` 不降，说明可能存在跨页 payload 或批读策略问题。
- `SearchThreadNum` 从 `1 -> 8`：
  - 预期 query-level locality 统计分布基本一致；
  - 若 locality 指标在不同并发下明显漂移，说明还有隐藏的非确定性或统计污染。

### P1.2 matched-recall 对照

#### Hypothesis

当前 two-stage 的 recall/QPS 曲线里，存在两部分混合效应：

- 一部分来自 coarse PQ/topR 截断；
- 一部分来自 head build 或索引构建口径不一致。

需要在 matched recall 下比较，把”质量代价”和”payload I/O 代价”拆开。

#### Head 一致性说明

**当前实验未严格满足 same-head 条件**：
- Legacy 使用 `spann_index_official_u8default_20260430`
- Two-stage 使用 `spann_index_m2_u8default`

两个索引目录不同，head build 是否一致尚未验证。因此结论应理解为”matched-recall 对照”而非”same-head matched-recall 对照”。后续需要补充 head build 一致性验证。

#### 实验设计

- 使用同一个 head index 或从同一构建产物派生的 legacy / two-stage 版本；
- 固定查询集、truth、线程和 detailed stats 口径；
- 对 two-stage 做 `PostingTopRGlobal` sweep；
- 对 legacy 做 `InternalResultNum` sweep；
- 找到 Recall 接近的点再比 QPS、`PayloadPhysicalBytesRead`、`PagesRead`、`DistanceEvaluatedCount`。

#### 成功结果

- 能找到 matched recall 点；
- 在 matched recall 下，明确知道 two-stage 落后主要是 payload pages 没降，还是 coarse/rerank 成本偏高；
- 能给出”two-stage 若要作为默认模式，还缺什么”的清晰判断。

#### 实验结果 (2026-05-02)

| 配置 | QPS (st=1) | QPS (st=8) | Recall@10 | Physical Bytes | Pages Read |
|------|------------|------------|-----------|----------------|------------|
| Legacy (official index) | 943 | **5770** | 0.978 | 486 KB | 119 |
| Two-stage (TopRGlobal=512) | 285 | **1966** | 0.977 | 500 KB | 122 |

**注意**：两个索引目录不同（`spann_index_official_u8default_20260430` vs `spann_index_m2_u8default`），head build 是否一致未验证。结论应理解为"matched-recall 对照"，非严格 same-head。

**关键发现**：
1. Legacy QPS 是 Two-stage 的 **3 倍**
2. 物理读取量几乎相同
3. Two-stage 额外开销来自 code 解码和 coarse 处理

**结论**：符合”失败结果”描述 - 当前 two-stage 在现有 layout 上没有性能优势

#### 失败结果

- 无法得到可比的 matched recall 点；
- 或 matched recall 下 two-stage 的 payload physical pages 仍与 legacy 基本相同，但总成本更高，说明当前设计在现有 layout 上没有性能优势。

#### 失败信号

- same-head 条件无法成立，实验一直混入不同 head build；
- matched recall 点上 query hash/粗排候选统计不可解释；
- legacy 与 two-stage 的 recall 接近，但 payload/page 指标方向完全反直觉。

#### Ablation 预期

- two-stage `TopRGlobal` 上升：
  - 预期 Recall 上升；
  - 预期 `DistanceEvaluatedCount` 上升；
  - 预期 `PayloadPhysicalBytesRead` 不降或小幅上升；
  - 若 Recall 上升但 `PayloadPhysicalBytesRead` 反而明显下降，需要优先排查统计或布局差异。
- legacy `InternalResultNum` 上升：
  - 预期 Recall 上升；
  - 预期 requested bytes 上升；
  - 但由于路径更成熟，QPS 下降幅度可能仍小于 two-stage。

### P1.3a posting-order reorder (PQ code sort) - 已失败

#### 状态：已完成 - 当前策略失败 (2026-05-02)

#### 实验结果

| 配置 | Unique Pages | Candidates | avg(Candidates/Page) | Recall@10 |
|------|-------------|------------|----------------------|-----------|
| Original Two-stage | 122 | 512 | 4.20 | 0.977 |
| Reordered Two-stage (PQ code sort) | 123 | 512 | 4.27 | 0.976 |

**注意**：`avg(Candidates/Page)` 是先计算每个 query 的 `Candidates ÷ Unique Pages`，再取均值。这不是 `总 Candidates ÷ 总 Unique Pages`。

**关键发现**：PQ code 排序几乎没有效果
- `Candidates/Page`: 4.2 → 4.3 (仅提升 2.4%)
- `Unique Pages`: 122 → 123 (基本不变)
- Recall 基本不变

#### 为什么 PQ code 排序无效

**根本原因**：PQ code 是 16 字节的量化码，代表向量在 16 个子空间中的聚类中心 ID。但：

1. **PQ code 粒度太粗**：
   - 每个子空间只有 256 个聚类中心
   - 相同 PQ code 的向量可能在原始空间中距离很远
   - PQ code 只用于 coarse scoring，不能准确反映向量相似性

2. **PQ code 排序信号与 query co-hit locality 不匹配**：
   - P1.3a 确实会改变 posting blob 内 payload 顺序；
   - 但按 PQ code 字典序相邻，不等价于同一 query 的 topR 候选会相邻；
   - 因此它没有显著减少同一 query 需要读取的 unique payload pages。

3. **问题在于 query 命中的 payload page 仍然太分散**：
   - 每个 posting list 的候选来自不同向量 ID
   - 这些向量在磁盘上可能分布在不同页面
   - 仅靠弱相关排序无法解决 query-level page 分散问题

#### 后续方向

历史记录：P3 已证明 query co-hit 层面存在足够理论上限，P4/P4b/P4c 已进入并完成多轮 build-side layout objective 迭代。当前不再停留在“是否值得做 layout”，而是推进 P4d layout 策略整合。

### P1.3b posting-aware payload physical layout（chunk-locality 已失败，co-hit 上限已通过 P3 验证）

#### 方向

代码层面需要先修正文档里的一个旧判断：two-stage 的 payload 已经写在 posting blob 内，`BuildSingleChunkTwoStagePostingBlob(...)` 和 `BuildChunkedPostingBlob(...)` 都会把 payload bytes 写入 posting blob 的 payload 区。因此 P1.3a 的 PQ code sort 实际改变了 posting 内 payload 顺序，但它没有显著改善 page locality。

这说明问题不是“payload 完全不能重排”，而是“简单按 PQ code 排序不足以让同一 query 的命中候选集中到少数 page”。P3 已完成 query co-hit trace 和 theoretical best packing 验证，理论 page reduction 上限 `40%~50%`；P4 same-head CoHit v1 已证明实际 layout 在 `TopRGlobal=256/512` 有效，但 `768` 收益衰减。

候选方向：

1. **query co-hit trace / theoretical best packing**：已完成，理论上限超过进入 P4 阈值。
2. **per-posting co-hit layout**：v1 已完成 same-head st=1 验证，使用 P3 trace 生成 offline order file 并在 build 阶段重排 posting blob。
3. **head-cluster / posting-group layout**：只作为 co-hit 信号不足时的备选解释，不进入近期实现。
4. **chunk-locality layout**：当前原型已失败，不继续盲扫 chunk size；除非 co-hit trace 证明 chunk 内共现密度可利用。

#### 当前结论

P1.3a 的 PQ code 排序策略**失败**，更准确的原因是它只做了弱相关的 posting 内排序，不能让 query topR 候选稳定集中到少数 payload page。

P1.3b 需要更有针对性的 build-side physical layout 设计。收益上限已由 P3 证明，v1 已由 P4 证明方向有效，v2 已证明 page-fill 贪心不足；下一步是 query-set/page-objective CoHit v3，而不是继续证明“是否值得做 layout”或微调 v2。

建议：
- 标记 P1.3a 为"当前策略失败"
- query co-hit trace 和 theoretical best packing 已完成
- 理论上限已足够，CoHit v1 已部分成功，当前实现应进入 page-aware v2
- 当前 two-stage 仍可作为功能正确的实现，但性能优势需要更大范围的架构改动

#### Hypothesis（P1.3b 当前假设）

如果 query co-hit trace 证明同一 query 的 TopR candidates 在 posting 内存在稳定共现关系，并且 theoretical best packing 有足够 page reduction 上限，那么按 co-hit 重排 posting blob 内 payload 应该能让 coarse/topR 命中的候选更可能落在相邻 page：

- `uniquePayloadPages/query` 应下降；
- `candidatesPerPayloadPage` 应上升；
- `PayloadPhysicalBytesRead/PagesRead` 应下降；
- QPS 应随之上升；
- Recall 在不改 coarse/topR 语义时应基本不变。

#### 实验设计

- 不改 coarse scoring / topR / rerank 语义；
- 先导出 global TopR 后的 `(queryID, postingID, vectorID, payloadPageID)` trace；
- 离线计算 current packing 与 theoretical best packing 的 `uniquePayloadPages/query` 差距；
- 只有 best packing 达到 `10%~15%+` page reduction 后，才实现 posting blob 内 co-hit layout；
- 构建前保留 `(vectorID -> payload bytes)` 的可校验映射；
- 所有 layout 都必须复用同一 head、query、truth、线程、CSV 口径；
- 所有 layout 都必须输出 payload byte hash / exact distance 校验；
- 若进入 layout prototype，使用以下对照：
  - `baseline_layout`：当前写入顺序；
  - `pq_code_layout`：P1.3a 的已失败基线，用于回归对照；
  - `cohit_layout`：基于 query trace 或 sampled co-hit 统计的 posting 内重排；
  - `chunk_locality_layout`：已失败基线，只保留复测或回归用途；
- 每个 layout 固定跑 `TopRGlobal=256/512/768` 和 `SearchThreadNum=1/8`；
- 每个 layout 记录 `uniquePayloadPages`、`avg(Candidates/Page)`、`payloadPagesPerPosting`、`totalPayloadPageSpans`、`PayloadPhysicalBytesRead`、`PagesRead`、QPS、Recall、stage hashes。

#### 成功结果

- theoretical best packing 先显示 `uniquePayloadPages/query` 至少有 `10%~15%` 可降空间；
- 实际 `PayloadPhysicalBytesRead` 或 `PagesRead` 相比 `baseline_layout` 明确下降，建议至少达到 `10%~15%` 量级；
- `avg(Candidates/Page)` 明确上升，建议至少达到 `20%` 相对提升；
- QPS 提升方向与 physical pages/bytes 下降方向一致，建议在 matched recall 点至少有 `10%+` 提升；
- Recall 基本不变，建议绝对值下降不超过 `0.001~0.002`，且能被 query-level 归因解释；
- query-level payload hash、final result hash、exact distance 校验保持语义一致。

#### 失败结果

- theoretical best packing 上限不足 `10%`，说明 layout 路线收益不够；
- layout 重排后 payload physical pages 几乎不变；
- 或 pages 降了但 QPS 不升，说明瓶颈转向 decode/rerank/调度；
- 或 Recall 出现不可解释下降，说明 page contract / payload mapping / build layout 一致性存在问题。
- 或 layout 只对训练 query 有效，对 held-out query locality 不改善，说明 co-hit layout 过拟合。

#### 失败信号

- 重排后 payload byte hash / exact distance 对不上；
- query-level payloadPageHash 稳定但 Recall 异常下滑；
- `PagesRead` 下降而 `PayloadPhysicalBytesRead` 不变且 tail waste 无改善；
- 单线程正确、多线程异常，说明 build layout 改动引入了新的并发或映射问题。
- `cohit_layout` 的 stage hash 在重复构建间不稳定，说明 layout 生成过程存在非确定性。

#### Ablation 预期

- `baseline_layout -> pq_code_layout`：
  - 预期结果仍接近 P1.3a，仅有很小 locality 变化；
  - 如果这次突然大幅改善，优先检查是否混入其他构建参数变化。
- `baseline_layout -> cohit_trace_best_packing`：
  - 预期 theoretical best pages 明显低于 current pages；
  - 如果理论上限不足 `10%`，停止 layout 实现。
- `baseline_layout -> cohit_layout`：
  - 预期 `uniquePayloadPages/query` 下降；
  - 预期 `payloadPagesPerPosting` 下降；
  - 预期 `avg(Candidates/Page)` 上升；
  - 若 locality 指标几乎不动，说明 query co-hit 并不能在 posting 内形成可利用的 page locality。
- `baseline_layout -> chunk_locality_layout`：
  - 已完成原型显示 pages/KB 未下降；
  - 后续不再盲扫 chunk size，除非 co-hit trace 证明 chunk 内共现密度随 target size 改善。
- `TopRGlobal=256 -> 512 -> 768` 在同一物理布局下：
  - 预期 Recall 上升；
  - 预期 `uniquePayloadPages/query` 上升或持平；
  - 如果 layout 真有改善，`avg(Candidates/Page)` 应在所有 TopR 档位同时抬升；
  - 若只在高 TopR 档位看到改善，说明布局收益依赖大候选集，默认档位价值有限。
- `SearchThreadNum=1 -> 8`：
  - 预期 locality 指标分布基本不变；
  - 预期 QPS 受益于更少 physical pages；
  - 若单线程收益存在但并发收益消失，优先怀疑调度/锁竞争/IO 饱和。

#### 2026-05-02 P1.3b chunk-locality 原型结果

本轮实现了最小代码原型：

- 新增 `PostingPayloadLayout` 参数；
- `PostingPayloadLayout=ChunkLocality` 使用 chunked two-stage posting，将 payload 按 chunk 内局部聚类写入；
- `PostingChunkPruneMode=None`，避免把 layout 实验和 pruning 质量损失混在一起；
- `.meta` 正确写出：
  - `LayoutType=chunked_twostage_v1`
  - `ChunkPruneMode=None`
  - `PayloadLayout=chunk_locality_payload_v1`

构建日志与产物：

- 构建配置：`configs/p1_chunklocality_build.ini`
- 搜索配置：`configs/p1_chunklocality_st1.ini`
- 索引目录：`/home/ray/data/sift1m/spann_index_m2_u8default_chunklocality`
- 结果 CSV：`/tmp/p1_chunklocality_st1/query_io_stats.csv`
- 汇总：`/tmp/p1_chunklocality_summary.tsv`

过程中发现并修复了一个 loader bug：

- 现象：构建完成后加载失败，日志为 `Posting 191 too small for new two-stage header`；
- 根因：two-stage metadata loader 首次只读 posting 起始页 4KB。当 posting 的 `pageOffset` 靠近页尾且 header/directory 跨页时，loader 会误判 header 不完整；
- 修复：首次 metadata read 改为至少覆盖 `pageOffset + NewPostingHeader + directory entry`，按 page 对齐读取；
- 验证：修复后 `cmake --build build --target ssdserving SPTAGTest -j4`、`./Release/SPTAGTest --run_test=SPANNPostingFormatTest` 通过，chunk-locality 1M 索引可加载并完成 10K query 搜索。

P1.3b chunk-locality 结果（`TopRPerPosting=64`，`TopRGlobal=512`，`SearchThreadNum=1`）：

| Layout | Recall@10 | unique pages | payload KB | candidates/page | postings | page spans | approx QPS |
|--------|-----------|--------------|------------|-----------------|----------|------------|------------|
| Non-chunked two-stage baseline（P2.2） | 0.97678 | 121.98 | 487.91 | 4.20 | 61.40 | 520.35 | 242.25 |
| Chunk-locality prototype | 0.97603 | 124.53 | 498.11 | 4.11 | 61.36 | 523.12 | 254.50 |

注意：上表不是严格 same-head 对照，chunk-locality 重新构建了 head，因此 QPS/Recall 只能作为方向性参考。更可靠的是 locality 指标：`unique_payload_pages` 和 `payload_kb` 没有下降。

#### P1.3b 当前判定

- `chunk_locality_layout` 的 hypothesis 在当前实现上**失败**：它没有降低 payload physical pages，也没有提升 `candidates/page`；
- Recall 基本保持，说明 payload mapping 语义大体正确；
- loader bug 修复是必要 correctness 改进，但不改变性能结论；
- 当前结果进一步支持 P2.2 的判断：瓶颈不是“payload 能不能在 posting blob 内重排”，而是“重排信号是否与 query-time TopR co-hit 集合一致”。

#### 失败原因分析

- 当前 chunk-locality 使用的是 posting 内向量 L2/residual 局部性，不是 query-level co-hit 局部性；
- Query-time payload candidates 是 `PostingTopRGlobal` 后的跨 posting 合并集合，单个 posting 内的静态 L2 chunk 不一定能聚合同一 query 的最终候选；
- chunked layout 增加了 directory/centroid 元数据，并改变 page packing，可能抵消甚至恶化 payload page locality；
- 因为 pruning 关闭，所有 chunk 都参与 coarse scan，chunk 的作用只剩 payload 物理排列，收益上限更依赖候选 co-hit 是否真的落在同一 chunk。

#### 下一步

- 不建议继续扩大 `PostingChunkTargetSize` 做盲扫，除非先证明 chunk 内 co-hit 密度随 target size 改善；
- P3 trace 与 theoretical best packing 已完成，CoHit v1/v2 same-head 已证明 256/512 有效；
- 下一步不再重复 chunk-locality、P3 或 page-fill v2，而是实现 query-set/page-objective CoHit v3。

### P2.1 coarse recall / miss-case attribution 修正（代码已完成，结果已验证）

#### 2026-05-02 实现更新

本阶段已完成代码侧最小闭环，范围如下：

- `ScanCompactCodes(...)` 在 truth 存在时，先对每个 scanned posting/chunk 的完整 `localCandidates` 排序，再在执行 `PostingTopRPerPosting` 截断前记录 truth 的 best local rank。
- `ComputeMissCaseAttribution(...)` 现在按四个互斥 loss bucket 分类：
  - `truthMissingPostingNotVisited`：truth 没有出现在任何 scanned posting/chunk 中。注意这是“not observed in scanned posting/chunk”，不是严格全局“not in any posting”。
  - `truthDroppedByPerPostingTopR`：truth 出现在 scanned posting/chunk 中，但 local rank 低于 `PostingTopRPerPosting`。
  - `truthDroppedByGlobalTopR`：truth 通过 per-posting topR，但未进入 `PostingTopRGlobal` 后的 rerank candidates。
  - `truthDroppedByRerankTopK`：truth 进入 rerank candidates，但 exact rerank 后未进入最终 topK。
- `coarseRecallAfterDedupe` 改为使用 `m_bestCoarseCandidateByVector`，语义是 per-posting topR 后、global topR 前的 dedupe recall。
- `rerankRecall` 改为使用 `m_mergedCandidates`，语义是 global topR 后实际 payload/rerank candidate recall。
- `finalRecall` 改为复制并排序当前 query result，只统计 `ResultNum` 个最终 topK；不再把 `SearchInternalResultNum`/内部 result 容量误当 final topK。
- 新增 `truthRecoveredByHeadResult`，用于解释 truth 没进入 posting rerank candidates、但由 head result 保留在最终 topK 的情况。loss bucket 只统计未进入最终 topK 的 truth。
- CSV 新增 `truth_dropped_rerank_topk`，并把原 `truth_missing_posting_not_visited` 表头改成 `truth_missing_not_observed_in_scanned_posting`。
- `truthMissingNotInPosting` 目前保留为 reserved 字段，只有后续接入离线全量 `truthVID -> all postingIDs` membership 后，才可用于严格判断“not in any posting”。
- 新增 `scripts/run_spann_p2_topr_sweep.sh` 和 `scripts/summarize_spann_p2_sweep.py`，用于自动生成 P2.2 sweep 配置、运行每个组合并汇总 attribution/payload 指标。

当前代码验证：

- `cmake --build build --target ssdserving SPTAGTest -j4`：通过。注意 P2 sweep 使用 `Release/ssdserving`，必须显式构建 `ssdserving` 目标。
- `./Release/SPTAGTest --run_test=SPANNPostingFormatTest`：通过。
- `python3 -m py_compile scripts/summarize_spann_p2_sweep.py`：通过。
- `scripts/run_spann_p2_topr_sweep.sh --help`：通过。
- P2.2 smoke：`scripts/run_spann_p2_topr_sweep.sh -c configs/p2_miss_attribution.ini -o /tmp/p2_topr_sweep_smoke4 -p /home/ray/data/sift1m/spann_index_m2_u8default -r 64 -g 512 -t 1 -q 10` 通过。结果中 `truth_attribution_closure_error=0`，`truth_recovered_by_head_result=1.3`，验证了 head contribution 被单独拆出。
- P2.2 full sweep：`results/io_analysis/p2_topr_sweep_20260502_topk_v2/summary_metrics.tsv` 全部组合 `truth_attribution_closure_error=0`，说明 P2.1 attribution 闭合成立。

#### Hypothesis

P1.2 已说明 two-stage 在当前 layout 上没有性能优势，但 Recall/QPS tradeoff 里仍混有 coarse 质量问题。P2.1 的核心假设是：

- 只要记录 truth 在已扫描 posting/chunk 内的完整 local rank，就能把 `!inCoarse` 拆成 `not observed in scanned posting/chunk` 和 `per-posting topR 截断`；
- 再结合 dedupe 前后、global topR 后、final topK 后的集合，就能把 loss 拆成 per-posting topR、global topR、exact rerank/topK；
- 严格的 `not in any posting` 需要额外离线全量 membership，当前代码不再把它和 scanned-posting miss 混在一起；
- attribution 闭合后，后续 topR sweep 才能解释 Recall/QPS 曲线。

#### 实验设计

- 固定当前 two-stage 索引与 query/truth；
- 禁用 chunk pruning，先只看 pure M2；
- 为每个 query 记录 visited posting IDs；
- 先使用已实现的 scanned-posting attribution 跑 P2.2；
- 后续如仍需要严格区分 posting-not-visited 与 not-in-any-posting，再建立或导出 `truthVID -> postingID list` 全量 membership 映射；
- 对每个 truth VID 记录：
  - 是否在 scanned posting/chunk 中；
  - 在 posting 内的 coarse rank；
  - 是否被 `PostingTopRPerPosting` 截断；
  - 是否进入 global topR；
  - 是否进入 final rerank candidates；
  - 是否来自 head result 而非 posting result；
- 导出修正后的：
  - `truthMissingPostingNotVisited`
  - `truthRecoveredByHeadResult`
  - `truthDroppedByPerPostingTopR`
  - `truthDroppedByGlobalTopR`
  - `truthDroppedByRerankTopK`
  - `truthMissingNotInPosting`（reserved，待全量 membership）
  - `truthDroppedByExactRerank`（已由 `truthDroppedByRerankTopK` 覆盖）

#### 成功结果

- 对每个 query，truth 的 attribution 合计能与 truth count 闭合；
- `truthMissingPostingNotVisited` 不再长期为 0，除非确实所有 truth 都出现在 scanned posting/chunk 中；
- `truthDroppedByPerPostingTopR` 对 `PostingTopRPerPosting` 增大敏感；
- `truthDroppedByGlobalTopR` 对 `PostingTopRGlobal` 增大敏感；
- `truthDroppedByRerankTopK` 能解释进入 rerank 但未进最终 topK 的 miss；
- `truthRecoveredByHeadResult` 能解释 `Final Recall > Rerank Recall`；
- 能判断默认档位的主要质量损失来自 posting visit、per-posting topR、global topR、PQ 排序还是 exact rerank。

#### 失败结果

- attribution 合计不闭合；
- 或 `truthMissingPostingNotVisited` 仍吞掉大部分 miss，且 posting visit 参数变化后完全不动，说明还缺全量 membership 或 scanned-posting 记录未接入成功；
- 或 `truthMissingNotInPosting` 非零，说明 reserved 字段被误写；
- 或 head result 与 posting result 的贡献无法拆开，导致 `Final Recall > Coarse Recall` 仍不可解释。

#### 失败信号

- `coarseRecall@TopRGlobal` 与最终 Recall 趋势明显背离，且无法由 rerank/page 指标解释；
- `truthDroppedBy*` 各项合计后与真实 miss 数不闭合；
- `finalRecall + truthMissingPostingNotVisited + truthDroppedByPerPostingTopR + truthDroppedByGlobalTopR + truthDroppedByRerankTopK + truthMissingNotInPosting != truthCount`；
- 同一参数重复两轮，miss attribution 分布大幅漂移；
- `st=1` 和 `st=8` 的 miss attribution 结构明显不同，但 query hash 没有解释这种差异。
- `truthMissingNotInPosting` 非零但没有全量 membership 实现；
- head-only truth 被误计为 posting pipeline recall。

#### Ablation 预期

- 使用 scanned-posting rank attribution 前后：
  - 预期 `truthMissingNotInPosting` 下降到 0 或保持 0；
  - 预期 `truthMissingPostingNotVisited` 或 `truthDroppedByPerPostingTopR` 上升到合理非零值；
  - 如果各桶完全不变，说明新映射没有接入 attribution 路径。
- 后续接入全量 membership 前后：
  - 预期 `truthMissingPostingNotVisited` 进一步拆成 posting-not-visited 与 strict not-in-any-posting；
  - 预期 strict not-in-any-posting 接近 0，若显著非零，说明 build-side replica/posting assignment 存在异常或 truth/global VID 对齐有问题。
- 分离 head result contribution 后：
  - 预期能解释 `Final Recall > Coarse Recall`；
  - 如果不能解释，说明 final result set 或 recall 统计口径仍有混合。
- `SearchThreadNum=1 -> 8`：
  - 预期 attribution 分布基本一致；
  - 如果不同，优先查 query workspace / result merge 的并发稳定性。

### P2.2 topR / visit 参数 sweep（2026-05-02 已执行）

#### Hypothesis

在 P2.1 attribution 闭合后，`PostingTopRPerPosting`、`PostingTopRGlobal` 和 posting visit 数量的 sweep 应能分别影响不同 miss bucket，从而建立质量成本曲线。

#### 实验设计

- 固定 same index / same query / same truth；
- 禁用 chunk pruning；
- sweep 矩阵：
  - `PostingTopRPerPosting=32/64/96/128`
  - `PostingTopRGlobal=128/256/384/512/768`
  - posting visit 或 head search 参数使用当前可用的最小矩阵；
  - `SearchThreadNum=1` 建质量基线，`SearchThreadNum=8` 做并发复核；
- 每个点记录 Recall、QPS、`DistanceEvaluatedCount`、`PagesRead`、`PayloadPhysicalBytesRead`、P2 attribution buckets。
- 当前环境读取 `/proc/<pid>/io` 会触发权限问题，本轮使用 `--no-monitor`，依赖 `ssdserving` detailed CSV 统计 payload/page 指标。
- 已执行命令：

```bash
scripts/run_spann_p2_topr_sweep.sh \
  -c configs/p2_miss_attribution.ini \
  -o results/io_analysis/p2_topr_sweep_20260502_topk_v2 \
  -p /home/ray/data/sift1m/spann_index_m2_u8default \
  -r 32,64,96,128 \
  -g 128,256,384,512,768 \
  -t 1 \
  -q 10000 \
  --no-monitor
```

脚本会输出：

- `run_matrix.tsv`：每个参数组合对应的 config、query CSV、run dir；
- `summary_metrics.tsv`：每个参数组合的 recall、P2 buckets、payload pages、candidate/page、payload KB 等均值。

#### 实际结果摘要

以 `PostingTopRPerPosting=64` 为主线：

| TopRGlobal | Recall@10 | dropped_global | not_observed_scanned | unique_pages | payload KB | approx st=1 QPS |
|------------|-----------|----------------|----------------------|--------------|------------|-----------------|
| 128 | 0.92518 | 0.5593 | 0.1866 | 65.31 | 261.25 | 324.88 |
| 256 | 0.96204 | 0.1882 | 0.1866 | 94.96 | 379.84 | 276.80 |
| 384 | 0.97276 | 0.0794 | 0.1868 | 111.72 | 446.88 | 255.32 |
| 512 | 0.97678 | 0.0390 | 0.1868 | 121.98 | 487.91 | 242.25 |
| 768 | 0.97925 | 0.0103 | 0.1868 | 132.75 | 531.02 | 228.75 |

固定 `PostingTopRGlobal=512` 看 `PostingTopRPerPosting`：

| TopRPerPosting | Recall@10 | dropped_per_posting | dropped_global | unique_pages | payload KB | approx st=1 QPS |
|----------------|-----------|---------------------|----------------|--------------|------------|-----------------|
| 32 | 0.97689 | 0.0016 | 0.0364 | 122.89 | 491.56 | 261.76 |
| 64 | 0.97678 | 0.0001 | 0.0390 | 121.98 | 487.91 | 242.25 |
| 96 | 0.97677 | 0.0000 | 0.0392 | 121.96 | 487.84 | 233.58 |
| 128 | 0.97677 | 0.0000 | 0.0392 | 121.96 | 487.84 | 230.86 |

#### 结论

- P2.2 的核心 hypothesis 部分成立：`PostingTopRGlobal` 是主要质量旋钮，增大后 `truthDroppedByGlobalTopR` 单调下降，Recall 单调上升，但 payload pages / payload KB / latency 同步上升。
- `PostingTopRPerPosting` 不是当前瓶颈：从 32 增到 64/96/128 后，`truthDroppedByPerPostingTopR` 已接近 0，Recall 几乎不变，但 coarse candidate count before/after dedupe 和 CPU 开销上升。
- `truthMissingNotObservedInScannedPosting` 基本固定在 `0.1866~0.1868`，说明剩余质量下限来自 head/posting coverage 或未接入的全量 membership 维度，而不是 per-posting TopR。
- `truthRecoveredByHeadResult` 基本固定在约 `1.57`，解释了 `Final Recall > Rerank Recall`，也说明最终 Recall 不能只看 posting pipeline。
- 当前 two-stage 性能瓶颈集中在 Fetch Payload & Rerank：Global TopR 从 256 到 512，Recall 只提升约 `0.0147`，但 unique pages 从约 `95` 增到约 `122`，payload KB 从约 `380 KB` 增到约 `488 KB`；P1 attribution 显示 scan/merge/plan 只解释 27.4% ex-latency gap，主要差距仍要在 payload fetch/rerank 内部继续拆分。

#### 对 hypothesis 的判定

- `PostingTopRGlobal` 影响 global miss bucket：成立。
- `PostingTopRPerPosting` 影响 per-posting cutoff miss：方向成立，但当前默认附近已经不是有效瓶颈。
- posting visit 参数影响 posting-not-visited miss：本轮未 sweep，不能判定；需要后续单独做 head/search visit 或全量 membership 补证。
- 能找到 matched recall 下成本最低区域：部分成立。`TopRGlobal=512~768` 能接近 legacy recall，但成本过高；`TopRGlobal=256~384` 成本较低但 recall gap 明显。

#### 实际成功信号

- 所有 sweep 点 `truth_attribution_closure_error=0`；
- `truth_missing_not_in_posting_reserved=0`，reserved 字段未被误写；
- `truthDroppedByGlobalTopR` 随 `PostingTopRGlobal` 上升稳定下降；
- `DistanceEvaluatedCount` / `rerankCandidateCount` 与 `PostingTopRGlobal` 对齐；
- payload pages 与 payload KB 随 `PostingTopRGlobal` 上升，方向符合预期。

#### 实际失败信号 / 风险信号

- `PostingTopRPerPosting` 上升带来更多 coarse candidate 和更低 QPS，但几乎没有 Recall 收益，不应继续作为主优化方向。
- `truthMissingNotObservedInScannedPosting` 对 TopR sweep 基本不动，说明仅扩大 TopR 不能解决 coverage 下限。
- 高 `PostingTopRGlobal` 档位的 Recall 主要靠读更多 payload page 换来，不能作为最终性能优化方案。
- P2.2 只跑了 `SearchThreadNum=1`，还没有复核并发下 attribution 分布是否稳定。
- 本轮未使用外部 `/proc/<pid>/io` monitor，因此 page/byte 结论来自 detailed CSV；如果后续恢复 monitor，需要确认两个口径一致。

#### 成功结果

- `PostingTopRPerPosting` 上升主要降低 per-posting cutoff miss；
- `PostingTopRGlobal` 上升主要降低 global topR miss；
- posting visit 参数上升主要降低 posting-not-visited miss；
- 能找到 matched recall 下成本最低的参数区域。

#### 失败结果

- 参数变化无法稳定改变对应 miss bucket；
- 或 Recall 提升主要来自 head result，posting pipeline attribution 没有解释力；
- 或成本随参数上升过快，matched recall 下没有任何 two-stage 可用档位。

#### 失败信号

- 同一 sweep 点重复运行，bucket 分布大幅漂移；
- `PostingTopRGlobal` 上升但 `rerankCandidateCount` 不升；
- `PostingTopRPerPosting` 上升但 per-posting cutoff miss 不变，同时 coarse candidate count 也不变；
- Recall 提升伴随 payload pages 下降，方向反常，需要排查统计或配置混用。

#### Ablation 预期

- `PostingTopRPerPosting` 上升：
  - 预期 `truthDroppedByPerPostingTopR` 下降；
  - 预期 `coarseRecall@TopRGlobal` 上升或持平；
  - 预期 payload/page 成本小幅上升。
- `PostingTopRGlobal` 上升：
  - 预期 `truthDroppedByGlobalTopR` 下降；
  - 预期 `rerankCandidateRecall@TopRGlobal` 上升；
  - 预期 `DistanceEvaluatedCount`、`PagesRead`、`PayloadPhysicalBytesRead` 上升。
- 如果 `PostingTopRPerPosting/Global` 都升高，但 Recall 提升有限：
  - 预期说明主要问题在 PQ coarse 排序或 posting 未访问，而不是 topR 截断。
- 如果 `truthMissingPostingNotVisited` 占比高：
  - 预期说明 head / posting visit 策略才是主问题，单纯调 payload layout 收益上限有限。

### 更新后的执行顺序

从当前结果出发，下一步不再是泛泛地“继续优化 P1”，而是按以下顺序推进：

1. **官方 strict baseline 参数产品化**：先把可用性能档位固定下来，默认性能路径优先使用 `InternalResultNum` 和 `SearchThreadNum`。
2. **Fetch Payload & Rerank 子阶段拆分**：已完成，P2 已确认 `Payload Read Wait` 是绝对主瓶颈。
3. **query co-hit trace / theoretical best packing**：已完成，理论 page reduction 上限 `40%~50%`。
4. **CoHit layout v1**：same-head st=1 已部分成功，256/512 有效，768 收益弱。
5. **page-aware CoHit v2 + st=8**：已完成，弱正向但 768 未达 `10%+`。
6. **query-set/page-objective CoHit v3**：已完成，768 达标但 512 回退。
7. **P4d layout 策略整合**：当前主线，目标是在 512/768 间找到 Pareto layout 或明确参数化 layout。
8. **M3 pruning 重做**：暂缓，只有在 payload layout 和 coarse 质量都可解释后再判断真实价值。

### 独立的全局失败信号

以下信号需要单独记录，一旦出现，应暂停继续做性能结论：

1. query-level hash 不稳定，但 Recall/QPS 表面正常；
2. locality 指标、payload bytes/page 指标与 raw log 口径对不上；
3. same-head 条件未满足却继续横向比较 legacy 与 two-stage；
4. payload reorder 后 exact distance 或 payload byte hash 不一致；
5. 某个 ablation 的 QPS 提升没有伴随任何可解释的 bytes/pages、CPU/cycles 或 scheduling 指标变化；
6. Recall 下降被归因于“参数 tradeoff”，但 coarse/miss-case 观测并未支持该解释。

### 结论性行动项

从现在开始，P1 不再以“two-stage 是否能跑”为目标，而改为以下验收链条：

1. **先用官方 strict baseline 确定性能目标线**
2. **Fetch Payload & Rerank 内部成本已拆解，主瓶颈为 Payload Read Wait**
3. **当前用 query co-hit trace 证明 payload layout 的理论上限**
4. **理论上限已足够，当前实现并验证 co-hit layout**
5. **最后验证 layout 改善是否能同时转化为 bytes/pages/QPS 改善**

在这五步完成之前，不建议把 P1 标记为完成，也不建议把当前 two-stage 结果作为默认性能方案。
