# M4 Storage-Optimized SPANN Index 方案

日期：2026-05-03  
目标方向：Primary-Secondary / Route-Payload Split  
定位：Storage-optimized exact-safe SPANN profile，而不是默认 QPS 优化主线  
适用数据口径：strict `UInt8 + DEFAULT`，SIFT1M / SIFT10M，matched recall 优先

---

## 执行状态 (2026-05-03)

| 阶段 | 状态 | 结果 |
|------|------|------|
| A0: Legacy baseline replay | ✅ 完成 | 已有数据 |
| A1: Storage-only sidecar builder | ✅ 完成 | 存储减少 **73.1%** |
| A2: Offline query simulator | ✅ 完成 | VIDOrder: 2140, CoHit: 1257 pages/query (vs Legacy 119) |
| A3: Pure online M4 prototype | ❌ 不执行 | A2 结果不符合决策规则 |
| A4: Hybrid online M4 prototype | ❌ 不执行 | A2 结果不符合决策规则 |

### 最终结论

**M4 仅作为 Storage-only Sidecar，不实现在线查询路径。**

**原因**：
1. ✅ 存储目标达成：Sidecar 减少 73.1% (>=50% 目标)
2. ❌ 在线查询不可行：Primary pages/query = 1257 vs Legacy = 119 (超出 10x)
3. 根因：Legacy 的 Posting 结构提供 Spatial Locality，M4 Primary Store 打破这种 locality

**推荐用途**：
- 归档存储 (Archive)
- 分发打包 (Deploy Package)
- 冷数据存储 (Cold Storage)

---

## 1. 背景与问题定义

当前 SPANN legacy static index 中，posting record 通常包含：

```text
[VID][full vector payload]
```

由于 replica / closure 机制，同一个 VID 可能被物理写入多个 head 对应的 posting list 中。这样可以提高边界召回，但带来明显的 full payload 存储放大。

已有实验显示：

- storage-level duplication 很高；
- query-level duplicate ratio 不高；
- pure primary-secondary 作为 QPS 优化方向失败；
- 但如果目标改为存储压缩，并允许轻微查询性能下降，该方向仍然有很大价值。

因此，本方案不再把 M4 定义为：

```text
primary-secondary performance optimization
```

而是重新定义为：

```text
M4 Storage-Optimized Exact-Safe SPANN Format
```

其核心目标是：

> 在不损失 recall / result correctness 的前提下，大幅降低索引空间占用；查询性能可以轻微下降，但必须可控、可配置、可回退。

---

## 2. 目标与非目标

### 2.1 目标

本方案目标包括：

1. **降低索引空间占用**
   - 优先目标：index footprint 降低 `>= 50%`
   - 理想目标：index footprint 降低 `60%~75%`

2. **保持查询正确性**
   - exact-safe 模式下 candidate set 不变；
   - full vector exact distance 不变；
   - Recall@10 与 legacy matched；
   - result hash 在 deterministic 测试下应保持一致或仅存在已解释的 tie-breaking 差异。

3. **控制查询性能下降**
   - Performance profile 不变，仍使用 legacy；
   - Storage-optimized online profile 的 QPS drop 目标 `<= 5%~10%`；
   - Capacity profile 可接受更大 QPS 下降，但必须明确标注。

4. **保持 baseline 兼容**
   - legacy index 不被破坏；
   - M4 作为 sidecar / optional format；
   - 不支持 silent fallback；
   - 未启用 M4 时结果、性能、文件格式与 legacy 完全一致。

5. **支持多 profile 产品化**
   - Performance：legacy full-payload postings；
   - Balanced：hybrid payload-budgeted primary-secondary；
   - Capacity：pure primary-secondary one-copy payload。

---

### 2.2 非目标

本方案不追求：

1. 默认提升 QPS；
2. 替代 legacy official baseline；
3. 引入 approximate code-first TopR；
4. 引入 recall-risk chunk pruning；
5. 与 M2 two-stage posting 绑定；
6. 在第一版中解决 routing / cluster margin 问题。

---

## 3. 核心设计

### 3.1 Legacy 当前形态

```text
Posting_A:
  [VID_1][payload_1]
  [VID_2][payload_2]
  ...

Posting_B:
  [VID_1][payload_1']   # same VID, duplicated payload
  [VID_9][payload_9]
  ...
```

特点：

