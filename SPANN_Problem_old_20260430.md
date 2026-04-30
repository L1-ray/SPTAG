# SPANN 算法问题分析与解决方案（修订版）

> 本文档是在原有 `SPANN_Problem.md` 基础上的修订版。修订目标是：保留原文对 SPANN posting list 读放大、边界点复制、高并发 I/O 拥塞等问题的主线判断，同时降低过度绝对化表述，补充可验证指标，修正 PQ/rerank 与 chunk 下界剪枝的数学边界，并给出更适合工程落地的分阶段改造路线。

---

## 3.1 磁盘 I/O 读放大与多线程并发拥塞

SPANN 的核心设计是将较小的 head/centroid index 放在内存中，将大规模 posting lists 放在 SSD 上。查询时先在内存 head index 中定位若干候选 head，再读取这些 head 对应的 posting list，并在 posting 内做细粒度距离计算。这个设计用较少的随机访问次数换取较大的顺序读取块，在单线程或低并发条件下可能表现很好。

但在高维、高 recall 和高并发场景中，这一设计会暴露明显的读放大问题。关键原因不是 SSD 本身太慢，而是当前查询路径通常以 **posting list** 作为最小 I/O 单位，而不是以“真正有希望进入最终 Top-K 的少量候选向量”作为最小 I/O 单位。

当内存索引定位到若干候选 posting 后，系统往往需要将这些 posting 中的大量向量 payload 读入内存，然后逐条计算距离。最终进入 Top-K 或 rerank 候选集的向量通常只占被扫描元素的一小部分，但系统已经为整段 posting 支付了读取、传输、解压和扫描成本。这个比例不能在没有实验数据时固定写成“1% 有用、99% 无用”，更准确的表述应当是：

```text
useful_candidate_ratio
= rerank_candidate_count / scanned_posting_element_count
```

如果这个比例长期很低，就说明 posting 读取与最终有效候选之间存在严重错配。

在多线程环境下，读放大会被进一步放大。多个查询线程同时读取多个大 posting，底层 SSD 看到的是大量并发大块读取请求。若系统缺少全局 bytes-in-flight 控制、热点 posting 请求合并、page-level 去重和大小感知调度，SSD 控制器内部队列会快速堆积，CPU 线程则在等待 I/O 期间空转。此时增加查询线程数并不一定提升吞吐，反而可能更快把存储设备推入过载区。

因此，原文中“I/O Jamming”的方向判断是合理的，但建议将其作为本文诊断术语使用，而不是当作严格学术概念。更工程化的说法是：

> SPANN 在高并发下的核心风险，是 posting 级大块读取在 SSD 侧形成排队拥塞，使 QPS 随线程数增加过早平顶，P95/P99 延迟显著上升。

---

## 3.2 聚类边界复制、索引膨胀与负载失衡

SPANN 为了提高聚类边界附近的召回，会将边界点或 closure points 复制到多个相邻 posting list 中。这个机制可以提高 recall，但代价是存储、构建和查询路径上的重复开销。

需要注意的是，原文中“高维空间中绝大多数点都在边界上”的说法过于绝对。更准确的判断是：

> 在名义维度很高、intrinsic dimension 较高、cluster margin 较小的数据上，query 到多个 head 的距离可能非常接近，head routing 的区分度会下降。为了保持 recall，系统往往需要访问更多 posting 或增加更多 secondary assignment，从而放大 I/O 和存储成本。

边界点复制最直接的后果包括：

1. **索引尺寸膨胀**  
   同一向量 payload 被写入多个 posting，重复的是最重的 full vector 或高保真 payload，而不仅是轻量元数据。

2. **构建成本增加**  
   build 阶段不仅要完成 primary assignment，还要处理 closure / secondary assignment，导致额外写放大和排序、合并、压缩成本。

3. **posting 负载不均**  
   高密度区域交界处的 head 可能收到大量 secondary vectors，形成超大 posting 或热点 posting。

4. **查询期重复读取**  
   同一 VID 可能通过多个 posting 被读到。如果缺少 VID-level 去重和 page-level 合并，重复 payload 会进一步浪费带宽和 cache。

原文中“边界复制破坏底层数据结构的导航性、导致连通性坍塌”的说法需要降调。边界点物理复制更直接影响的是 posting 尺寸、I/O 放大、负载均衡和缓存效率；它是否进一步损害 head graph 的连通性，需要通过图连通分量、搜索路径长度、head degree 分布、miss case 等指标单独验证，不能直接作为确定结论。

