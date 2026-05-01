# SPANN M2/M3 代码与验证方案审查记录（2026-05-01）

## 结论

当前 M2/M3 方向仍然成立，但不能再按“性能方案已经成功”来描述。更准确的状态是：

1. **格式与搜索闭环已经基本打通**：new-format static posting 能 build / load / search，M2/Chunked-M2 已不再是早期 `distanceEvaluated=0` 的失真状态。
2. **metadata query 热路径问题已经阶段性解决**：metadata 已能在 `LoadIndex` 阶段预解析并缓存，query 阶段 `MetadataBytesRead=0` 的结果证明这一步有效。
3. **code physical read inflation 已通过 code cache 路线显著压掉**：code-cache 档位下 `CodePhysicalBytesRead=0`，QPS 从 metadata-cache 档约 `1.07k` 提升到约 `2.93k`。
4. **剩余主瓶颈已经转向 payload physical page inflation**：即使 code I/O 清零，`PayloadPhysicalBytesRead` 仍约 `388KB/query`，`PagesRead` 仍约 `95/query`，这仍是当前最明确的性能大头。
5. **历史 benchmark 曾出现高并发 recall 下降，但在当前代码上尚未被单测复现**：旧日志里 `SearchThreadNum=8` 的 recall 低于 `SearchThreadNum=1`，因此仍需继续复核；但当前代码已经补上 query 级 `coarse/payload/final` hash 和并发确定性回归测试，最新 `SPANNPostingFormatTest` 可稳定通过，所以这项目前更准确地说是“待 canonical benchmark 复核的风险”，而不是“当前已确认仍存在的 bug”。
6. **M3 pruning 当前只能按 heuristic 处理**：更细 chunk 能提高 prune ratio，但当前 threshold/chunk/topR 组合会先伤 recall，不能作为默认 safe pruning 主线。
7. **本轮代码已经完成一批基础修正与热路径优化**：包括 `SearchIndex(...)` 失败路径 workspace 归还、static two-stage workspace 固定 async channel、code bytes 统计纠正、workspace 级 buffer/hash-map 复用、payload read plan 页键缓存与 rerank 内环减扫，这些改动已通过 `SPANNPostingFormatTest` 回归。

因此，下一阶段不应继续围绕“是否实现 M2/M3”讨论，而应围绕以下问题收敛：

- 历史并发 recall 现象是否还能在当前代码和 canonical benchmark 上复现；
- payload physical pages 是否真的下降；
- coarse candidate recall 损失来自哪里；
- M3 pruning 是否有 exact upper bound 支撑；
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
- payload physical pages 仍然是最值得继续优化的主瓶颈。

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
2. 剩余主瓶颈已经集中到 payload page：payload physical bytes/pages 仍高。
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

## 3. 优化方向优先级排序

### P0：复核历史并发 recall 现象是否仍存在（最高优先级）

#### 状态：已完成 ✓ (2026-05-01)

#### 测试结果

使用 strict `UInt8 + DEFAULT` 格式和官方索引 `/home/ray/data/sift1m/spann_index_official_u8default_20260430`，测试配置：

```
ValueType=UInt8
VectorType=DEFAULT
InternalResultNum=64
SearchPostingPageLimit=4
IndexDirectory=/home/ray/data/sift1m/spann_index_official_u8default_20260430
```

| SearchThreadNum | QPS | Recall@10 | 延迟 |
|---|---|---|---|
| 1 | 951 | 0.978319 | ~10.5 ms |
| 2 | 1946 | 0.978319 | ~5.1 ms |
| 4 | 3764 | 0.978319 | ~2.7 ms |
| 8 | 5889 | 0.978319 | ~1.7 ms |

#### 验收结论

**历史并发 recall 下降现象已不存在！**

- 所有并发级别下 Recall@10 均稳定在 0.978319
- QPS 随 SearchThreadNum 近似线性增长
- 并发扩展性良好，st=8 时 QPS ≈ 5889，与历史记录 (~5945) 接近

#### 配置文件

测试配置文件已保存在：
- `configs/p0_st1_trace.ini`
- `configs/p0_st2_trace.ini`
- `configs/p0_st4_trace.ini`
- `configs/p0_st8_trace.ini`

#### 结论

历史 benchmark 中观察到的 `st=8` recall 下降现象在当前代码上已无法复现。当前代码新增的并发确定性测试和 workspace 生命周期修复已生效。后续优化可以安全地以 `st=8` 作为默认并发配置。

---

### P0 原始分析（保留作为参考）

#### 为什么排第一

