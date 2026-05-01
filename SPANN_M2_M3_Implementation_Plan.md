# Context

用户当前的决策是：**先不做 M1，直接推进 M2 和 M3**。

这里的 M2 / M3 不是抽象讨论，而是要基于仓库当前实现，制定一份可以落地执行的实现计划：

- M2：两阶段 posting（compact code 粗筛 + payload 精排）
- M3：posting chunk 化（directory + chunk pruning + chunk 内 code/payload 分层）

本计划需要同时满足三个约束：

1. 以 `/home/ray/code/SPTAG/SPANN_Problem_old_20260430.md` 中的 M2/M3 设计为目标语义；
2. 以当前代码现实为边界，而不是假设一个全新系统；
3. 先走**最小 blast radius** 路线，优先把 static SPANN 路径跑通，再考虑 dynamic/KV 路径。

经过代码阅读，当前最关键的事实是：

- `SPANNIndex.cpp` 通过 `m_extraSearcher->SearchIndex(...)` 将 head search 结果转给 posting search；
- static 路径 `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraStaticSearcher.h` 已经具备：
  - posting 元数据目录 `ListInfo`
  - posting 文件构建与加载（`OutputSSDIndexFile` / `LoadingHeadInfo`）
  - 默认 Linux 可用的 `ASYNC_READ + BATCH_READ` 搜索路径；
- dynamic 路径 `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraDynamicSearcher.h` 当前仍然是：
  - `db->MultiGet(...)` 读取完整 posting blob
  - 线性扫描 full vector record
  - 直接计算精确距离；
- 当前量化/ADC 能力已经存在于：
  - `/home/ray/code/SPTAG/AnnService/inc/Core/Common/IQuantizer.h`
  - `/home/ray/code/SPTAG/AnnService/inc/Core/Common/PQQuantizer.h`
  - `/home/ray/code/SPTAG/AnnService/inc/Core/Common/OPQQuantizer.h`
  - `/home/ray/code/SPTAG/AnnService/inc/Core/Common/QueryResultSet.h`
  但这些能力还没有用于 posting 内部的 compact-code coarse scoring；
- 当前官方 Linux 代码现实约束见 `/home/ray/code/SPTAG/TEST_COMMANDS.md:1074`：只有默认 `ASYNC_READ + BATCH_READ` 路径可稳定测试，非 `BATCH_READ` / 同步模式都不能直接作为现成验证路径。

因此，本计划推荐：**先做 static-only 的 M2/M3，新格式、新查询路径、新指标先在 static SPANN 上闭环，dynamic 路径只做 guardrail，不在第一阶段一起改。**

# 推荐实现方案

## 1. 范围决策：第一阶段只做 static SPANN，不同时改 dynamic

### 推荐范围

第一阶段仅覆盖：

- `Storage::Static` 的 SPANN posting 格式与搜索路径
- 默认 Linux 可用的 `ASYNC_READ + BATCH_READ` 执行路径
- M2 + M3 的联合格式设计，但实现分两步落地

第一阶段**不直接实现**：

- `ExtraDynamicSearcher` 的新 posting 格式
- FileIO / SPDK / RocksDB 三类 dynamic backend 的 chunk/code/payload 重构
- 更新 / merge / split / reassign 等 dynamic mutation 路径的重写

### 原因

static 路径当前已经天然具备 M2/M3 需要的三个基础：

1. **明确的 posting 文件写入点**：`ExtraStaticSearcher::OutputSSDIndexFile(...)`
2. **明确的 posting 文件读取点**：`ExtraStaticSearcher::LoadingHeadInfo(...)`
3. **明确的 posting 搜索执行点**：`ExtraStaticSearcher::SearchIndex(...)`

dynamic 路径当前则是整 blob `MultiGet + full scan` 语义；如果第一步同时改它，会把问题从“实现 M2/M3”扩展成“重做 dynamic blob layout + checksum + mutation + backend addressing”，范围过大。

## 2. 实施阶段划分

## 2.1 Phase 0：参数与格式识别框架先落地

目标：在不改搜索行为的前提下，为新 posting 格式建立**可识别、可切换、可共存**的基础。

### 关键修改文件

- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ParameterDefinitionList.h`
- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/Options.h`
- `/home/ray/code/SPTAG/AnnService/src/Core/SPANN/SPANNIndex.cpp`
- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`

### 新增配置建议

至少增加以下配置项：

- `SSDPostingFormatVersion`
- `EnableTwoStagePosting`
- `EnableChunkedPosting`
- `PostingCodeType`（如 `PQ` / `OPQ` / `SQ`）
- `PostingTopRPerPosting`
- `PostingTopRGlobal`
- `PostingChunkTargetSize`
- `PostingChunkPruneMode`
- `PostingPayloadBatchPages`

### 格式兼容策略

**不要直接破坏 legacy `m_ssdIndex` 文件头布局。**

当前 static reader `LoadingHeadInfo(...)` 假设文件头是固定 legacy 布局；如果直接在旧文件头前加 magic/version，会让旧 loader 失配。

推荐方案：

- 给 static posting 新增一个 sidecar metadata 文件，例如：
  - `<SSDIndex>.meta`
  - 或 `<SSDIndex>.format`
- sidecar 内保存：
  - magic
  - format version
  - posting layout type（legacy / twostage_v1 / chunked_twostage_v1）
  - code type / code bytes
  - chunk pruning mode
  - payload layout 描述

加载规则：

- sidecar 不存在 -> 走 legacy static path
- sidecar 存在且声明新格式 -> 走新 static path
- config 只控制行为参数，不再单独决定二进制解析方式

### 必须增加的 guardrail

在 `SPANNIndex.cpp` / static load path 中明确限制：

- `EnableTwoStagePosting` / `EnableChunkedPosting` 初版只支持 `Storage::Static`
- 若用户在 dynamic storage 上打开这些选项，应显式报错，而不是默默回退或错用 legacy parser

## 2.2 Phase 1：先落地 M2（单 chunk 版本）

目标：先实现“compact code 粗筛 + payload 精排”，但每个 posting 先只放 **1 个 chunk**。这样格式已经是 M2/M3 统一格式，但先不做真实 chunk pruning。

### 关键思想

先把新 posting 统一成：

```text
Posting
  ├── Posting Header
  ├── Chunk Directory（初始只有 1 个 chunk）
  ├── Compact Code Block
  └── Payload Block