更稳妥的表述是：

> 边界点复制是 SPANN 保 recall 的有效手段，但它将召回问题转化为存储和带宽问题。在高维和高并发场景下，这种代价会被显著放大，尤其容易形成大 posting、热 posting 和重复 payload 读取。

---

## 3.3 结合当前实现的结构性根因分析

结合原文提到的 `SPANN::Index`、`ExtraStaticSearcher`、`ExtraDynamicSearcher` 和 `ExtraFileController` 这类实现路径来看，当前问题不宜简单归因于 page limit、线程数或 cache 大小等参数，而应从 posting list 的物理组织方式分析。

### 3.3.1 当前查询路径的本质：posting 是最小 I/O 单位

当前 SPANN 查询路径可以抽象为：

1. 在内存 Head Index 上搜索，得到若干候选 head。
2. 根据 head ID 定位对应 posting list。
3. 从磁盘或缓存中读取 posting list。
4. 对 posting 内元素顺序扫描，逐条计算距离或进入 rerank 流程。

因此，单次查询消耗的磁盘字节量可近似表示为：

```text
bytes_read_per_query
≈ Σ bytes(fetched_posting_i)
```

更细化后应写成：

```text
bytes_read_per_query
≈ directory_bytes
 + code_bytes
 + payload_bytes
 + metadata_bytes
 + duplicate_read_bytes
```

当前原始模式的问题是 `payload_bytes` 占比过高，而且 `duplicate_read_bytes` 在边界复制和多 query 并发下可能进一步放大。

当向量维度升高时，每条 full vector 的字节数线性增加。如果 posting 内元素数没有随维度同比例下降，单个 posting 的页数会快速膨胀。此时即使每次读取仍然是顺序 I/O，整体也可能被 SSD 带宽和队列深度限制。

### 3.3.2 静态 SPANN 的压缩并未根本改变读放大模式

当前静态路径中的若干优化，例如：

- `EnableDeltaEncoding`
- `EnablePostingListRearrange`
- `EnableDataCompression`

能够减少落盘体积，改善顺序读取时的字节压缩率，也可能降低冷存储成本。但如果查询期仍然需要将整段 posting 读出、解压并扫描，那么这些优化没有改变“posting 作为最小读取单位”的根本模式。

也就是说，这类压缩主要解决：

```text
每页有多大
```

但没有根本解决：

```text
为什么必须把这些页全读出来
```

要真正降低查询期读放大，需要让压缩表示直接参与粗筛，或者让 posting 被进一步切分成可裁剪、可调度的子结构。

### 3.3.3 动态 SPANN / SPFresh 路径的主要价值是更新能力，不是查询期结构性减负

动态/SPFresh 路径的主要目标是支持在线更新和局部重平衡。若 posting value 中仍然直接保存类似：

```text
[VID][Version][Vector Payload]
```

这样的完整记录，那么查询 `MultiGet` 之后仍然需要遍历完整 posting 并计算距离。

因此，动态路径的核心价值应理解为：

```text
提升可更新性、降低全量 rebuild 频率
```

而不是天然解决：

```text
查询期整表读取、整表扫描和高维 payload 过重
```

在高维和高并发场景下，动态路径同样可能遇到：

- 单个 posting value 过大；
- 热点 posting 成为并发瓶颈；
- cache miss 后仍需整段读取；
- 更新带来的版本管理和碎片整理成本。

### 3.3.4 多线程性能衰退的核心：缺少跨 query 的字节级调度

当前模式中，每个查询线程基本独立完成：

- 选择自己的 head；
- 发起自己的 posting 读取；
- 等待自己的数据返回；
- 在本线程内完成扫描和 rerank。

这种设计在低并发下简单有效，但在高并发下会产生典型存储系统问题：

```text
并发 query 数增加
→ 同时在飞行的 posting bytes 增加
→ SSD queue depth 上升
→ I/O latency 上升
→ CPU 等待 I/O
→ QPS 过早平顶，P99 拉长
```

因此，瓶颈不是“线程不够多”，而是“缺少全局 I/O admission control”。更准确的诊断指标应包括：

```text
bytes_in_flight
pages_in_flight
SSD_queue_depth
read_latency_by_size
CPU_iowait
per_query_io_wait_time
```

### 3.3.5 边界点复制是物理复制，不是轻量引用