- 每个 posting 直接包含 full payload；
- 查询读取 posting 后可以立即计算距离；
- posting-level I/O 是 4KB~16KB 小块随机读；
- pipeline 成熟，性能强；
- 存储空间因 replica 机制明显膨胀。

---

### 3.2 M4 Pure Primary-Secondary

```text
Route Posting:
  [VID_1]
  [VID_2]
  ...

Primary Payload Store:
  VID_1 -> payload_1
  VID_2 -> payload_2
  ...

Location Table:
  VID -> payload_offset
```

查询流程：

```text
1. Head search 得到 legacy 相同 posting IDs。
2. 读取 route posting。
3. 得到 VID 集合。
4. query-local dedupe。
5. 根据 VID -> primary payload offset 生成 read plan。
6. 批量读取 primary payload pages。
7. full vector exact distance。
8. 返回 Top-K。
```

优点：

- 存储节省最大；
- full payload 全局只存一份；
- exact-safe，不改变候选集合。

缺点：

- route read + primary payload read 形成两阶段 I/O；
- primary payload locality 可能很差；
- pages/query 可能远高于 legacy；
- 不适合作为默认 performance profile。

---

### 3.3 M4 Hybrid Payload Budget

Pure one-copy 过激。更现实的 online 方案是 hybrid：

```text
Primary copy:
  always keep full payload

Hot / query-critical secondary:
  keep full payload in posting

Cold secondary:
  route-only
```

即：

```text
Posting record can be:
  FullPayloadRecord = [VID][payload]
  RouteOnlyRecord   = [VID][payload_location_ref]
```

查询时：

```text
if record has local full payload:
    compute distance directly
else:
    add VID to primary payload fetch plan
```

目标：

- 保留 performance-critical 副本；
- 剥离 cold secondary payload；
- 在空间节省和查询性能之间折中。

---

## 4. Baseline 兼容性 Review

### 4.1 文件格式兼容

推荐使用 sidecar，不修改 legacy 文件：

```text
ssdIndex
ssdIndex.m4.meta
ssdIndex.m4.route
ssdIndex.m4.route.dir
ssdIndex.m4.payload
ssdIndex.m4.loc
ssdIndex.m4.checksum
```

兼容原则：

| 场景 | 行为 |
|---|---|
| M4 disabled | 完全走 legacy path |
| M4 enabled 且 sidecar 完整 | 使用 M4 path |
| M4 enabled 但 sidecar 缺失 | fail loudly |
| M4 sidecar checksum mismatch | fail loudly |
| legacy index only | 正常运行 |
| M4 sidecar present but disabled | 不影响 legacy |

不允许：

```text
EnableM4=true 但 sidecar 不可用时 silent fallback 到 legacy。
```

原因：

- benchmark 会被污染；
- 用户会误以为 M4 生效；
- correctness / performance 结论不可信。

---

### 4.2 查询结果兼容

M4 exact-safe mode 必须满足：

```text
same visited postings
same candidate VID set
same payload values
same distance function
same top-k semantics
```

允许的差异：

- floating tie-breaking；
- equal-distance vectors 的排序差异；
- 多线程非确定性下的稳定性噪声。

必须记录：

```text
result_hash_legacy
result_hash_m4
recall_legacy
recall_m4
candidate_set_diff_count
payload_checksum_mismatch_count
```

成功要求：

```text
candidate_set_diff_count = 0
payload_checksum_mismatch_count = 0
Recall@10 delta within run-to-run noise
```

---

### 4.3 参数兼容

新增参数建议：

```ini
EnableM4StorageOptimizedIndex=false
M4Profile=Performance|Balanced|Capacity
M4FailOnMissingSidecar=true

M4RouteRecordFormat=VIDOnly
M4PayloadStoreLayout=VIDOrder|PrimaryPostingOrder|CoHitOrder
M4HybridKeepPolicy=None|HotPosting|RecallCritical|Budgeted

M4StorageBudgetRatio=0.50
M4MaxQPSDropRatio=0.10
M4PayloadChecksum=true
```

默认值：

```text
EnableM4StorageOptimizedIndex=false
M4Profile=Performance
```

即默认行为完全等价 legacy。

---

## 5. 实现复杂度评估

### 5.1 总体复杂度

| 模块 | 复杂度 | 风险 |
|---|---:|---|
| Sidecar builder | 中 | route/payload/location 一致性 |
| Route posting reader | 中 | offset / alignment / page span |
| Location table | 中 | VID->offset 正确性、内存占用 |
| Primary payload read planner | 中高 | random page fanout |
| Hybrid keep policy | 中高 | 策略选择和可解释性 |
| Query path integration | 中高 | correctness / latency |
| Checksum & validation | 中 | 工程量但必要 |
| Benchmark harness | 中 | 需要防 silent fallback |