历史 code-cache benchmark 中，`SearchThreadNum=1` 与 `SearchThreadNum=8` 的 recall 差距过大：

```text
st=1: Recall@10≈0.9627
st=8: Recall@10≈0.941
```

这不是单纯性能优化问题，而是结果语义稳定性问题。但当前代码状态需要更谨慎地描述：

- benchmark 历史记录提示这里有风险；
- 当前单测已经补上并发确定性覆盖，且最新通过；
- 所以下一步应该是“先复核是否还能复现”，而不是直接假定 bug 仍然存在。

只要这件事还没复核清楚，后续任何更激进的 QPS 提升都仍然可能建立在错误前提上。

#### 主要怀疑点

1. `ExtraWorkSpace` 是否真正线程独占；
2. `m_postingRuntimeMetadata` / `m_codeCache` 是否只读且生命周期稳定；
3. `PostingBlockInfo::m_cachedCode` 指针是否可能因 vector resize/move 失效；
4. `AsyncReadRequest` / `PayloadReadRequest` 是否在 batch 分组或 callback 中复用了错误状态；
5. `QueryResultSet` 在 head search 与 posting search 之间的 `worstDist()`、target、quantized target 是否被并发污染；
6. `m_payloadDiskRequests` / `m_chunkCodeDiskRequests` capacity 扩容后，已保存的 index/pointer 是否仍有效。

#### 具体执行步骤

1. 固定同一索引、同一 binary、同一 query set，分别跑：
   - `SearchThreadNum=1`
   - `SearchThreadNum=2`
   - `SearchThreadNum=4`
   - `SearchThreadNum=8`
2. 保持：
   - `PostingTopRPerPosting=64`
   - `PostingTopRGlobal=256`
   - `EnableChunkedPosting=false`
   - `PostingChunkPruneMode=None`
   - code cache 开启状态不变
3. 对每档输出：
   - `Recall@10`
   - `QPS`
   - `DistanceEvaluatedCount`
   - `CoarseCandidateCount`
   - `CoarseCandidateCountAfterDedupe`
   - `RerankCandidateCount`
   - `PayloadPhysicalBytesRead`
   - `PagesRead`
4. 如果 `DistanceEvaluatedCount`、candidate count、payload pages 在不同 `st` 下不一致，再优先查 workspace/request 生命周期、async request 状态或结果集合并路径。
5. 抽样固定 10 个 query，输出每个 query 的：
   - visited posting IDs；
   - merged candidate VID list hash；
   - payload page key list hash；
   - final top10 VID list。
6. 对比 `st=1` 与 `st=8` 同一 query 的 candidate hash 差异，定位 recall 差异出现在哪一阶段：
   - coarse scan 前；
   - merge/dedupe 后；
   - payload read 后；
   - exact rerank 后。
7. 在定位前，不继续下探 `PostingTopRGlobal=192/128`，避免把已有并发问题和更激进 candidate 截断混在一起。

#### 验收标准

- `st=8` 的 `Recall@10` 应接近 `st=1`；
- 同一 query 在不同并发下的 candidate hash 应稳定，除非存在明确的非确定性排序 tie 且不影响 final result；
- 若 recall 仍低于 legacy，应能明确归因于 coarse topR/PQ，而不是并发状态污染。

---

### P1：降低 payload physical page inflation

#### 为什么排第二

metadata 和 code physical I/O 已经分别被 metadata cache、code cache 显著压掉。当前 code-cache 档位下：

```text
MetadataBytesRead = 0
CodePhysicalBytesRead = 0
PayloadPhysicalBytesRead ≈ 388KB/query
PagesRead ≈ 95/query
```

所以 payload physical pages 已经是唯一主要 I/O 大头。继续优化 metadata/code 的收益空间小，而 payload 是继续逼近 legacy QPS 的主战场。

#### 当前问题本质

当前 payload 逻辑是按候选所在 page 读取：

- `BuildPayloadReadPlan(...)` 为 candidate 覆盖的 page 建 request；
- `FetchPayloadPagesAndRerank(...)` 读 page，再拷贝 payload 到 scratch 做 exact distance。

问题是：

```text
payload logical bytes 很小
但为了这些 payload 要读很多 4KB physical page
```

这说明当前 payload layout 与 coarse topR 命中位置不匹配，候选过于分散。

#### 具体执行步骤

1. 补充 payload page locality 指标：
   - `uniquePayloadPages/query`
   - `payloadCandidates/query`
   - `candidatesPerPayloadPage`
   - `payloadPageReuseRatio`
   - `payloadPagesPerPosting`
   - `payloadTailWasteBytes`