如果 `ReplicaCount` 的语义是在多个 head 对应的 posting 中物理写入同一向量，那么其代价不仅是多几条索引项，而是 full payload 的重复存储和重复读取。

这会直接造成：

- **空间放大**：同一向量在多个 posting 中重复出现；
- **构建放大**：build 阶段需要写入和维护多个副本；
- **查询放大**：多个 posting 命中同一 VID 时可能重复读取；
- **缓存污染**：同一语义对象占用多份 cache 空间；
- **热点加剧**：高密度边界区域的 posting 更容易过大。

因此，副本机制应从“完整 payload 复制”逐步转向“primary 保存完整 payload，secondary 只保存轻量路由信息”。

### 3.3.6 当前实现已经暴露出正确方向，但还没有完全落成主路径

现有参数中已经出现一些关键线索，例如：

- `EnableADC`
- `Rerank`
- `Quantizer`

这说明系统已经意识到“两阶段搜索”的必要性，即：

1. 先用较小表示做粗筛；
2. 再对少量候选做精排。

但如果磁盘 posting 结构仍以完整 payload 为主，ADC / rerank 就只是局部能力，而不是完整的 code-first posting 主路径。真正的目标应是让 posting 本身成为：

```text
directory → compact code block → full payload block
```

而不是单一的 full-vector record 序列。

### 3.3.7 高维问题的更准确表述：低 cluster margin，而不是“维度高必然失败”

高维向量不一定天然不可聚类。真正危险的是以下组合：

- intrinsic dimension 高；
- query 到多个 head 的距离 margin 小；
- embedding 分布各向异性明显；
- cluster 之间重叠严重；
- 为达到固定 recall 需要访问的 head 数持续增加。

因此，需要引入以下指标判断是否存在 routing 退化：

```text
cluster_margin(q) = distance(q, head_2) - distance(q, head_1)

true_nn_posting_rank
= 真实近邻所在 posting 在 head 排序中的名次

heads_needed_for_target_recall
= 达到目标 recall 需要访问的 head 数
```

如果大量 query 的 `cluster_margin` 很小，且 true nearest neighbors 经常落在排序较后的 posting 中，那么问题已经不只是 posting 太大，而是首层 routing 区分度不足。

---

## 3.4 必须补齐的观测指标

在进行结构改造之前，应先补齐观测面。否则很难判断瓶颈到底来自 payload 过大、posting 过长、重复读取、SSD 队列拥塞，还是首层 routing 失效。

### 3.4.1 系统层指标

```text
1. bytes_read_per_query
2. pages_read_per_query
3. postings_touched_per_query
4. posting_elements_scanned_per_query
5. useful_candidate_ratio
6. duplicate_vector_read_ratio
7. SSD queue depth
8. SSD read bandwidth utilization
9. CPU iowait / idle time
10. per_query_io_wait_time
11. P50 / P95 / P99 latency
12. QPS vs thread_count curve
```

其中最关键的是：

```text
useful_candidate_ratio
= rerank_candidate_count / scanned_posting_element_count
```

以及：

```text
duplicate_vector_read_ratio
= duplicate_vid_read_count / total_vid_read_count
```

这两个指标可以直接衡量“读了多少无效数据”和“复制机制造成多少重复读取”。

### 3.4.2 索引结构指标

```text
1. posting size distribution: P50 / P90 / P99 / max
2. posting page count distribution
3. replica count distribution
4. primary / secondary assignment ratio
5. head hotness distribution
6. boundary vector ratio
7. posting cache hit ratio by bytes
8. code cache hit ratio by bytes
9. chunk skip ratio
10. secondary assignment contribution to recall
```

注意，cache 命中率应按字节和收益统计，而不能只看对象数量。一个巨大 posting 的一次命中，可能会挤掉大量更小、更高复用价值的 code block 或 directory page。

### 3.4.3 召回与路由指标

```text
1. recall@10 / recall@100
2. coarse candidate recall
3. rerank candidate recall
4. true_nn_posting_rank
5. heads_needed_for_target_recall
6. cluster_margin distribution
7. miss case 中真实近邻落在哪个 posting / chunk
```

特别需要区分：

```text
coarse_candidate_recall
```

和：

```text
final_recall
```

因为 rerank 只能修正候选集内部排序。如果真实近邻没有进入粗筛候选集，rerank 无法补救。

---

## 3.5 可行解决方案：从调参转向结构化改造

如果继续围绕 `PostingPageLimit`、线程数、cache 大小等参数做微调，只能推迟瓶颈出现。真正有效的路线应当把 posting 改造成可分层、可裁剪、可调度的数据结构。