### 5.2 可控性判断

实现复杂度整体 **可控但不能低估**。

可控原因：

- 不需要改 head index；
- 不需要改 distance function；
- 不需要 approximate code；
- 不需要改变 candidate set；
- 不需要做 recall-risk pruning；
- 可以 sidecar 化，与 legacy 解耦。

主要风险：

- primary payload page fanout 过大；
- location table 错误导致 silent correctness bug；
- hybrid policy 复杂化；
- query path 读 plan 与 batch I/O 集成复杂；
- sidecar 和 legacy index 版本不匹配。

---

## 6. 预期收益

### 6.1 Pure Capacity Profile

Hypothesis:

```text
Pure primary-secondary one-copy payload 可以把 index footprint 降低 60%~75%，
但查询性能可能明显下降。
```

预期：

| 指标 | 预期 |
|---|---:|
| Index footprint reduction | `60%~75%` |
| Recall | matched |
| Candidate set | identical |
| QPS | 可能下降 `20%~50%+` |
| P99 | 可能明显上升 |
| pages/query | 可能高于 legacy |
| 适用场景 | cold / capacity-first / offline serving |

### 6.2 Hybrid Balanced Profile

Hypothesis:

```text
保留 hot/query-critical secondary full payload，
只剥离 cold secondary payload，
可以实现 30%~50% 空间下降，同时 QPS drop 控制在 5%~10%。
```

预期：

| 指标 | 预期 |
|---|---:|
| Index footprint reduction | `30%~50%` |
| Recall | matched |
| QPS drop | `<= 5%~10%` |
| P99 increase | `<= 10%~15%` |
| route + primary pages | `<= legacy_pages × 1.1` |
| 适用场景 | storage-sensitive online serving |

### 6.3 Storage-only Sidecar

Hypothesis:

```text
M4 compact sidecar 可以作为离线压缩 / 分发 / 冷存储格式，
不接 query path 时可以安全获得空间收益。
```

预期：

| 指标 | 预期 |
|---|---:|
| Archive footprint reduction | `60%~75%` |
| Query performance | no impact |
| Recall | no impact |
| Restore time | 需测量 |
| 适用场景 | archive / deploy package / cold backup |

---

## 7. Hypotheses

### H0: Storage saving hypothesis

M4 可以显著减少索引空间。

Success:

```text
Index footprint reduction >= 50%
Payload storage reduction >= 60%
Route + loc + metadata overhead <= saved payload bytes × 25%
```

Failure signal:

```text
metadata / alignment / route overhead 吃掉超过 40% 的 payload saving
sidecar 总大小下降 < 30%
payload checksum 或 VID coverage 无法稳定验证
```

Expected ablation:

| Ablation | Expected observation |
|---|---|
| pure one-copy | 最大空间节省 |
| hybrid keep 25% secondary payload | 空间节省下降，但 query pages 改善 |
| hybrid keep 50% secondary payload | 空间节省进一步下降，query 接近 legacy |
| storage-only sidecar | 最大安全收益，但不影响 online footprint unless deployed as compact-only |

---

### H1: Exact-safe query compatibility hypothesis

M4 exact-safe path 可以保持 legacy 查询结果一致。

Success:

```text
candidate_set_diff_count = 0
payload_checksum_mismatch_count = 0
Recall@10 delta <= run-to-run noise
result_hash identical or tie-difference explained
```

Failure signal:

```text
同一 query 出现 candidate VID 缺失
payload checksum mismatch > 0
Recall 出现稳定下降
同配置多次运行 result hash 差异超出 legacy 自身波动
```

Expected ablation:

| Ablation | Expected observation |
|---|---|
| VID-only route + exact payload | recall 与 legacy 一致 |
| hybrid local payload + primary fallback | recall 与 legacy 一致 |
| checksum disabled | performance 略好，但不能作为 correctness 验收 |
| sidecar version mismatch | fail loudly |

---

### H2: Pure primary-secondary performance regression hypothesis

Pure one-copy primary payload 会节省空间，但 query I/O 可能恶化。

Success for capacity profile:

```text
Index footprint reduction >= 60%
Recall matched
QPS drop explicitly quantified
P99 regression explicitly quantified
```