2. 输出每 query 的 payload page 分布摘要：
   - top postings by payload pages；
   - pageID 是否连续；
   - 每个 posting 中命中的 page span；
   - candidate 是否集中在少数 chunk/page。
3. 先做只读 instrumentation，不改读取逻辑，确认：
   - physical bytes 大是因为 page 分散；
   - 还是因为 candidate 跨页；
   - 还是因为重复 page 去重不充分。
4. 在 build 侧评估 payload 重排，而不是先改 query merge：
   - 按 compact code/coarse-friendly 顺序写 payload；
   - 或按 chunk 内局部聚类顺序写 payload；
   - 保证 topR 候选更可能落在相邻 page。
5. 对比重排前后：
   - `PayloadPhysicalBytesRead`
   - `PagesRead`
   - `candidatesPerPayloadPage`
   - `Recall@10`
   - `QPS`
6. 如果再次尝试 payload request merge，必须加严格 page-contract 验证：
   - 每个 candidate 的 `(postingID, pageID, pageLocalOffset, payloadBytes)`；
   - merge 前后读取出的 payload byte hash 必须一致；
   - 先在单线程、少量 query 上逐 candidate 比较 exact distance。

#### 验收标准

- `PayloadPhysicalBytesRead` 或 `PagesRead` 有明确下降；
- `PayloadLogicalBytesRead` 可以不变，但 `candidatesPerPayloadPage` 应上升；
- recall 不应出现无法解释的下降；
- QPS 提升必须与 physical pages/bytes 下降方向一致。

---

### P2：补齐 coarse candidate recall / miss-case 观测

#### 为什么排第三

M2 的质量损失可能来自多个阶段：

1. PQ/coarse code 排序误差；
2. per-posting topR 截断；
3. global topR 截断；
4. chunk pruning；
5. payload read/rerank 实现问题；
6. 并发状态问题。

如果只看最终 `Recall@10`，无法判断该调 `topR`、改 quantizer、关 pruning，还是修 bug。

#### 具体执行步骤

1. 在离线验证模式下加载 truth，给每个 query 记录：
   - truth top10 VID；
   - coarse candidates before dedupe；
   - coarse candidates after dedupe；
   - final rerank candidates；
   - final top10。
2. 新增或导出以下指标：
   - `coarseRecall@TopRGlobal`
   - `rerankCandidateRecall@TopRGlobal`
   - `truthDroppedByPerPostingTopR`
   - `truthDroppedByGlobalTopR`
   - `truthDroppedByChunkPrune`
   - `truthMissingBecausePostingNotVisited`
3. 做 topR sweep：
   - `PostingTopRPerPosting=32/64/96/128`
   - `PostingTopRGlobal=128/192/256/384/512`
   - 先禁用 chunk pruning；
   - 先用 `SearchThreadNum=1` 建质量基线，再扩大到 `st=8`。
4. 对每个 sweep 点同时记录：
   - `Recall@10`
   - `coarseRecall@10`
   - `DistanceEvaluatedCount`
   - `PayloadPhysicalBytesRead`
   - `PagesRead`
   - `QPS`
5. 找出 matched recall 下的最小 rerank/payload 成本，而不是只追求固定 `256`。

#### 验收标准

- 能解释 M2 与 legacy 的 recall gap；
- 能区分 coarse 排序问题和 payload/rerank 实现问题；
- topR 参数选择基于 recall curve，而不是固定沿用 `64/256`。

---

### P3：重新设计 M3 pruning 验证，不把 heuristic 当 safe

#### 为什么排第四

M3 是有潜力的，但当前还不是主瓶颈。更细 chunk 已证明能提高 prune ratio，但 recall 下降明显；如果在 P0/P1/P2 前继续强推 pruning，很容易把质量问题和 I/O 问题混在一起。

#### 当前安全边界

`||q-c||-radius` 对 L2 可以作为 chunk 内点的距离下界，但安全剪枝还需要一个可靠的 exact topK 上界。当前若用 head result 的 `queryResults.worstDist()` 作为 threshold，它不是 payload exact topK 上界，因此只能归为 heuristic。

#### 具体执行步骤

1. 默认实验矩阵中先关闭 pruning：
   - `PostingChunkPruneMode=None`
2. 单独开 M3 pruning 实验，不与 code-cache 并发问题混跑。
3. 增加 pruning attribution：
   - pruned chunk count；
   - pruned records count；
   - pruned chunks 中是否包含 truth VID；
   - 每个 miss query 被哪个 chunk prune 掉。