推荐的目标形态是：

```text
Posting
  ├── Directory
  │     ├── chunk_id
  │     ├── centroid / radius / bound info
  │     ├── count
  │     ├── code_offset
  │     └── payload_offset
  │
  ├── Compact Code Blocks
  │     ├── VID
  │     ├── PQ / OPQ / SQ / residual code
  │     └── optional primary pointer
  │
  └── Payload Blocks
        ├── full vector
        ├── high fidelity vector
        └── version / metadata
```

### 3.5.1 M0：先做观测闭环

这是所有改造的前置步骤。至少需要在查询链路中记录：

```text
query_id
visited_heads
fetched_postings
fetched_pages
bytes_read
scan_count
rerank_count
duplicate_vid_count
io_wait_time
cpu_distance_time
final_recall
```

没有这些指标，后续两阶段 posting、chunk 化、I/O 调度都无法评估真实收益。

### 3.5.2 M1：引入全局 I/O 调度与细粒度 cache

从工程落地收益/复杂度比看，全局 I/O 调度应提前。它不一定改变单 query 的最佳情况，但能明显改善高并发稳定性。

调度器至少应管理：

```text
1. bytes-in-flight budget
2. pages-in-flight budget
3. per-query I/O budget
4. large-posting admission control
5. hot posting request coalescing
6. page-level dedup
7. request priority
8. timeout / degradation policy
```

推荐优先级顺序：

```text
directory reads
> compact code reads
> small hot chunk reads
> payload rerank reads
> large cold posting reads
```

同时，cache 粒度应从“整个 posting”下降到：

- posting directory；
- hot chunk metadata；
- compact code block；
- hot payload page；
- primary vector page。

缓存评价指标不应只看 hit ratio，而应看：

```text
benefit_per_cached_byte
= saved_io_time / cached_bytes
```

这样可以避免巨大 posting 挤占大量 cache，但实际单位容量收益很低。

### 3.5.3 M2：引入两阶段 posting 结构

两阶段 posting 是最重要的结构性改造。目标不是单纯压缩落盘体积，而是让查询期可以先在 compact code 上完成粗筛，只有少量高价值候选才触发 full payload 读取。

粗筛层可以选择：

- PQ；
- OPQ；
- SQ / int8 scalar quantization；
- residual quantization；
- product residual code；
- head-aware 或 chunk-aware residual code。

查询路径变为：

```text
1. 读取 compact code block。
2. 使用 ADC 或其他近似距离快速打分。
3. 每个 posting / chunk 只保留 top-r 候选。
4. 跨 posting 合并候选并去重。
5. 按 payload page 排序后批量读取 full vector。
6. 对候选做精确 rerank。
```

I/O 模型从：

```text
read_cost ≈ Σ full_posting_bytes
```

变成：

```text
read_cost ≈ Σ code_block_bytes
          + Σ selected_payload_page_bytes
```

该方案的关键约束是：

1. **必须度量 coarse_candidate_recall**  
   压缩表示有量化误差。如果真实 Top-K 在粗筛阶段被漏掉，rerank 无法恢复。

2. **payload fetch 必须 page-aware**  
   如果每个候选都触发一次随机小读，收益可能被 NVMe 最小读取粒度和随机 I/O 放大抵消。候选应按 primary page 排序、批量读取、跨 query 合并。

3. **compact code 应能直接参与打分**  
   如果压缩后仍然必须完整解压才能计算距离，那么它只是文件压缩，不是查询语义压缩。

### 3.5.4 M3：将 posting 改造成可裁剪的 chunk / sub-posting

chunk 化解决的是“整张 posting 太大、不可裁剪”的问题。它与两阶段 posting 正交，可以组合使用。

推荐结构：

```text
Posting Directory
  ├── Chunk_1: centroid, radius, count, code_offset, payload_offset
  ├── Chunk_2: centroid, radius, count, code_offset, payload_offset
  └── ...

Chunk_i
  ├── compact codes
  └── payload references / payload block
```

查询路径：

```text
1. 读取 posting directory。
2. 对每个 chunk 计算下界或启发式 bound。
3. 跳过不可能进入候选集的 chunk。
4. 对剩余 chunk 读取 compact code。
5. 粗筛、合并、去重、rerank。
```

数学上需要注意：

- 对 L2 等满足三角不等式的 metric distance，如果 `radius` 是 chunk 内向量到 chunk centroid 的真实上界，则：

