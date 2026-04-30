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

- `L2`：可先实现安全下界 `||q - c|| - radius`
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

- `L2` 先做 hard prune
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
9. L2 下做安全 pruning 验证
10. 再决定是否进入 dynamic 版 M2/M3

## 验收标准

至少满足：

1. legacy static index 不受影响；
2. new static format 能 build / load / search；
3. 在相近 recall 下，`payloadBytesRead` 与 `distanceEvaluatedCount` 明显下降；
4. `requested_read_bytes` 或 `readPages` 至少有下降趋势；
5. M3 在 `L2` 场景下 pruning 不引入不可接受的 recall 损失；
6. 整个方案只依赖当前仓库可稳定跑通的默认 `BATCH_READ` 路径，不以尚未修好的 non-BATCH/sync 分支为前提。