```

这样 M2 先验证：

- code-first 搜索是否能显著减少 payload 读取
- coarse candidate recall 是否可控
- payload page-aware fetch 是否可行

### 关键修改文件

- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`
- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/IExtraSearcher.h`
- `/home/ray/code/SPTAG/AnnService/src/Core/SPANN/SPANNIndex.cpp`
- 可能涉及量化器接入：
  - `/home/ray/code/SPTAG/AnnService/inc/Core/Common/IQuantizer.h`
  - `/home/ray/code/SPTAG/AnnService/inc/Core/Common/PQQuantizer.h`
  - `/home/ray/code/SPTAG/AnnService/inc/Core/Common/OPQQuantizer.h`

### static build 路径改造点

在 `ExtraStaticSearcher::BuildIndex(...)` / `OutputSSDIndexFile(...)` 中新增新格式写出逻辑：

对每个 posting：

1. 收集 posting 内所有向量；
2. 为每个向量生成 compact code；
3. 为每个向量记录 payload 在 posting payload block 中的位置；
4. 生成只有 1 个 entry 的 chunk directory；
5. 写出：
   - posting header
   - chunk directory
   - compact code block
   - payload block
6. 写出 sidecar metadata。

### compact code block 建议记录内容

每条记录至少要能支撑 coarse scoring 和后续 payload fetch：

```text
[VID][CompactCode][PayloadLocator]
```

`PayloadLocator` 初版可以不是全局 pointer，只要能定位到当前 posting payload 区域内的 record/page 即可。

### static search 路径改造点

当前 `ExtraStaticSearcher::SearchIndex(...)` 逻辑是：

- 整个 posting 读入
- 可选解压
- 逐条 parse
- 直接精确距离计算

要改成分层流程：

1. 读取 posting header + chunk directory；
2. 读取 compact code block；
3. 对 code block 做 coarse scoring；
4. 每个 posting 保留 top-r 候选；
5. 跨 posting 合并候选并去重；
6. 生成 payload page-aware read plan；
7. 批量读取 payload page；
8. exact rerank；
9. 写入 `QueryResultSet`。

### 关键实现要求

不要把当前 exact parse path 强行改造成 coarse path。建议在 `ExtraStaticSearcher.h` 内部新增明确的分层函数，例如：

- `ReadPostingHeaderAndDirectory(...)`
- `ScanCompactCodes(...)`
- `MergeCoarseCandidates(...)`
- `BuildPayloadReadPlan(...)`
- `FetchPayloadPagesAndRerank(...)`

保持 legacy path 与 new-format path 并存，避免把 `m_parsePosting` / `m_parseEncoding` 等旧逻辑搅在一起。

### 候选管理建议

需要在 `ExtraWorkSpace` 中新增 coarse candidate buffer，而不是直接把 coarse score 写进最终 `QueryResultSet`。

原因：

- coarse 阶段允许同一个 VID 从不同 posting/chunk 多次出现；
- 应先合并、选择最佳 coarse 候选，再决定是否读 payload；
- 否则过早 dedupe 可能把更优 coarse hit 剪掉。

建议候选结构至少包含：

```text
vectorID
coarseDist
postingID
chunkID
payloadOffset
payloadPageID
```

## 2.3 Phase 2：在同一格式上打开 M3（真实多 chunk）

目标：把“单 chunk posting”扩展为“多 chunk posting + chunk pruning”。

### 关键修改文件

仍然主要集中在：

- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`
- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/IExtraSearcher.h`

### build 侧新增逻辑

在构建 posting 时，不再把 posting 作为单一连续向量集合，而是先做 chunk 切分。

对每个 posting：

1. 将 posting 内向量划成多个 chunk；
2. 对每个 chunk 计算：
   - `centroid`
   - `radius`
   - `count`
   - `code_offset`
   - `code_bytes`
   - `payload_offset`
   - `payload_bytes`
3. 把 chunk metadata 写入 posting directory；
4. 写出所有 chunk 的 compact code blocks；
5. 写出所有 chunk 的 payload blocks。

### chunk 划分策略

**不要按写入顺序硬切。**

初版建议：

- 采用 posting 内局部聚类或残差分布聚类；
- 至少保证 chunk 内部是空间上相对紧凑的；
- 让 `radius` 具备实际 pruning 价值。

### query 侧新增逻辑

查询流程升级为：

1. 读取 posting directory；
2. 对每个 chunk 计算 bound；
3. 根据当前 coarse threshold 跳过不可能有价值的 chunk；
4. 只读取 surviving chunk 的 compact code；
5. 继续 Phase 1 的 coarse -> merge -> payload fetch -> rerank 流程。

### 距离度量约束

- `L2`：可先实现下界 `||q - c|| - radius`；但如果剪枝阈值来自 head-search 的 `worstDist()`，这一阶段应按 heuristic 处理，不要写成 safe prune
- `Cosine` / inner product：初版不要声称安全剪枝；
  - 要么禁用 hard prune
  - 要么明确标成 heuristic，并在验证中单独量化 recall 影响

## 3. quantizer / ADC 的复用策略

目标是**复用现有 quantizer / ADC**，而不是自己再写一套 compact code 距离框架。

### 现有可复用点

- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/QueryResultSet.h`
  - query target 已可量化
- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/IQuantizer.h`
  - 提供 quantizer 接口与 ADC 入口
- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/PQQuantizer.h`
- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/OPQQuantizer.h`
- `/home/ray/code/SPTAG/AnnService/src/Core/SPANN/SPANNIndex.cpp`
  - `SetQuantizer(...)`

### 推荐复用方式

#### build 时

- 用现有 quantizer 为 posting payload 生成 compact code；
- compact code 直接写入 new posting format 的 code block。

#### query 时

- 继续复用 `QueryResultSet` 对 query 的量化表示 / ADC table；
- coarse scoring 对 compact code 调用 quantizer distance path；
- exact rerank 仍然走当前 `ComputeDistance(...)` 对 full payload 的精确计算。

### 一个必须坚持的边界

- **coarse code scoring** 和 **payload exact scoring** 必须分开实现；
- 不要试图用 `m_parseEncoding` 这类旧 payload parse 逻辑兼容 code block 读取；
- 否则后面会把 delta/rearrange/compression 与 code-scoring 逻辑缠死。

## 4. 需要修改的关键文件与职责

## 4.1 `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`

这是整个 M2/M3 初版的核心文件。

需要负责：

- 新 posting format 的构建与写出
- 新 posting format 的加载与识别
- static 搜索主路径改造成 directory/code/payload 分层
- chunk pruning
- payload page-aware fetch
- legacy/new path 双栈并存

重点函数：

- `LoadIndex(...)`
- `SearchIndex(...)`
- `BuildIndex(...)`
- `OutputSSDIndexFile(...)`
- `LoadingHeadInfo(...)`
- `GetWritePosting(...)`
- `ListInfo` 结构体

## 4.2 `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/IExtraSearcher.h`

需要扩展：

- `ExtraWorkSpace`
  - coarse candidates
  - payload read plan
  - chunk 临时状态
- `SearchStats`
  - `codeBytesRead`
  - `payloadBytesRead`
  - `chunksPruned`
  - `chunksScanned`
  - `coarseCandidateCount`
  - `coarseCandidateRecall`（若能离线回填）

## 4.3 `/home/ray/code/SPTAG/AnnService/src/Core/SPANN/SPANNIndex.cpp`

负责：

- 新参数透传与 load/save wiring
- quantizer 接入保持通畅
- 对 unsupported dynamic new-format 组合做 fail-fast

重点函数：

- `LoadIndexData(...)`
- `SearchIndex(...)`
- `SearchDiskIndex(...)`
- `SetQuantizer(...)`

## 4.4 `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ParameterDefinitionList.h`
## 4.5 `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/Options.h`

负责：

- 增加 M2/M3 所需参数
- 保存/恢复配置
- 作为 build/search 两侧统一参数入口

## 4.6 `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraDynamicSearcher.h`

第一阶段不是主改造文件，但需要做两件事：

1. 对 new-format 开关 + dynamic storage 给出清晰报错；
2. 审核 `GetWritePosting(...)` / static->dynamic bootstrap 相关假设，避免误把 new static format 当 legacy posting 处理。

## 5. 风险控制与兼容性策略

## 5.1 不要原地替换 legacy parser

必须保留：

- legacy static format 搜索路径
- new static format 搜索路径

二者并行，靠 sidecar format metadata 选择。

## 5.2 不要第一步触碰 dynamic mutation 逻辑

以下逻辑初版一律不做结构重写：

- merge
- split
- append
- reassign
- version/checksum 更新路径

否则实现规模会失控。

## 5.3 payload fetch 必须 page-aware

M2 的收益成立前提之一，就是 payload 读取不能退化成“每个候选一个小随机读”。

因此初版就必须实现：

- 候选按 payload page 聚合
- 按 page 批量读取
- page 内再做 exact rerank

## 5.4 cosine 下的 chunk pruning 必须保守

M3 初版安全边界：

- `L2` 可以先做 heuristic prune，但不能声称是 exact safe prune
- `Cosine` 先默认不开启 hard prune，或仅以显式 heuristic 模式测试

## 6. 验证方案

## 6.1 功能正确性验证

### legacy 兼容

- 用旧 static index 加载搜索
- sidecar 不存在时必须稳定走 legacy path

### new-format 构建与加载

- 构建 M2 新格式 static index
- 重新加载
- 验证 loader 是根据 sidecar 走新路径，而不是依赖 config 猜格式

### 搜索正确性

对比：

- legacy static exact path
- M2 static path
- M2 + M3 static path

输出至少比较：

- `Recall@K`
- `coarse candidate recall`
- `rerank candidate count`
- `distanceEvaluatedCount`

## 6.2 性能验证

受当前仓库约束，验证只基于：

- 默认 `ASYNC_READ + BATCH_READ` Linux 路径

至少比较以下指标：

- `requested_read_bytes`
- `readPages`
- `postingsTouched`
- `postingElementsRaw`
- `distanceEvaluatedCount`
- `avg latency`
- `p95 / p99 latency`
- `Recall@10`

新增建议指标：

- `codeBytesRead`
- `payloadBytesRead`
- `chunksPruned`
- `chunksScanned`
- `coarseCandidateCountBeforeDedupe`
- `coarseCandidateCountAfterDedupe`

## 6.3 验证顺序

1. 小规模 static 数据集做格式和加载调试；
2. static M2 对比 legacy static；
3. static M2 + M3 对比 M2-only；
4. 仅在 static 闭环稳定后，再评估 dynamic 是否值得进入下一轮。

## 6.4 近期 strict `UInt8 + DEFAULT` 同盘验证记录

2026-05-01 已在 `/home/ray/data/sift1m` 上做过一轮同盘对比，口径对齐为：

- 数据格式：`UInt8 + DEFAULT`
- baseline 线程：`SearchThreadNum=8, NumberOfThreads=16`
- 构建线程：`SelectHead=16, BuildHead=16, BuildSSDIndex=16`
- 搜索参数：`InternalResultNum=64, SearchPostingPageLimit=4`
- 对比对象：`legacy`、`M2`、`Chunked-M2`

已确认的事实：

- `legacy` 与 `SIFT1M_Official_Alignment_Summary.md` 中的优化基线基本一致，`QPS` 约 `5945`, `avg latency` 约 `1.344 ms`, `Recall@10` 约 `0.9783`；
- `M2` 与 `Chunked-M2` 目前都能构建出 new-format posting 元数据，但搜索阶段仍出现大量 `invalid new-format header`；
- `M2` 的搜索结果退化到 `Recall@10≈0.00003`，`distanceEvaluatedCount=0`，说明当前并未形成可用的 two-stage 搜索闭环；
- `Chunked-M2` 的搜索结果退化到 `Recall@10≈0.1612`，`distanceEvaluatedCount=0`，`chunksScanned/chunksPruned` 也未形成有效统计，同样不能视为有效性能提升；
- 因此，当前阶段更准确的结论是：**M2/M3 已进入“格式生成完成、搜索路径仍未正确对齐”的验证状态，而不是“已完成并可比较性能”的状态**。

这次验证还说明：

- 仅把 build 侧切到 new-format 还不够，search 侧也必须显式进入同一实验格式；
- 只看 `QPS` 和 `latency` 会误判，因为当前失败路径会提前结束，表面数值更快，但没有真实的读/算行为；
- 后续继续推进时，应优先修正 new-format search path 的 header/layout 解析一致性，再谈 `M2`/`M3` 的性能结论。

## 6.5 2026-05-01 后续排查补充

在继续排查上述 `invalid new-format header` 之后，又确认了两个更具体的事实：

- `SPANNIndex.cpp::SearchDiskIndex(...)` 之前确实没有检查 `m_extraSearcher->SearchIndex(...)` 的返回值，导致 static new-format 查询阶段即使内部已经 `Fail`，外层仍然返回 `Success`，从而把错误伪装成“低延迟、高 QPS”的假象；
- `/home/ray/data/sift1m/m2m3_compare_20260501/index_m2/SPTAGFullList.bin` 与 `index_chunked/SPTAGFullList.bin` 这两份现有 benchmark 产物，磁盘上 posting 起始内容仍然是旧 record layout，而 sidecar `.meta` 已声明 `twostage_v1` / `chunked_twostage_v1`。也就是说，之前那轮 benchmark 很大概率是在拿“旧布局二进制 + 新格式声明”做搜索，结果天然不可信。

这意味着下一步验证必须遵守两个前提：

- 第一，fail-fast 修复后的二进制必须重新跑搜索，确保 new-format 解析失败不会再被吞掉；
- 第二，`M2` / `Chunked-M2` 必须用当前代码重新 build 一轮索引，再谈 recall 或性能对比；不能继续复用这批旧产物。

## 6.6 2026-05-01 Linux `O_DIRECT` 读对齐修复后复测

在重新 build `index_m2` / `index_chunked` 之后，又确认了两个新的 runtime 问题并完成修复：

- `M2` 的 code block 读在 Linux `AsyncFileIO + O_DIRECT` 下曾使用未对齐的子页读取；已改为按页对齐读取，并在 `PostingBlockInfo` 中保存 `m_codeBufferOffset`，解析时从对齐后的 buffer 偏移取 code；
- `Chunked-M2` 的 metadata 扩读在 `ReadPostingHeaderAndDirectory(...)` 中曾对页尾尾巴做小尺寸二次读取，触发 `O_DIRECT` 失败；已改为把 metadata 扩读也向上取整到 page size，再做解析。

这次修复后，`UInt8 + DEFAULT` 同盘复测结果如下：

- `legacy`：`QPS≈5927.68`，`Recall@10=0.978319`，`DistanceEvaluated≈2314.285`
- `M2`：`QPS≈715.51`，`Recall@10=0.957495`，`DistanceEvaluated=256`，`CodeBytesRead≈84351`，`PayloadBytesRead≈387871`
- `Chunked-M2`：`QPS≈698.03`，`Recall@10=0.958495`，`DistanceEvaluated=256`，`ChunkPruneRatio≈0.075`，`ChunksPruned≈6.588/query`
- 当前对比的 canonical 运行时口径已经拆成 `MetadataBytesRead`、`CodeLogical/PhysicalBytesRead`、`PayloadLogical/PhysicalBytesRead`，后续所有对比都应优先看这些分项，而不是只看总 `RequestedBytesRead`

结论：

- 当前 `M2` / `Chunked-M2` 已经从“格式可写但搜索失真”修复为“可稳定搜索”；
- 但就这轮参数而言，`M2` / `Chunked-M2` 仍未超过同口径 `legacy` 基线，后续还需要继续按计划收敛 `topR`、chunk 划分和 payload/page 策略，才能争取真正的性能收益。
- `Chunked-M2` 的 `ChunkPruneMode` 现在应视为 heuristic L2 pruning，而不是 safe pruning。

## 6.7 2026-05-01 `PostingChunkTargetSize=32` 继续实验

为继续推进 `M3`，又在同一套 `UInt8 + DEFAULT`、同盘、`SearchThreadNum=8 / NumberOfThreads=16` 条件下，额外做了更细 chunk 粒度实验：

- 复用现有 head index，只重建 SSD posting，生成 `index_chunked_t32`
- 搜索参数先保持 `PostingTopRPerPosting=64 / PostingTopRGlobal=256`

这轮 `t32` 的直接结果是：

- `Chunked-M2(t32)`：`QPS≈579.95`，`Recall@10=0.926713`
- `CodeBytesRead≈81024`，低于 `t64` 的 `≈84057`
- `PayloadBytesRead≈378472`，低于 `t64` 的 `≈391005`
- `ChunkPruneRatio≈0.191`，明显高于 `t64` 的 `≈0.075`

但同时也观察到：

- 尽管 chunk pruning 更积极，当前 `t32` 的 `Recall@10` 明显低于 `t64`
- 在当前搜索实现下，减少的 code/payload 读取并没有转化为更高 `QPS`
- 把搜索侧 `topR` 提高到 `96/384` 后，`Recall@10` 只回到 `≈0.9347`，而 `QPS` 进一步降到 `≈489`
- 当前 `ChunkPruneRatio` 的提升只能证明更细 chunk 能减少扫描量，不能证明 pruning 已达到 safe L2 的语义

因此当前阶段可以得出一个更具体的 M3 结论：

- **更细 chunk 粒度本身是有效的**，因为它确实提高了 prune ratio，并降低了 code/payload bytes
- **但仅靠缩小 `PostingChunkTargetSize` 还不够**；如果 chunk clustering / bound / coarse candidate 策略不一起跟进，就会先吃掉 recall，且不一定换来吞吐收益

## 6.8 2026-05-01 code block 合并读尝试

还尝试过把 `ReadChunkCodeBlocks(...)` 从“每个 block 一次 aligned 直读”改成“同文件相邻 window 合并读，再拆分回各自 buffer”。

这条路线在当前 Linux 路径上的结论是：

- 额外的 read-plan 构建与 buffer copy 开销，抵消了减少 syscall 的理论收益
- 在当前查询规模下，`QPS` 出现明显回退
- 因此代码已回退到保守但更稳的 **aligned direct read per block** 实现，优先保持 correctness 与 fail-fast

## 6.9 2026-05-01 统计口径拆分后复核

在把 `Metadata / Code / Payload` 的 logical/physical 统计拆开后，又用同一套 `UInt8 + DEFAULT`、同盘配置重新跑了一次 baseline 对照，确认新统计口径是可用的：

- `M2`：`QPS≈732.65`，`Recall@10=0.957936`，`MetadataBytesRead≈260828`，`CodeLogicalBytesRead≈84351`，`CodePhysicalBytesRead≈339053`，`PayloadLogicalBytesRead≈32768`，`PayloadPhysicalBytesRead≈387871`
- `Chunked-M2`：`QPS≈701.61`，`Recall@10=0.958645`，`MetadataBytesRead≈272658`，`CodeLogicalBytesRead≈84057`，`CodePhysicalBytesRead≈389731`，`PayloadLogicalBytesRead≈32768`，`PayloadPhysicalBytesRead≈391005`，`ChunkPruneRatio≈0.075`

这轮复核说明：

- 统计拆分后，`M2` / `Chunked-M2` 的行为没有失真，新的 logical/physical 口径可以作为后续分析基线；
- `Chunked-M2` 的 `ChunkPruneRatio` 仍然只有轻度收益，且当前实现仍应按 heuristic pruning 解读。

## 6.10 2026-05-01 为什么当前还打不过 legacy

这一轮对比已经把一个关键事实暴露得很清楚：**问题不再是 recall 逻辑是否闭环，而是 new-format search 的系统性 I/O 和阶段拆分过重**。

在 `UInt8 + DEFAULT`、同盘、线程已按官方基线优化的前提下：

- `legacy` 仍然是 `QPS≈5927.68`，`Recall@10=0.978319`
- `M2` 只有 `QPS≈732.65`，`Recall@10=0.957936`
- `Chunked-M2` 只有 `QPS≈701.61`，`Recall@10=0.958645`

这说明当前瓶颈不是“再多调一点 topR”就能解决的。更具体地看，new-format 路径存在四个会直接吞掉吞吐的结构性问题：

1. **metadata 热路径反复读盘**
   - `M2` 每个 query 仍要读约 `260-272KB` 的 metadata；
   - 这部分是稳定开销，和真正 exact rerank 无关；
   - 既然 metadata 的内容对同一个 posting 是静态的，就应该在 `LoadIndex` 时预解析并缓存，而不是每次 query 重读。

2. **code path 仍然是逐 block 同步读**
   - `ReadChunkCodeBlocks(...)` 现在还是每个 block 一次 `ReadBinary(...)`；
   - 这意味着 code 阶段会被 syscall、对齐和调度开销放大；
   - legacy 路径虽然也读整 posting，但它没有把一个 posting 拆成 metadata/code/payload 三段分别跑多个热路径。

3. **物理读远大于逻辑读，page inflation 很重**
   - `M2` 的 `CodeLogicalBytesRead≈84KB`，但 `CodePhysicalBytesRead≈339KB`；
   - `PayloadLogicalBytesRead≈32KB`，但 `PayloadPhysicalBytesRead≈388KB`；
   - 这说明当前 page 颗粒度和数据布局不匹配，`topR` 再怎么调，很多收益也会被 4KB 对齐吞掉。

4. **M3 pruning 还不能作为主要提速手段**
   - 当前 chunk prune 只能按 heuristic 理解；
   - `PostingChunkTargetSize=32` 说明更细 chunk 的确能提升 prune ratio，但 recall 先掉了；
   - 因此 pruning 只能作为后续优化项，不能拿来当主 QPS 引擎。

基于以上事实，优化优先级应当是：

1. **先把 metadata 从 query 热路径移走**
   - 预期收益是立刻减少每 query 的固定 I/O；
   - 这一步不改变语义，风险最低。
2. **再把 code block 读改成批量异步**
   - 让 code 阶段复用已有 batch I/O 能力；
   - 目标是减少同步小 I/O 和 syscall 数。
3. **然后再收紧 payload 读计划**
   - 提高 batch pages、减少 page 重复读、减少 copy；
   - 这一层才是进一步拉近 legacy 的地方。
4. **最后才是重新调 topR / chunk size / pruning**
   - 因为这属于 recall / pruning tradeoff，不是纯吞吐优化。

当前这版代码已经开始执行第 1 步：把 new-format posting metadata 在 load 阶段缓存起来，query 时不再重复读 header / directory / centroid。下一步如果这条路径验证有效，再继续把 `ReadChunkCodeBlocks(...)` 改成批量异步读。

### 为什么这一步优先级最高

这里选择 **先做 metadata cache**，不是因为它看起来最简单，而是因为它同时满足下面四个条件：

1. **它命中的是真正的固定成本**
   - metadata 开销几乎每个 query 都会发生；
   - 它不依赖 `topR`、payload hit rate、chunk prune ratio；
   - 因此只要去掉，收益就更接近“确定性收益”，而不是依赖参数运气。

2. **它不改变候选集合语义**
   - metadata cache 只是在搬运“何时读、从哪里读”；
   - 不改变 coarse candidate、rerank candidate、distance 公式；
   - 这意味着它适合作为第一层优化，因为 correctness 风险最低。

3. **它能立刻验证“瓶颈是否真在 I/O 结构”**
   - 如果 metadata 移出热路径后 QPS 仍几乎不动，说明主矛盾就不在这里；
   - 如果 QPS 有明显提升，说明 new-format 路径的性能问题确实是结构性 I/O，而不是单纯 quantizer 或 SIMD 算得慢。

4. **它能为后续优化提供更干净的基线**
   - 如果 metadata 仍混在 query 热路径里，后续无论调 code read 还是 payload batching，都很难分辨到底是谁在起作用；
   - 先把 metadata 清零，后面的对比才有解释力。

### 为什么没有先做 code async / code cache

从“理论收益”看，code phase 当然也是大头；但它不适合作为第一步，原因是：

1. **code path 和 workspace/request 生命周期耦合更深**
   - `ReadChunkCodeBlocks(...)`、`ScanCompactCodes(...)`、`AsyncReadRequest` 的使用方式是联动的；
   - 一旦直接改成完全不同的 read mode，很容易把 request callback、buffer ownership、diskRequestIndex 等语义一起改坏。

2. **code phase 的失败模式更像“部分正确、局部崩坏”**
   - metadata path 出问题通常很快 fail-fast；
   - code path 出问题则更容易出现“结果还返回了，但 recall 已经慢慢漂掉”的情况；
   - 这类问题更难诊断，也更容易污染 benchmark 结论。

3. **Linux AIO 路径对 code phase 的适配并不天然成立**
   - payload page path 本来就是按 page 请求设计的；
   - code block path 则有更复杂的 aligned subrange、不同 block 大小、不同 offset 分布；
   - 在没验证底层 AIO 约束前，直接把 code phase 切到 batch async，失败概率比 metadata cache 高得多。

### 如何判断每一步是否值得保留

为了避免“QPS 变快但语义坏了”的假优化，后续每一步都应按同一标准判断：

1. **先看 correctness 是否保持在可接受区间**
   - 至少保证 `Recall@10` 没有出现明显异常下跌；
   - 如果 recall 先崩，再高的 QPS 都不计入有效收益。

2. **再看对应分项指标是否真的下降**
   - metadata 优化看 `MetadataBytesRead`；
   - code 优化看 `CodePhysicalBytesRead` / `diskIOCount`；
   - payload 优化看 `PayloadPhysicalBytesRead` / `readPages`。

3. **最后才看端到端 QPS**
   - 因为有些优化只是把 I/O 从一个阶段挪到另一个阶段；
   - 只有分项指标和 QPS 同时改善，才能说明它是真正的主线收益。

### 后续怎么排才最稳

在当前阶段，更稳的推进顺序应该是：

1. **metadata cache**
   - 目标：消掉固定 metadata I/O；
   - 风险最低，且最容易验证。

2. **code phase 的保守优化**
   - 先确认 request 生命周期、buffer ownership、Linux AIO 权限/约束；
   - 不直接把所有 code read 一次性切到激进模式。

3. **payload plan 收紧**
   - 减少 page duplication、无效 batch、额外 copy；
   - 这是继续拉近 legacy 的第二层收益来源。

4. **参数/布局联合调优**
   - 包括 `PostingTopRPerPosting`、`PostingTopRGlobal`、chunk size、chunk clustering；
   - 这一层应在热路径 I/O 结构相对稳定后再做，否则调参结论不可靠。

## 6.11 2026-05-01 metadata cache 执行结果

第 1 步已经完成并做了同盘复测。当前做法是：

- `LoadIndex` 阶段预解析 new-format posting metadata；
- query 阶段 `ReadPostingHeaderAndDirectory(...)` 优先使用内存缓存，不再为 header / directory / centroid 重复读盘。

这轮复测结果：

- `M2(metadata cache)`：`QPS≈1086.60`，`Recall@10≈0.955875`
- `RequestedBytesRead≈726924`
- `MetadataBytesRead=0`
- `CodePhysicalBytesRead≈339053`
- `PayloadPhysicalBytesRead≈387871`

和前一版 `M2(QPS≈732.65)` 相比，可以确认：

- **metadata cache 是有效的**；
- 它把固定 metadata I/O 从 query 热路径里完全移掉；
- 单靠这一步，`QPS` 已经从 `≈733` 拉到 `≈1087`，提升约 `48%`；
- 但即便如此，距离 `legacy≈5928 QPS` 仍然有明显差距，说明 metadata 不是唯一瓶颈。

## 6.12 2026-05-01 code async / code cache 尝试结论

在 metadata cache 之后，又继续验证了两个更激进的方向：

1. `ReadChunkCodeBlocks(...)` 改成复用 Linux `BatchReadFileAsync(...)`
2. 在 load 阶段把 compact code 也缓存进内存，query 不再读 code block

这两条路线当前都不能直接进入主线：

- `code async batch read` 在当前 Linux AIO 路径上触发了 `io_submit(...): Operation not permitted`，说明这条路径和当前 code read 的调用方式/权限假设并不兼容；
- `code cache` 试验版进一步引入了运行时 `std::bad_function_call`，说明当前 workspace/request 语义还没完全和“零 code I/O”分支对齐。

因此当前阶段的主线结论是：

- **保留 metadata cache**，因为它已验证有效且稳定；
- **回退 code async / code cache 试验**，避免把未稳定方案混进主线；
- 下一轮若继续推进，应优先做更保守的 code-phase 优化，例如：
  - 明确区分“cached code block”与“disk-backed code block”的 request 生命周期；
  - 或者改为 page-level、权限已验证的独立异步通道，而不是直接复用当前 payload batch path。

## 6.13 2026-05-01 当前瓶颈复核与 payload merge 尝试结论

在 `metadata cache` 稳定后，又针对当前日志做了一轮更严格的瓶颈复核。稳定基线以 `/tmp/m2_search_current.log` 为准，核心现象如下：

- `M2(metadata cache)` 稳定约为：`QPS≈1072~1086`，`Recall@10≈0.955~0.956`
- `MetadataBytesRead=0`
- `CodeLogicalBytesRead≈84351`，`CodePhysicalBytesRead≈339053`
- `PayloadLogicalBytesRead=32768`，`PayloadPhysicalBytesRead≈387871`
- `PagesRead≈177.472/query`
- `BatchReadTotalLatency≈5.79 ms`

这说明当前主瓶颈已经非常明确：

1. **metadata 已经不是瓶颈**
   - 这部分已经被完全挪出 query 热路径；
   - 再继续在 metadata 上做文章，收益空间很小。

2. **真正的热区是 code/payload 的 page amplification**
   - code 逻辑读只有 `~84KB`，物理读却是 `~339KB`；
   - payload 逻辑读只有 `32KB`，物理读却是 `~388KB`；
   - 问题不是 rerank 本身读了太多“有效字节”，而是为了拿到这些有效字节，当前 layout/page 组织付出了过高的 4KB 对齐与离散访问成本。

3. **payload 路径的“请求数”仍然偏多**
   - 当前 payload 仍是按 page 建请求，再在 rerank 阶段按 page 回填 payload；
   - 在统计上，它会把很多相邻页拆成多个独立 I/O；
   - 因此从系统层面看，`diskIOCount` 仍偏高。

基于这个判断，又尝试过一版 **payload 连续页合并读取**：

- 做法：
  - 把同一 posting 下、被 rerank 候选命中的相邻 payload page 合并成一次读取；
  - rerank 阶段再从合并后的 buffer 中按页内 offset 拷贝出 candidate payload；
  - 目标是先压 `diskIOCount` / `batch read latency`，且不改变候选集合语义。

- 观测到的直接结果：
  - `QPS` 确实从 `≈1073` 提升到 `≈1160`
  - `Total Disk IO Distribution` 从 `≈158` 降到 `≈119`
  - `BatchReadTotalLatency` 从 `≈5.79 ms` 降到 `≈5.26 ms`

- 但该方案**不能进入主线**，原因同样非常明确：
  - `Recall@10` 稳定从 `≈0.955` 掉到 `≈0.949~0.950`
  - `RequestedBytesRead`、`PayloadPhysicalBytesRead`、`PagesRead` 基本没有下降
  - 这说明该优化当前只是在减少“请求个数”，并没有减少 page 物理放大；更重要的是，它已经改变了 rerank 结果语义

因此这一轮的主线结论是：

- **payload merge 这条实现当前已回退，不进入默认主线**
- 它证明了一个重要事实：
  - 仅仅减少请求数，确实能改善 `batch read latency` 和 QPS；
  - 但如果 page-to-buffer 的映射稍有偏差，就会立刻污染 exact rerank；
  - 这次回退的根本原因不是“recall 下降本身不可接受”，而是**收益与退化之间的 tradeoff 还不够干净可解释**：
    - 当前 `Recall@10` 从 `≈0.955` 掉到 `≈0.949~0.950`
    - 但 `RequestedBytesRead` / `PayloadPhysicalBytesRead` 基本没降
    - 因而目前更像是“实现偏差或调度形态变化”，而不是一个可信的、可产品化的 tradeoff

这一轮还顺带暴露了一个更底层但值得保留的实现问题：

- Linux `AsyncFileIO::BatchReadFile(...)` 内部原先把 `aio_nbytes` / `io_uring_prep_read(...)` 写死为 `PageSize`
- 这对于“未来任何大于 4KB 的 batch request”都是潜在 correctness 风险
- 因此这里保留了一个**独立且正确的底层修复**：
  - 让 Linux batch read 尊重 `AsyncReadRequest::m_readSize`
  - 这不是 payload merge 的一部分，而是底层 I/O 契约修复

### 为什么 payload merge 当前不应继续硬推

这一点需要明确写下来，避免后面重复走弯路：

1. **它没有命中当前最大收益项**
   - 现在减少的是 I/O 请求数，而不是 physical bytes；
   - 但当前和 legacy 拉开数量级差距的核心，仍然是 page amplification，而不只是 syscall 个数。

2. **它太容易破坏 rerank correctness**
   - payload 是 exact distance 的输入；
   - 只要 buffer page 映射、offset 计算、尾页边界有一点点不一致，就会直接反映成 recall 下滑；
   - 相比 metadata cache，这类风险高得多。

3. **当前收益解释力不够强**
   - 因为 `RequestedBytesRead` 和 `PayloadPhysicalBytesRead` 没变；
   - 即使 QPS 上升，也更像是“调度形态变了”，而不是主瓶颈真正被解除；
   - 在还没有把 correctness 做到完全对齐前，不值得把它并入主线。

### 当前更合理的下一步

基于这一轮结果，下一步不应该继续在“payload 连续读试验版”上堆补丁，而应按下面顺序推进：

1. **保留 metadata cache 稳定主线**
   - 当前稳定基线已经可重复；
   - 后续优化都必须和这条基线比较。

2. **把 code physical read inflation 当成更高优先级**
   - code 阶段 `84KB -> 339KB` 的放大比例和 payload 一样严重；
   - 但 code 阶段仍处在 coarse rerank 之前，更适合先做“不会污染 exact payload”的结构优化。

3. **如果再碰 payload，只做可强验证的 page-contract 优化**
   - 例如严格 page-aligned、语义等价的请求组织；
   - 或者先只补 instrumentation，把“请求合并前后 page 命中图”打清楚；
   - 而不是直接把连续页 merge 当成主线优化提交。

4. **继续以可解释 tradeoff 为准绳**
   - `QPS↑ / Recall↓` 并非天然不可接受；
   - 如果 `QPS` 提升幅度足够大，而 `Recall@10` 只是小幅下降，并且下降原因清楚、行为稳定、适用场景明确，这类优化可以作为可选模式保留；
   - 但若 `Recall` 下降后，核心 I/O 指标没有同步改善，或下降原因解释不清，就不能把它当成可信主线收益。

## 6.14 2026-05-01 code cache 路线复活后的实测结论

在 6.12 把 code cache 判定为“暂不进主线”之后，又沿着更保守的方向重新收敛了一轮，实现上做了三件关键修复：

1. **load-time code cache 改成“先精确子区间读，失败再回退到对齐页读”**
   - 这一步只发生在 `LoadIndex`；
   - query 热路径不依赖它的读法，因此不需要把 load-time cache 强行绑定到 page-aligned inflation。

2. **修复 empty posting 的误判**
   - `LoadPostingRuntimeMetadata(...)` 对空 posting 会合法返回；
   - 原先 `LoadPostingRuntimeCodeCache(...)` 却先检查 `m_valid` 再检查 `listPageCount/listEleCount`，会把空 posting 误判成失败；
   - 这已经修正为：空 posting 直接 `Success`，不再在 `LoadIndex` 阶段错误 fail-fast。

3. **移除 code-cache 热路径里的调试竞争源**
   - 早期 probe 版在 `ReadChunkCodeBlocks(...)` 中保留了多线程探针日志；
   - 这条日志在 `SearchThreadNum>1` 时会引入无意义的共享热路径竞争；
   - 当前已去掉，避免把 probe 行为混进性能/正确性结论。

修完这几处后，`code cache` 路线已经可以稳定跑完 `UInt8 + DEFAULT` 的整套 10K 查询。

### 6.14.1 结果一：`SearchThreadNum=1 / NumberOfThreads=16`

在保持 `UInt8 + DEFAULT`、同盘、`InternalResultNum=64`、`PostingTopRPerPosting=64`、`PostingTopRGlobal=256` 不变，只把 query 并发收敛到 `SearchThreadNum=1` 时：

- `QPS≈432.77`
- `Recall@10≈0.962687`
- `MetadataBytesRead=0`
- `CodeLogicalBytesRead=0`
- `CodePhysicalBytesRead=0`
- `PayloadPhysicalBytesRead≈387871`

这个结果说明：

- **code cache 的语义本身是通的**；
- 在 code 全缓存后，`CodePhysicalBytesRead` 已经被压到 **0**；
- recall 没有出现异常崩塌，说明“cached code block -> coarse candidate -> payload rerank”这条链路整体可工作；
- 此时系统瓶颈已经几乎完全收敛到 payload page 读取。

### 6.14.2 结果二：官方优化线程档 `SearchThreadNum=8 / NumberOfThreads=16`

继续回到当前对齐 `SIFT1M_Official_Alignment_Summary.md` 的线程档，结果是：

- `QPS≈2928~2929`
- `Recall@10≈0.941`
- `MetadataBytesRead=0`
- `CodeLogicalBytesRead=0`
- `CodePhysicalBytesRead=0`
- `RequestedBytesRead≈387871`
- `PayloadPhysicalBytesRead≈387871`
- `PagesRead≈94.695/query`

和稳定主线 `M2(metadata cache)` 相比：

- `QPS` 从 `≈1072~1086` 提升到 `≈2929`，提升约 **2.7x**
- `CodePhysicalBytesRead` 从 `≈339053` 直接降到 **0**
- `RequestedBytesRead` 也从 `≈726924` 收敛到 `≈387871`
- 当前剩余物理读几乎全部来自 payload

这说明从“结构性 I/O 消减”的角度看，**code physical read inflation 已经被真正打掉**，不是单纯调度形态变化。

### 6.14.3 为什么这组结果不能简单当成“完全收敛”

尽管 `QPS` 提升非常明显，但这里还不能把 `SearchThreadNum=8` 下的 recall 直接当成最终 canonical 结论，原因是：

- 当只把 `SearchThreadNum` 从 `1` 提到 `8`，而保持 `NumberOfThreads=16`、`topR`、payload path、code cache 内容不变时：
  - `QPS` 按预期大幅提升；
  - 但 `Recall@10` 会从 `≈0.9627` 掉到 `≈0.941`。

这说明当前剩下的可疑点更像是：

1. **多 query 并发下的 shared-state / workspace / request 语义仍需复核**
   - 因为 code cache 本身在 `SearchThreadNum=1` 时是通的；
   - recall 的额外下跌是在 query 并发放大后出现的。

2. **这已经不是 code physical bytes 的问题**
   - 两个线程档位下 `CodePhysicalBytesRead` 都已经是 0；
   - 差异更像是并发执行形态暴露出的结果稳定性问题。

3. **但这也不等于必须立刻整体回退 code cache**
   - 用户目标里已经明确接受“QPS 大幅提升、Recall 小幅下降”的可选 tradeoff；
   - 当前 `≈2929 QPS / ≈0.941 Recall@10` 至少已经是一个稳定、可复现、I/O 指标方向完全一致的性能档；
   - 它可以作为 **可选性能模式** 继续保留和分析，而不是直接否掉。

### 6.14.4 这一步对主线推进意味着什么

到这一轮为止，M2/M3 热路径的结论已经比之前清楚很多：

1. **metadata inflation：已解**
   - `MetadataBytesRead=0`

2. **code physical read inflation：基本已解**
   - `CodePhysicalBytesRead=0`
   - `RequestedBytesRead` 也因此从 `≈727KB` 降到 `≈388KB`

3. **payload physical read inflation：成为唯一主要瓶颈**
   - `PayloadPhysicalBytesRead` 仍约 `≈387871/query`
   - `PagesRead` 仍约 `≈94.7/query`
   - 这说明接下来若要继续向 `legacy≈5927.68 QPS` 靠近，主战场已经明确转移到 payload path

4. **官方线程档下的 recall 稳定性仍需继续排查**
   - 这属于“并发语义/执行形态”问题；
   - 它不改变“code inflation 已被显著消除”这一结构性事实，但会影响最终默认档位该怎么选。

5. **`PostingTopRGlobal` 的进一步下探还不稳定**
   - 在当前 code-cache 路线下，把 `PostingTopRGlobal` 从 `256` 降到 `192/128` 时，运行会在 query 起始阶段重新触发 `std::bad_function_call`；
   - 这说明“zero code I/O + 更激进 rerank 收缩”这条组合还有额外语义问题没有清掉；
   - 因此当前可复现、可引用的 code-cache benchmark 仍以 `PostingTopRGlobal=256` 为准。

### 6.14.5 下一步优先级更新

基于当前结果，下一步优先级应当更新为：

1. **保留 metadata cache**
2. **保留 code cache 路线，继续作为零 code I/O 主线试验**
3. **优先转向 payload physical read inflation**
   - 因为它现在已经是绝对大头；
   - 后续任何想继续冲高 QPS 的动作，都必须真正压 `PayloadPhysicalBytesRead` / `PagesRead`。
4. **并行排查 `SearchThreadNum>1` 下的 recall 稳定性**
   - 重点看 shared-state / workspace / request 语义；
   - 但不应因此否认 code cache 已经带来的结构性收益。

### 关于 recall / QPS 的判定口径

这里需要明确修正一个容易写得过头的原则：

- **不是所有 recall 下降都要一票否决**
- 更合理的判断方式是分三类：

1. **默认主线**
   - 目标是尽量保持当前 canonical recall 水平；
   - 只有在 recall 基本稳定的前提下，再追求 QPS 改善。

2. **可选性能模式**
   - 允许用小幅 recall 损失换取明显 QPS 提升；
   - 但必须满足：
     - tradeoff 稳定可复现；
     - `QPS` 提升和 `physical bytes/readPages/diskIOCount` 的变化方向一致；
     - 参数、场景、预期 recall 档位都写清楚。

3. **直接判失败的情况**
   - recall 下滑明显，但 QPS 收益不够大；
   - 或者 QPS 看似提升，但核心 I/O 指标没有同步改善；
   - 或者结果解释不清，怀疑是实现错误、页映射错误、边界处理错误。

就本轮 payload merge 而言，它属于第 3 类，而不是因为“任何 recall 下降都不允许”，而是因为：

- 它的 recall 退化已经稳定可见；
- 同时 `RequestedBytesRead` / `PayloadPhysicalBytesRead` 并未真正下降；
- 所以当前证据更支持“实现/语义问题未完全对齐”，而不是“这是一个值得接受的高性能模式”。

# 关键复用点

- `/home/ray/code/SPTAG/SPANN_Problem_old_20260430.md:412-500`
  - M2/M3 目标语义定义
- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`
  - static posting build/load/search 主体