```text
||q - x|| >= ||q - c|| - radius
```

可作为安全下界。

- 对 cosine / inner product，不能直接套用上述 L2 下界。需要重新推导保守界，或将该 bound 标记为启发式剪枝。否则可能影响 recall。

chunk 的构造也很重要。建议优先按局部空间结构或 residual 分布划 chunk，而不是简单按写入顺序切分。否则 chunk radius 可能很大，剪枝效果有限。

### 3.5.5 M4：Primary-Secondary 去重，避免 full payload 多份复制

边界召回可以保留多路路由，但 full payload 不应在多个 posting 中反复复制。

推荐设计：

```text
Primary Posting:
  [VID][Version][Full Payload][Optional Full Metadata]

Secondary Posting:
  [VID][Short Code or Residual Code][Primary Pointer][Optional Bound Info]
```

查询逻辑：

```text
1. secondary 记录参与 coarse scoring。
2. 只有进入全局 top-R 的 VID 才触发 primary payload fetch。
3. payload fetch 按 primary page 合并。
4. 多个 posting 命中同一 VID 时只保留最佳候选记录。
```

这个方案的收益是：

- 降低索引空间放大；
- 降低 build 写放大；
- 降低重复 payload 读取；
- 提高 cache 单位容量收益。

但它也有风险：

- secondary 命中后可能引入 primary 随机读取；
- 需要维护 VID → primary location 映射；
- 更新场景要处理 primary / secondary 一致性；
- 如果 coarse code 质量不足，可能导致 primary fetch 过多。

因此，Primary-Secondary 不能单独使用，必须与 compact code、候选去重、page-aware payload batching 和 hot primary page cache 绑定设计。

### 3.5.6 M5：副本分配从固定 ReplicaCount 改为预算驱动

固定 `ReplicaCount` 的问题是默认所有向量都需要相似数量的副本，所有 posting 的容量压力也相近。但真实数据中，内部点和边界点的歧义度不同，不同 head 的热度和尺寸也不同。

推荐将副本分配改为预算约束优化：

```text
maximize:
  expected_recall_gain(v, h)

subject to:
  posting_size(h) <= size_budget
  total_replica_bytes <= storage_budget
  hotness(h) <= hotness_budget
```

或者使用代价函数：

```text
assign_cost(v, h)
= distance_cost(v, h)
+ λ1 * posting_size_penalty(h)
+ λ2 * hotness_penalty(h)
+ λ3 * replica_byte_cost(v)
```

这样系统不再只看几何距离，也会显式考虑：

- posting 是否已经过大；
- head 是否已经过热；
- 该副本是否真的贡献 recall；
- 该副本会消耗多少字节；
- 是否存在更便宜的 secondary-lite 表示。

该方案不是免费午餐。如果高维数据确实需要更多多路覆盖才能保持 recall，那么限制副本可能带来 recall 下降。因此它必须与 recall-gain 估计和 miss-case 分析结合使用。

### 3.5.7 M6：改造首层 routing，降低对 cluster-first 的依赖

posting 层改造能显著降低 route error 的代价，但不能消除 route error 的来源。如果高维语义向量的 cluster margin 很小，系统仍然需要访问更多 head 才能维持 recall。

长期方向应考虑：

1. **弱化 cluster-first，强化 hybrid-first**  
   让 head/cluster 更多承担存储布局和初始定位职责，而不是独自决定最终候选范围。

2. **引入 graph-assisted routing**  
   使用局部图导航、beam search 或二级图结构扩展候选范围，降低对单一中心距离排序的依赖。

3. **使用 multi-entry / learned router**  
   让 routing 决策不只依赖 query 到 centroid 的距离，而是结合多个特征预测应该访问哪些 posting。

4. **按 workload 选择主导航结构**  
   对高维语义向量，如果图结构在 recall-latency 上显著优于 cluster-first，则 SPANN 更适合作为存储优化型二级结构，而不是唯一主路由结构。

---

## 3.6 推荐改造优先级

从工程落地角度，建议采用以下阶段路线：