4. 实现两档模式：
   - `HeuristicL2`：当前模式，只能作为可选 recall/QPS tradeoff；
   - `SafeL2CandidateUpperBound`：只有当 threshold 来自 exact rerank 上界时才允许标为 safe。
5. 改进 chunk 构造质量：
   - 不按写入顺序硬切；
   - 按 posting 内 residual / vector locality 聚类；
   - 输出 chunk radius 分布 `P50/P90/P99/max`；
   - 输出 chunk 内候选密度与 prune ratio 的关系。
6. 对比 `PostingChunkTargetSize=32/64/128` 时，不只看 prune ratio，还必须看：
   - truth pruned count；
   - chunk radius 分布；
   - payload pages/query；
   - recall/QPS curve。

#### 验收标准

- heuristic 模式必须标明 recall 损失；
- safe 模式必须证明 threshold 是 exact upper bound；
- chunk size 调整必须带来 physical pages 下降，而不是只带来 logical bytes 下降；
- 若 recall 下降但 payload physical pages 不降，应判为失败。

---

### P4：保留 code cache，暂不回到激进 code async 主线

#### 为什么排第五

code physical read inflation 已经通过 code cache 打掉：

```text
CodePhysicalBytesRead = 0
```

因此当前不应优先回到高风险的 code async / window merge-copy。之前直接复用 Linux batch async 曾触发 `io_submit(...): Operation not permitted`，说明这条路还有环境/权限/调用契约问题。

#### 具体执行步骤

1. 保留 load-time code cache 作为当前主试验路径；
2. 移除或降级 code cache 热路径中的高频日志；
3. 给 code cache 加 memory footprint 统计：
   - total code cache bytes；
   - per-posting code cache size distribution；
   - load time 增量；
   - RSS 增量。
4. 只有在 payload 优化后仍需要进一步压内存/启动时间时，再重新评估 code async；
5. 若重启 code async，必须先抽象独立 page-level code read path，而不是直接复用当前 payload batch path。

#### 验收标准

- code cache 默认不破坏 recall；
- `CodePhysicalBytesRead=0` 稳定；
- load-time 成本和内存占用可接受；
- 不再把 code async 失败与主线结果混在一起。

---

### P5：统一 benchmark 记录规范

#### 为什么仍需要做

这不是性能瓶颈本身，但能防止重复误判。历史上已经出现过格式声明与二进制布局不一致、SearchIndex 返回值被吞、report 与 raw log 口径不一致等问题。

#### 具体执行步骤

每轮 benchmark 必须记录：

1. git commit / diff 摘要；
2. binary 路径和 build flags；
3. index build 配置；
4. search 配置；
5. index 目录；
6. result 目录；
7. raw `sptag.log` 中 QPS / Recall 行；
8. detailed stats CSV；
9. 是否 cold cache；
10. 是否复用旧 index；
11. sidecar format metadata 摘要。

每个结论旁边必须标明：

```text
source = raw log / detailed stats / report summary
```

#### 验收标准

- 同一个表中的 QPS/Recall/bytes 必须来自同一轮运行；
- 若 `report.md` 与 raw log 不一致，raw log 优先；
- 任何 new-format benchmark 必须重新 build 与当前代码匹配的 index。

---

## 4. 不建议优先推进的方向

### 4.1 不建议继续盲目缩小 `PostingChunkTargetSize`

`t32` 已经说明更细 chunk 能提高 prune ratio，但 recall 下降更明显。继续缩小 chunk 只会放大 metadata / chunk management / candidate fragmentation 风险。

只有在以下条件满足后才继续做 chunk-size sweep：

- 并发 recall 稳定；
- pruning attribution 可解释；
- chunk radius 分布可观测；
- payload physical pages 是主要优化目标。

### 4.2 不建议把 payload merge 作为默认优化继续补丁化

payload merge 当前 QPS 提升没有伴随 physical bytes 下降，且 recall 下滑。继续补丁化很容易在 exact rerank 输入上引入隐蔽错误。

如果再做，只能作为 page-contract correctness 实验，不能先进入主线。

### 4.3 不建议优先调低 `PostingTopRGlobal`

当前 `PostingTopRGlobal=192/128` 曾触发 `std::bad_function_call` 或不稳定行为。并且 topR 降低会同时影响 recall、payload pages、candidate count，容易掩盖并发语义问题。

应先完成 P0/P2，再做 topR 下探。

### 4.4 不建议现在进入 dynamic/KV 路径

当前 static M2/M3 仍未完成性能和并发语义收敛。dynamic 路径涉及 blob layout、mutation、checksum、merge/split/reassign，范围会明显扩大。