Failure signal:

```text
QPS drop > 50% 且无 clear capacity use case
P99 increase > 2x
route + primary pages/query > legacy × 3
primary payload read wait dominates total latency
```

Expected ablation:

| Ablation | Expected observation |
|---|---|
| pure primary payload by VID order | 空间最好，locality 最差 |
| primary payload by primary posting order | locality 可能略好 |
| primary payload by query co-hit order | primary pages/query 应下降 |
| add primary payload page cache | 若 primary page reuse 低，收益有限 |
| sort read plan by physical offset | request count 可能下降，但 overread 可能增加 |

---

### H3: Hybrid payload budget hypothesis

保留部分 secondary payload 可以显著降低 performance regression。

Success:

```text
Index footprint reduction >= 30%
QPS drop <= 10%
P99 increase <= 15%
route + primary pages/query <= legacy_pages × 1.1
```

Failure signal:

```text
为达到 QPS drop <= 10%，必须保留 >80% secondary payload
空间下降 < 20%
policy 对不同 st/ir/query-set 不稳定
hot-secondary keep policy 导致 build 结果不可复现
```

Expected ablation:

| Ablation | Expected observation |
|---|---|
| keep hot posting secondary | QPS 改善，但空间下降受限 |
| keep recall-critical secondary | recall/hash 更稳，QPS 可能改善 |
| keep top X% queried secondary | 对 benchmark query 有利，需 held-out 验证 |
| budget 25/50/75% payload kept | 空间与 QPS 形成 Pareto |
| cold secondary route-only | 空间收益主要来源 |

---

### H4: Query-path storage saving does not equal global storage saving

全局副本多不代表 query path 中可省 I/O 多。

Success of hypothesis:

```text
query-level duplicate payload ratio remains much lower than storage-level replica ratio
primary pages/query explains QPS regression better than storage saving
```

Failure signal:

```text
query-level duplicate payload ratio >= 30%
primary pages/query <= legacy pages/query
M4 online QPS matches or exceeds legacy
```

Expected ablation:

| Ablation | Expected observation |
|---|---|
| measure storage replica ratio | high |
| measure query duplicate ratio | much lower |
| simulate one-primary pages/query | can be much higher than legacy |
| hybrid keep hot secondaries | bridges gap between storage and query path |

---

### H5: SIFT10M compatibility hypothesis

SIFT10M 不一定比 SIFT1M 更适合 M4 online query path。

Success of hypothesis:

```text
SIFT10M query-level duplicate ratio remains low
cross-query reuse remains low
M4 pure online path does not improve QPS
```

Failure signal:

```text
SIFT10M duplicate payload ratio >= 30%
primary page cache hit rate high enough to offset fanout
M4 hybrid achieves >=50% storage reduction with <=10% QPS drop
```

Expected ablation:

| Ablation | Expected observation |
|---|---|
| SIFT1M pure M4 | space saving large, online QPS likely poor |
| SIFT10M pure M4 | space saving large, online QPS likely poor |
| SIFT10M hybrid | more relevant than pure |
| high-concurrency st16 | primary page cache may help, must verify |

---

## 8. Ablation Plan

### A0: Legacy baseline replay

Purpose:

```text
Establish stable legacy reference.
```

Configs:

```text
SIFT1M: st=1/4/8/16, ir=64, pl=4
SIFT10M: st=8/16, ir=64, pl=4
```

Metrics:

```text
QPS
Recall@10
avg/p95/p99 latency
pages/query
requested bytes/query
posting_elements_raw
duplicate ratio
index footprint
```

Expected:

```text
Matches existing baseline within run-to-run noise.
```

Failure signal:

```text
QPS variance >5%
Recall variance beyond known noise
requested bytes/query mismatch >3%
```

---

### A1: Storage-only sidecar builder

Purpose:

```text
Measure true space saving without query-path risk.
```

Variants:

```text
pure VID-only route
route + debug primaryPostingID
route alignment variants
loc table 8B/12B/16B
```

Expected:

```text
index/sidecar footprint reduction >=50%
payload checksum valid
VID coverage complete
```

Failure signal:

```text
sidecar route/loc overhead too large
checksum mismatch
VID coverage mismatch
build time regression >50%
```

---

### A2: Offline query simulator

Purpose:

```text
Estimate online query page cost before implementation.
```

Simulate:

```text
legacy_pages/query
route_pages/query
primary_pages/query
route+primary_pages/query
primary_candidates/page
```