| 阶段 | 优先级 | 改造项 | 目标 | 主要风险 |
|---|---:|---|---|---|
| M0 | 最高 | 观测指标补齐 | 证明瓶颈来源 | 埋点不足导致误判 |
| M1 | 高 | 全局 I/O 调度 + 细粒度 cache | 快速缓解高并发 P99 和 QPS 平顶 | 调度策略不当引入排队 |
| M2 | 高 | 两阶段 posting | 降低每条向量查询期字节数 | coarse recall 下降、payload 随机读 |
| M3 | 中高 | posting chunk 化 | 降低整张 posting 不可裁剪的问题 | chunk 半径过大导致剪枝弱 |
| M4 | 中 | Primary-Secondary 去重 | 降低边界复制带来的空间和重复读取 | primary fetch 随机化 |
| M5 | 中 | 预算驱动副本 | 控制 posting 膨胀和热点 | recall 与成本权衡复杂 |
| M6 | 长期 | 首层 routing 改造 | 处理高维低 margin 下的候选定位不稳 | 改动大、评估周期长 |

前两项优先级最高。M0 确保问题被量化，M1 能在不重写完整磁盘格式的前提下改善生产稳定性。M2 和 M3 是根因级改造，收益更大，但涉及磁盘格式、build 链路、查询链路和兼容性，工程周期更长。

---

## 3.7 这些方案能否解决超高维向量下的根本问题？

答案需要分层：

> posting 重构能显著缓解 SPANN 在高维下的 I/O、空间、并发和缓存问题，但不能单独根治 cluster-first routing 在低 margin 高维数据上的候选定位不稳。

必须区分两类问题。

### 3.7.1 存储系统问题

包括：

- posting 过大；
- full payload 过重；
- I/O 读放大；
- 副本空间膨胀；
- 并发请求拥塞；
- cache 粒度过粗；
- 重复 VID / page 读取。

这些问题可以通过前述 M1 到 M5 方案显著改善。

### 3.7.2 几何与索引问题

包括：

- query 到多个 head 的距离差很小；
- cluster 之间重叠严重；
- true nearest neighbors 分散在多个 posting；
- 为达到固定 recall 需要访问越来越多 head；
- centroid-based routing 的排序稳定性下降。

这些问题只能通过 posting 层优化部分缓解，不能完全解决。因为两阶段 posting 和 chunk 化只能降低“访问一个错误 posting 的代价”，不能解释“为什么需要访问这么多 posting”。

### 3.7.3 各方案在超高维场景中的边界

| 方案 | 能解决 | 不能解决 |
|---|---|---|
| 两阶段 posting | 每次探测 posting 的 full payload 成本太高 | 为什么 query 需要探测很多 head |
| chunk 化 | 进入 posting 后仍可局部裁剪 | head 层 routing 本身模糊 |
| Primary-Secondary | 边界点 full payload 多份复制 | cluster 边界本身变少 |
| residual / compact code | 单条高维向量太重 | head 之间距离 margin 过小 |
| 预算驱动副本 | 控制存储和热点膨胀 | 免费保持 recall |
| 全局 I/O 调度 | 并发不过早崩溃 | 单 query 逻辑访问范围过大 |
| 细粒度 cache | 提高局部热数据复用 | 普遍性低复用访问模式 |
| routing 改造 | 缓解候选定位不稳 | 需要更大架构改动 |

因此，一个成熟结论应是：

1. posting 层重构是必须做的，否则系统容易先死于 I/O 和空间成本；
2. 但如果目标是让 SPANN 长期适配 1536 维、3072 维甚至更高维语义 embedding，仅重构 posting 不够，还需要改造首层 routing；
3. cluster/head 更适合作为存储布局和粗定位结构，不宜单独承担全部候选范围判定责任。

---

## 3.8 修订版结论

当前 SPANN 的核心问题不是 SSD 不够快，也不是单个 page limit 参数没调好，而是 posting 层的三个基本假设在高维、高 recall 和高并发场景中被放大：

```text
1. 将整个 posting 作为最小 I/O 单位。
2. 将完整向量作为最小判别单位。
3. 将物理复制作为边界召回的主要手段。
```

对应的结构化替代方案是：

```text
1. 用 chunk / code block 取代整张 posting 读取。
2. 用 compact code 粗筛取代 full vector 全量扫描。
3. 用 primary-secondary 轻量路由取代 full payload 多份复制。
4. 用全局 I/O 调度取代线程级各自为战。
5. 用预算驱动副本取代固定 ReplicaCount。
6. 用 hybrid / graph-assisted routing 缓解高维低 margin 下的首层定位不稳。
```

更精炼地说：

> SPANN 的优化方向不应停留在调参，而应把 posting 从“一个大而重的向量容器”改造成“可分层、可裁剪、可调度、可去重的候选生成结构”。