现阶段 dynamic 只保留 guardrail：unsupported 组合 fail-fast。

---

## 5. 更新后的阶段计划

### Phase A：稳定性与观测闭环

目标：确保结果语义稳定，所有瓶颈可解释。

步骤：

1. 固定 code-cache M2，禁用 chunk pruning；
2. 做 `SearchThreadNum=1/2/4/8` recall 稳定性 sweep；
3. 加 query-level candidate/page hash probe；
4. 加 coarse recall / miss-case 指标；
5. 确认 raw log 与 detailed stats 来源一致。

产出：

- 并发 recall 差异归因报告；
- coarse candidate recall 表；
- payload page locality 基线表。

### Phase B：payload physical pages 优化

目标：真正降低 payload physical pages，而不是只减少 logical payload 或请求数。

步骤：

1. 先只加 instrumentation：page reuse、candidate/page density、page span；
2. 分析 payload 候选分散原因；
3. 尝试 build-side payload reorder；
4. 严格验证 reorder 前后 payload bytes hash / exact distance；
5. 对比 physical pages 与 QPS。

产出：

- payload locality 改善前后对比；
- matched recall 下的 QPS / pages curve。

### Phase C：M3 chunk quality 与 pruning 重做

目标：把 M3 从 heuristic bytes reduction 推进到可解释的 recall/QPS tradeoff。

步骤：

1. 关闭 pruning 建 chunk baseline；
2. 输出 chunk radius / count / payload pages 分布；
3. 改 chunk 构造为 posting 内空间聚类；
4. 做 `t32/t64/t128` 对比；
5. 实现 heuristic 与 safe 两档 pruning 语义；
6. 对 miss query 输出 truth 被 prune 的具体 chunk。

产出：

- chunk quality 报告；
- heuristic pruning tradeoff 表；
- safe pruning 可行性判断。

### Phase D：参数 sweep 与默认档位选择

目标：在稳定实现上选择默认参数，而不是在 bug/瓶颈未清时调参。

步骤：

1. topR sweep：`64/256`、`96/384`、`128/512`；
2. matched recall 对比 legacy `ir32/64/96`；
3. 评估可选性能模式：低 recall / 高 QPS；
4. 评估默认模式：尽量贴近 legacy recall。

产出：

- 默认参数建议；
- 可选性能模式参数；
- 不推荐参数组合列表。

---

## 6. 当前推荐优先级总表

| 优先级 | 方向 | 类型 | 状态 | 为什么优先 / 不优先 | 成功标准 |
|---:|---|---|---|---|---|
| P0 | 并发 recall 稳定性 | correctness | **已完成 ✓** | `st=8` recall 明显低于 `st=1`，会污染所有 QPS 结论 | 同 query 候选/结果在不同并发下稳定 |
| P1 | payload physical page inflation | performance | 进行中 | metadata/code I/O 已压掉，payload pages 是唯一大头 | `PayloadPhysicalBytesRead` / `PagesRead` 下降且 recall 稳定 |
| P2 | coarse recall / miss-case 指标 | observability | 待开始 | 不知道 recall 损失来自 PQ、topR、prune 还是实现错误 | 能解释每类 miss 的来源 |
| P3 | M3 pruning 重做 | quality/performance | 待开始 | 当前 pruning 是 heuristic，t32 已伤 recall | safe/heuristic 分档清楚，truth prune 可观测 |
| P4 | code cache 稳定化 | performance guardrail | 待开始 | code I/O 已清零，不宜先回到高风险 async | `CodePhysicalBytesRead=0`，内存/load 成本可接受 |
| P5 | benchmark 记录规范 | process | 持续执行 | 防止再次误判 | raw log / stats / report 来源一致 |

---

## 7. 总体判断

方案不需要推倒重来，但下一步主线需要从“继续堆 M2/M3 功能”转为“稳定语义 + 攻 payload physical pages”。

当前最重要的判断是：

```text
metadata inflation：已基本解决
code physical inflation：code cache 路线已基本解决
payload physical inflation：当前最大性能瓶颈
SearchThreadNum>1 recall drop：已验证不存在（P0 完成 ✓）
M3 pruning：当前只能作为 heuristic 实验项
```

因此，推荐执行顺序是：

```text
P0 并发 recall 稳定性（已完成 ✓）
→ P1 payload physical page 优化（当前进行中）
→ P2 coarse/miss-case 观测
→ P3 M3 chunk/pruning 重做
→ P4 code cache 成本与稳定性收敛
→ P5 benchmark 规范持续执行
```