Variants:

```text
VID-order payload layout
primary-posting-order layout
co-hit layout
hybrid keep 25/50/75%
```

Expected:

```text
pure one-copy saves storage but route+primary pages likely high
hybrid forms storage/QPS Pareto
```

Failure signal:

```text
all hybrid variants require route+primary pages > legacy ×1.5
primary candidates/page remains very low
held-out trace differs greatly from train trace
```

---

### A3: Pure online M4 prototype

Purpose:

```text
Validate exact-safe query path and measure true regression.
```

Expected:

```text
Recall/hash matched
Index footprint reduction >=60%
QPS regression quantified
```

Failure signal:

```text
candidate diff >0
payload checksum mismatch >0
QPS drop >50%
P99 >2x legacy
primary read wait dominates
```

---

### A4: Hybrid online M4 prototype

Purpose:

```text
Find usable online storage/QPS tradeoff.
```

Variants:

```text
keep 25% payload
keep 50% payload
keep 75% payload
hot-posting keep
recall-critical keep
budgeted keep
```

Expected:

```text
Pareto curve:
  more payload kept -> higher QPS, less space saving
  less payload kept -> lower storage, worse QPS
```

Success target:

```text
>=30% footprint reduction
<=10% QPS drop
<=15% P99 increase
recall/hash matched
```

Failure signal:

```text
no variant reaches >=30% footprint reduction with <=10% QPS drop
policy overfits train query set
held-out QPS regression much worse
```

---

### A5: Cache and read-plan ablation

Purpose:

```text
Determine whether primary payload random fetch can be mitigated.
```

Variants:

```text
no primary cache
primary page cache 64MB/256MB/512MB
physical-offset sorted read plan
gap-based opportunistic merge
prefetch next primary pages
```

Expected:

```text
cache helps only if primary page reuse exists
physical sort may reduce request count but increase overread
```

Failure signal:

```text
cache hit high but QPS not improved due to lock/copy overhead
overread bytes exceed saved request overhead
P99 worsens from cache lock contention
```

---

## 9. Review Checklist

### Baseline compatibility

- [ ] M4 disabled equals legacy output.
- [ ] Legacy index can be loaded without sidecar.
- [ ] M4 enabled but sidecar missing fails explicitly.
- [ ] Sidecar version and checksum verified.
- [ ] Candidate set equality test exists.
- [ ] Recall and result hash regression tests exist.

### Implementation complexity

- [ ] Sidecar builder separated from query path.
- [ ] Query simulator before online implementation.
- [ ] Route reader and payload reader independently tested.
- [ ] VID->offset table validated by checksum.
- [ ] Hybrid policy deterministic.
- [ ] No silent fallback in benchmark.

### Expected improvement

- [ ] Pure capacity expected footprint reduction written.
- [ ] Hybrid expected footprint/QPS/P99 tradeoff written.
- [ ] Failure thresholds explicit.
- [ ] Each ablation has expected observation and independent failure signal.
- [ ] No claim of QPS improvement without pages/read-wait evidence.

---

## 10. Decision Rules

### Continue to online pure M4 only if:

```text
storage-only footprint reduction >=60%
offline pure route+primary pages/query <= legacy ×3
checksum/candidate simulation clean
```

Otherwise pure M4 remains storage-only / capacity profile.

### Continue to online hybrid M4 only if:

```text
offline hybrid shows:
  footprint reduction >=30%
  route+primary pages/query <= legacy ×1.1~1.2
```

Otherwise do not implement online hybrid.

### Productize hybrid profile only if:

```text
>=30% footprint reduction
<=10% QPS drop
<=15% P99 increase
Recall/hash matched
no held-out regression
```

### Archive direction if:

```text
all hybrid variants fail storage/QPS Pareto
or correctness validation remains fragile
or primary read fanout dominates despite layout/cache
```

---

## 11. Final Expected Outcome

The most likely outcome is:

```text
Pure primary-secondary:
  excellent storage saving
  unacceptable default-query performance regression

Hybrid payload-budgeted:
  possible 30%~50% storage saving
  performance regression may be controllable

Storage-only sidecar:
  high confidence value for archive/distribution/cold storage
```

Therefore, the recommended path is:

```text
1. Build storage-only sidecar first.
2. Run offline query simulator.
3. Only implement online query path if hybrid simulator shows viable Pareto.
4. Keep legacy as default performance profile.
5. Treat M4 as storage profile, not performance profile.
```