- `/home/ray/code/SPTAG/AnnService/inc/Core/SPANN/IExtraSearcher.h`
  - workspace / stats 复用点
- `/home/ray/code/SPTAG/AnnService/src/Core/SPANN/SPANNIndex.cpp`
  - head-search 到 posting-search 的调度枢纽
- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/IQuantizer.h`
- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/PQQuantizer.h`
- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/OPQQuantizer.h`
- `/home/ray/code/SPTAG/AnnService/inc/Core/Common/QueryResultSet.h`
  - 现有 quantizer / ADC 能力复用入口
- `/home/ray/code/SPTAG/TEST_COMMANDS.md:1074`
  - 当前可执行验证路径现实约束

# 执行与验证

## 实施顺序（推荐）

1. 参数与 sidecar format metadata 落地
2. static-only new-format load/save 基础打通
3. static M2（单 chunk）build path 落地
4. static M2 query path 落地
5. coarse candidate / code bytes / payload bytes 指标补齐
6. static M2 正确性与性能对比
7. static M3（多 chunk + pruning）build path 落地
8. static M3 query path 落地
9. L2 下做 heuristic pruning 验证
10. 再决定是否进入 dynamic 版 M2/M3

## 验收标准

至少满足：

1. legacy static index 不受影响；
2. new static format 能 build / load / search；
3. 在相近 recall 下，`payloadBytesRead` 与 `distanceEvaluatedCount` 明显下降；
4. `requested_read_bytes` 或 `readPages` 至少有下降趋势；
5. M3 在 `L2` 场景下 heuristic pruning 的 recall 损失可接受，且不会被误写成 safe pruning；
6. 整个方案只依赖当前仓库可稳定跑通的默认 `BATCH_READ` 路径，不以尚未修好的 non-BATCH/sync 分支为前提。
