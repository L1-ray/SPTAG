# `spann_build_16t_nocache.log` 完整分析

## 1. 日志整体性质

这份日志文件 `results/spann_build_16t_nocache.log` 不是纯粹的 SPTAG 内部日志，而是两部分内容叠加在一起：

1. **外层监控脚本输出**
   - 负责记录开始/结束时间、实验环境、CPU/内存/I/O 统计、阶段耗时摘要。
2. **`ssdserving` 程序自身输出**
   - 负责记录 SPANN 构建流程本身：参数装载、`SelectHead`、`BuildHead`、`BuildSSDIndex`、回读索引统计等。

因此阅读这份日志时，最好把它分成两层来看：

- **监控层**：`results/spann_build_16t_nocache.log:1-28`, `:636-660`
- **构建层**：`results/spann_build_16t_nocache.log:29-634`

---

## 2. 本次实验在做什么

从日志开头可以看出，本次运行命令是：

- 配置文件：`/home/ray/code/SPTAG/spann_build_only.ini`
- 程序：`/home/ray/code/SPTAG/Release/ssdserving`
- 索引目录：`/media/ray/1tb/sift1m/spann_index`
- 数据集：`sift1m`

对应日志位置：

- `results/spann_build_16t_nocache.log:23-27`

这次任务的本质是构建一个 **SPANN 磁盘索引**。整体流水线可以概括为：

1. **SelectHead**：从 100 万个向量中选出一批 head 向量。
2. **BuildHead**：仅针对这些 head 向量构建内存头索引（BKT + 图结构）。
3. **BuildSSDIndex**：将全部 100 万个向量按照 head 归属组织为磁盘 posting list。
4. **回读磁盘索引**：检查构建结果并统计页分布。

---

## 3. 外层监控头部：实验环境与采样信息

日志开头 `results/spann_build_16t_nocache.log:1-28` 是监控脚本写入的元信息。

### 3.1 时间与缓存状态

- `开始时间: 2026-04-13 19:51:09`
- `已清除缓存: 是`

这说明这是一次 **nocache 测试**。也就是说，在实验前已经主动清理过系统页缓存，尽量让磁盘访问行为更接近冷启动。

### 3.2 实验环境

记录了：

- 内核版本：`6.17.0-20-generic`
- 操作系统：`Ubuntu 24.04.4 LTS`
- 磁盘：`sda`, `nvme0n1`

虽然 `CPU:` 和 `内存总量:` 这两行内容为空，但不影响后面主体分析，因为核心构建参数和运行统计都在日志后半段给出来了。

### 3.3 监控粒度

- `采样间隔: 1 秒`
- `监控磁盘: /media/ray/1tb (sda)`

这表示：

- CPU、内存、线程数、进程状态、磁盘 I/O 都是每秒采样一次。
- 后面 CSV/LOG 中的 I/O 统计只针对这个挂载点对应的磁盘设备。

---

## 4. 程序启动：基础参数与阶段参数装载

从 `results/spann_build_16t_nocache.log:29-61` 开始，进入 `ssdserving` 程序的实际初始化输出。

### 4.1 基础参数（Base）

对应：`results/spann_build_16t_nocache.log:29-41`

关键内容：

- `Using AVX2 InstructionSet!`
- `Dim = 128`
- `DistCalcMethod = L2`
- `IndexAlgoType = BKT`
- `ValueType = Float`
- `VectorPath = /media/ray/1tb/sift1m/sift_base.fvecs`
- `QueryPath = /media/ray/1tb/sift1m/sift_query.fvecs`
- `TruthPath = /media/ray/1tb/sift1m/sift_groundtruth.ivecs`

解释：

- 数据类型是 `Float`，维度是 `128`，距离度量是 `L2`。
- `IndexAlgoType = BKT` 并不代表整个系统是单纯 BKT；它更准确表示 **head index 这部分的内存索引核心结构是 BKT**。
- SPANN 本身是“内存头索引 + 磁盘 posting list”的组合结构。

### 4.2 BuildSSDIndex 参数先被装载

对应：`results/spann_build_16t_nocache.log:42-50`

可以读出：

- `BuildSsdIndex = true`
- `InternalResultNum = 64`
- `MaxCheck = 2048`
- `NumberOfThreads = 1`
- `PostingPageLimit = 12`
- `ReplicaCount = 8`
- `TmpDir = /tmp/`

这里有一个很重要的观察：

> 尽管文件名叫 `spann_build_16t_nocache.log`，但 **BuildSSDIndex 阶段真正使用的是 1 线程**。

也就是说，“16t”并不是所有阶段统一使用 16 线程。后面真正大量并行的是 `SelectHead` 和 `BuildHead`，它们用的是 14 线程。

### 4.3 SelectHead 参数

对应：`results/spann_build_16t_nocache.log:51-61`

关键参数：

- `BKTKmeansK = 32`
- `BKTLeafSize = 8`
- `NumberOfThreads = 14`
- `Ratio = 0.16`
- `SamplesNumber = 1000`
- `SaveBKT = false`
- `SelectThreshold = 50`
- `SplitFactor = 6`
- `SplitThreshold = 100`
- `TreeNumber = 1`

其中最关键的是：

- `Ratio = 0.16`：目标是让 head 数量约占总向量数的 16%。
- `TreeNumber = 1`：只建 1 棵树。
- `NumberOfThreads = 14`：这一阶段是多线程执行的。

---

## 5. SelectHead 阶段：从 100 万个点中挑出 16% 的 head

阶段开始于：

- `results/spann_build_16t_nocache.log:62`

### 5.1 读取全量基础向量

- `Begin initial data (1000000,128)...`

这说明程序先把 **100 万条、128 维** 的基础向量读入内存。后续所有 head 选择都是在这批全量向量上完成的。

### 5.2 参数预处理

- `Begin Adjust Parameters...`

这一步是在真正建树前，对选 head 过程涉及的参数做准备和内部调整。日志没有打印展开细节，但它是 SelectHead 的标准前置步骤。

### 5.3 为选 head 构建临时 BKT

对应：`results/spann_build_16t_nocache.log:65-87`

日志给出的顺序是：

- `Start generating BKT.`
- `Start invoking BuildTrees.`
- `BKTKmeansK: 32, BKTLeafSize: 8, Samples: 1000, BKTLambdaFactor:-1.000000 TreeNumber: 1, ThreadNum: 14.`
- `KmeansArgs: Using none quantizer!`

这说明 SelectHead 不是直接从全量向量里硬筛，而是先构造一棵 BKT 树，把全量数据组织成分层簇结构，然后基于树结构去挑 head。

#### 5.3.1 Lambda 试探日志是什么意思

对应：`results/spann_build_16t_nocache.log:69-75`

这些行如：

- `Lambda:min(1,78.8044) Max:63726 Min:7838 Avg:31250.000000 Std/Avg:0.456314 Dist:...`

可以理解成：

- 程序尝试不同的 `BKTLambdaFactor` 或近似平衡因子。
- 每次尝试会统计聚类分裂的均匀程度、簇大小分布和内部代价。
- 最终根据这些统计挑一个更合适的平衡因子。

最终结果是：

- `Best Lambda Factor:10.000001`

也就是说，在这个数据集和当前参数下，程序认为 `10.000001` 是更合适的构树平衡因子。

#### 5.3.2 真正递归建树

对应：`results/spann_build_16t_nocache.log:77-84`

这几行表示：

- 选完合适的 lambda 后，正式开始建 BKTree。
- 中间继续打印一些局部分裂统计。
- 最终：`1 BKTree built, 1000002 1000000`

这里数字略大于 100 万，通常是因为树节点和数据节点使用了内部统一编号，不代表数据量变了。

#### 5.3.3 临时 BKT 耗时

- `Invoking BuildTrees used time: 2.60 minutes`
- `Finish generating BKT.`

也就是说，SelectHead 阶段的大头时间，已经花在这一步上了。

### 5.4 动态选 head

对应：`results/spann_build_16t_nocache.log:88-396`

这一大段输出在做的是：

> 动态搜索一组 `SelectThreshold` / `SplitThreshold` 参数，使最终选出的 head 数量尽量接近目标比例 `Ratio=0.16`。

#### 5.4.1 每一行的含义

例如：

- `Select Threshold: 4, Split Threshold: 27, diff: -0.00%.`

可以理解为：

- 如果用这组阈值进行 head 选择，那么最终 head 数量和目标 16% 的偏差约为 `diff`。
- `diff` 越接近 0，说明越接近目标比例。

#### 5.4.2 最终选定结果

对应：`results/spann_build_16t_nocache.log:396-399`

最终选择为：

- `Final Select Threshold: 4, Split Threshold: 27.`
- `Seleted Nodes: 159997, about 16.00% of total.`
- `select head time: 165.00s`

这意味着：

- 从 1,000,000 个点中，选出了 **159,997** 个 head 点。
- 占比约 **16.00%**，几乎精确命中目标。

#### 5.4.3 SelectHead 阶段的本质

SelectHead 的目的不是做近邻搜索，而是做一个 **稀疏代表层**：

- 保留一部分“代表性较强”的点作为 head
- 后续完整数据都围绕这些 head 来组织
- 这样 SPANN 的 SSD posting list 才有稳定、可压缩、可分页的上层结构

---

## 6. BuildHead 阶段：只对 159,997 个 head 建内存头索引

阶段开始于：

- `results/spann_build_16t_nocache.log:400`

此时程序的处理对象已经不再是 100 万全量向量，而是刚刚筛出来的 **159,997 个 head**。

### 6.1 BuildHead 参数

对应：`results/spann_build_16t_nocache.log:401-408`

关键参数：

- `TPTNumber = 32`
- `TPTLeafSize = 2000`
- `RefineIterations = 3`
- `NumberOfThreads = 14`
- `NeighborhoodSize = 32`
- `MaxCheckForRefineGraph = 8192`
- `MaxCheck = 8192`

这些参数决定了 head 图构建的规模和精炼深度：

- 每个点的邻居规模目标为 `32`
- 图 refine 做 `3` 轮
- 搜索/图遍历时允许较大的候选访问上限 `8192`

### 6.2 读取 head 向量并重建 head BKT

对应：`results/spann_build_16t_nocache.log:409-418`

- `Load Vector(159997,128)`
- `Begin build index...`
- `Start to build BKTree 1`
- `Build Tree time (s): 11`

这里再次建树，但这次是为 **最终 HeadIndex 服务** 的，不再是 SelectHead 阶段的临时 BKT。

由于数据规模只剩 159,997 条，所以这次建树只花了 **11 秒**，远小于在全量 100 万数据上建临时树的 2.60 分钟。

### 6.3 构建 head 图：TPT 分区 + 初始图 + refine

对应：`results/spann_build_16t_nocache.log:419-520`

这是 BuildHead 的核心。

#### 6.3.1 TPT 分区

- `Parallel TpTree Partition begin`
- `Finish Getting Leaves for Tree 0` 到 `Tree 31`
- `Parallel TpTree Partition done`
- `Build TPTree time (s): 5`

这说明程序将 head 数据进一步按 `TPTNumber=32` 进行分区。可以把它理解成构图前的局部组织步骤：

- 先把点按树叶或局部区域分桶
- 然后优先在局部区域内构造近邻关系

#### 6.3.2 初始 KNN / RNG 图构造

- `Processing Tree X 0% / 20% / 40% / 60% / 80%`
- `Process TPTree time (s): 30`
- `BuildInitKNNGraph time (s): 36`

这里说明：

- 程序逐个处理各个 TPT 分区，构造初始图。
- 不是所有树都会打印到 80%，因为有些分区较小，进度打印点没全命中。
- 最终 36 秒左右生成了初始近邻图。

#### 6.3.3 图 refine

- `Refine 0 0%`
- `Refine RNG time (s): 83 Graph Acc: 0.999531`
- `Refine 1 0%`
- `Refine RNG time (s): 75 Graph Acc: 0.999844`
- `Refine 2 0%`
- `Refine RNG time (s): 37 Graph Acc: 1.000000`

这一步的含义是：

- 先有一个粗略初始图
- 再反复用图搜索和候选更新机制，把每个 head 的邻居逐步修正得更好
- `Graph Acc` 越来越高，最终达到 `1.000000`

从这点可以看出，在 `sift1m` 规模下，这次 BuildHead 非常顺利。

### 6.4 BuildHead 阶段结果

- `BuildGraph time (s): 233`
- `Build Graph time (s): 233`

两个日志内容本质上在表示同一阶段的总耗时。

总体看，BuildHead 阶段做了三件事：

1. 在 head 集合上建 BKT
2. 建初始图
3. 三轮 refine 把图打磨到高精度

最终总耗时约 245 秒。

### 6.5 保存 HeadIndex

对应：`results/spann_build_16t_nocache.log:521-525`

- `SaveIndex(.../HeadIndex) begin...`
- `Save Vector`
- `Save BKT`
- `Save RNG`
- `Save DeleteID`

说明 HeadIndex 的关键组成部分都被落盘了：

- head 向量
- BKT 树
- RNG 图
- 删除标记

---

## 7. HeadIndex 回读：为 BuildSSDIndex 做准备

对应：`results/spann_build_16t_nocache.log:526-570`

这一段不是重复构建，而是：

> 把刚刚构好的 HeadIndex 重新加载进来，作为 BuildSSDIndex 阶段的内存搜索底座。

### 7.1 为什么会重新打印一堆参数

这是因为回读 HeadIndex 的过程中，程序会把对应配置也重新装载一次。这里能看到很多 head index 参数被恢复，例如：

- `BKTNumber = 1`
- `NeighborhoodSize = 32`
- `RefineIterations = 3`
- `MaxCheck = 8192`
- `HashTableExponent = 2`

尤其值得注意的是：

- `results/spann_build_16t_nocache.log:562`：`Setting HashTableExponent with value 2`

这说明：

- **HeadIndex 这部分使用的是 BKT 头索引自己的哈希表参数**
- 在这次运行里，它的值明确是 `2`

### 7.2 回读了哪些结构

- `Load Vector (159997,128) Finish!`
- `Load BKT (1,159999) Finish!`
- `Load RNG (159997,32) Finish!`
- `Load DeleteID (159997,1) Finish!`

这说明 BuildHead 产物已经完整落盘，并成功回载，为下一阶段提供基础。

---

## 8. BuildSSDIndex 阶段：把 100 万向量组织成磁盘 posting list

阶段开始于：

- `results/spann_build_16t_nocache.log:571`

这是 SPANN 真正把“全量数据挂到 head 上”并写入磁盘索引的阶段。

### 8.1 BuildSSDIndex 的核心参数

对应：`results/spann_build_16t_nocache.log:572-574`

- `NumberOfThreads = 1`
- `MaxCheck = 2048`
- `HashTableExponent = 4`

这里和 BuildHead 的参数明显不同：

- BuildHead 回读时使用 `HashTableExponent = 2`
- BuildSSDIndex 阶段切换成了 `HashTableExponent = 4`

也就是说，这两个阶段虽然都属于同一次 SPANN 构建，但内部使用的是 **不同层次的索引参数**：

- `BuildHead`：BKT / head graph 参数
- `BuildSSDIndex`：SPANN SSD 构建参数

### 8.2 读取 head 选择结果和全量组织信息

对应：`results/spann_build_16t_nocache.log:575-577`

- `Load Data (159997,1) Finish!`
- `Loaded 159997 Vector IDs`
- `Full vector count:1000000 Edge bytes:12 selection size:8000000, capacity size:8000000`

其中：

- `159997` 对应 head 数量
- `1000000` 对应全量向量数
- `selection size: 8000000` 与 `ReplicaCount = 8` 对应，说明每个向量预留最多 8 个 replica/head 归属位置

### 8.3 对全量向量做 candidate searching

对应：`results/spann_build_16t_nocache.log:578-582`

- `Preparation done, start candidate searching.`
- `Batch 0 vector(0,1000000) loaded with 1000000 vectors (8000000) HeadIndex acc @64:0.987656.`
- `Searching replicas ended. RNG failed count: 28910963`
- `Search Time: 3.82 mins`

这一步是 BuildSSDIndex 的核心计算过程。它的本质是：

> 对 100 万个全量向量，利用已经构建好的 HeadIndex，为每个向量找到若干合适的 head 或 replica 归属。

#### 8.3.1 为什么只有 `Batch 0`

因为这次配置把 100 万向量一次性装进了一个 batch：

- `vector(0,1000000)`

没有分成多个批次处理。

#### 8.3.2 `HeadIndex acc @64` 是什么

这里可以理解为：

- 用 `InternalResultNum = 64` 的内部候选规模，在 HeadIndex 上做候选查找时，质量/命中率大约是 `0.987656`。
- 它不是最终 ANN 查询 recall，但能反映：**head 层搜索质量已经很高**。

#### 8.3.3 `RNG failed count` 不是致命错误

- `RNG failed count: 28910963`

这个值很大，但它不是崩溃或异常。它只是一个内部统计计数器，反映图搜索或候选扩展中的某类失败/未成功尝试次数。对百万规模数据和大量图遍历来说，绝对值大是正常现象。

### 8.4 候选排序、posting 限制和副本分布

对应：`results/spann_build_16t_nocache.log:583-596`

#### 8.4.1 排序候选

- `Time to sort selections:0.58 sec.`

表示对刚才生成的 `vector -> candidate heads` 结果进行排序整理。

#### 8.4.2 `PostingPageLimit = 12` 为什么变成了 15

日志一开始打印的是：

- `PostingPageLimit = 12`

但这里变成：

- `Build index with posting page limit:15`

这并不矛盾，而是程序内部做了自动修正。

根据源码逻辑，构建时会保证：

- posting page limit 至少大到足以容纳一个 posting list 的理论最大向量数上限。

在 128 维 float 数据上：

- 单条记录约为 `128 * 4 + 4 = 516 bytes`
- 默认 posting vector limit 约为 `118/119`
- 因此 12 页（12 * 4096）不够容纳这么多向量
- 程序自动把页数抬高到 `15`

所以后面紧跟着：

- `Posting size limit: 119`

说明 15 页时单个 posting list 最多可容纳大约 119 条记录。

#### 8.4.3 副本数量分布

- `Replica Count Dist: 0, 0`
- `Replica Count Dist: 1, 885`
- ...
- `Replica Count Dist: 8, 542224`

这表示：

- 每个向量最终保留了多少个 replica/head 归属。
- 有超过 54 万个向量保留了满额 8 个 replica。
- `Replica Count Dist: 0, 0` 表示没有向量完全找不到归属。

从分布看：

- 大量向量都获得了较丰富的 replica 归属
- 这对后续搜索 recall 通常是有利的

### 8.5 磁盘索引写出

对应：`results/spann_build_16t_nocache.log:597-605`

- `TotalPageNumbers: 760084`
- `IndexSize: 3113305612`
- `SubIndex Size: 1921024 bytes`
- `Padded Size: 16864916, final total size: 3113308160.`
- `Output done...`
- `Time to write results:4.77 sec.`

说明：

- 最终 SSD posting list 占用了约 `760,084` 个页。
- 索引主文件规模大约 **3.11 GB**。
- 真正的写盘时间只有不到 5 秒，说明 BuildSSDIndex 的主要时间成本不在文件写出，而在前面的 candidate searching 和归属组织上。

### 8.6 删除位图/版本映射落盘

- `Save versionLabelID To .../DeletedIDs.bin`
- `Save versionLabelID (1000000,1) Finish!`

说明：

- 删除状态/版本信息也一并保存下来了。
- SPANN 最终索引并不只是 posting list 本身，还包括这些配套元信息。

---

## 9. 回读 SSD 索引：验证结构并统计页分布

对应：`results/spann_build_16t_nocache.log:606-634`

这一段不是正式 benchmark 查询，而是构建完成后立即把索引打开一次，检查结构是否合理。

### 9.1 打开异步文件 I/O

- `AsyncFileIO::Open a file`
- `... opened, fd=3 threads=1 maxNumBlocks=64`

说明：

- 读 SSD 索引时使用了异步 I/O 封装。
- 这里线程数仍然是 `1`，与 BuildSSDIndex 配置一致。

### 9.2 读取文件头与整体结构信息

对应：`results/spann_build_16t_nocache.log:610-612`

- `list count 159997`
- `total doc count 1000000`
- `dimension 128`
- `list page offset 469`
- `Big page (>4K): list count 159026, total element count 5998830`
- `Total Element Count: 6000859`

解释：

- `list count = 159997`，正好对应 head 数量。
- `total doc count = 1000000`，对应全量基础向量数。
- `Total Element Count ≈ 600 万`，说明所有 posting list 中累计记录了大约 600 万个元素。

这和前面的 replica 分布是匹配的：

- 并不是每个向量都只属于 1 个 head
- 许多向量被复制到多个 posting list 中

### 9.3 Page Count Dist：每个 posting list 占了多少页

对应：`results/spann_build_16t_nocache.log:613-628`

例如：

- `Page Count Dist: 4 33796`
- `Page Count Dist: 5 35666`
- `Page Count Dist: 8 9559`
- `Page Count Dist: 15 81`

这表示：

- 有多少个 posting list 分别占用了 4 页、5 页、...、15 页。

从这个分布可得出几个结论：

1. 大部分 posting list 集中在 4~8 页。
2. 少量热点 list 才会顶到 14~15 页。
3. `PostingPageLimit=15` 的上限确实被少数 list 撞到了，但不是普遍现象。

这个统计非常重要，因为它直接反映：

- head 负载是否均衡
- posting list 是否过长
- 当前 page limit 设置是否合理

### 9.4 最终内部阶段耗时总结

对应：`results/spann_build_16t_nocache.log:629-632`

- `Current vector num: 1000000.`
- `select head time: 165.00s build head time: 245.00s build ssd time: 233.00s`

这是程序内部给出的三大核心阶段耗时：

- SelectHead：165 秒
- BuildHead：245 秒
- BuildSSDIndex：233 秒

---

## 10. 收尾与外层监控摘要

对应：`results/spann_build_16t_nocache.log:633-660`

### 10.1 程序正常关闭

- `AsyncFileReader: Destroying fd=3!`
- `AsyncFileReader: ShutDown!`

说明 I/O 资源正常回收，没有卡住或异常退出。

### 10.2 监控脚本汇总

#### 总时长

- `总运行时间: 646.7 秒`

内部三大阶段和为：

- `165 + 245 + 233 = 643 秒`

两者相差不多，差值基本来自：

- 启动
- 资源初始化
- 文件回读
- 收尾
- 外层监控脚本本身开销

#### CPU

- `平均 513.7%`
- `峰值 844.00%`

这说明：

- 虽然部分阶段配置为 14 线程，但全过程不是始终满负载并行
- 平均大约使用 5 个逻辑核心
- 峰值约 8.4 个核心

这是合理的，因为：

- BuildSSDIndex 本身是 1 线程
- 有些步骤存在同步、排序、I/O 等串行部分
- 监控采样是按秒统计，峰值会被平滑

#### 内存

- `RSS 平均 476.9 MB, 峰值 766.07 MB`
- `VSZ 峰值 1653.82 MB`

对 `sift1m` 规模、head 约 16% 的 SPANN 构建来说，这个内存占用是比较正常的。

#### 磁盘 I/O

- `累计读取: 492 MB`
- `累计写入: 180 MB`

需要注意：

- 这是监控脚本对特定挂载盘的统计口径，不等于全系统所有 I/O。
- 也不一定等于最终索引文件实际大小，因为存在页缓存和不同文件路径的统计差异。

#### 阶段耗时摘要

- `SelectHead: 166.0s`
- `BuildHead: 245.4s`

这里没有单独列出 `BuildSSDIndex`，并不是它没执行，而是监控脚本的 footer 没把该阶段单独展示出来。BuildSSDIndex 的真实耗时应以程序内部日志 `results/spann_build_16t_nocache.log:632` 为准，即约 233 秒。

---

## 11. 这份日志反映出的关键结论

### 11.1 构建是成功完成的

整份日志中没有出现：

- 崩溃
- 断言失败
- 明显的 `FAILED`/`Aborted`
- I/O 初始化失败

所以这是一次 **成功完成的 SPANN 构建**。

### 11.2 SelectHead 命中了目标比例

- 最终选出 `159,997` 个 head
- 占总量约 `16.00%`

说明动态阈值搜索工作正常。

### 11.3 BuildHead 质量很高

- 三轮 refine 后 `Graph Acc` 达到 `1.000000`

说明 head 层图质量很好，为后续 SSD posting 分配提供了可靠的上层索引。

### 11.4 BuildSSDIndex 的主要成本在候选搜索而不是写盘

- 搜索 replicas：`3.82 mins`
- 写结果：`4.77 sec`

可见这一步的大头是计算，不是输出。

### 11.5 PostingPageLimit 存在内部自动修正

- 配置初值：`12`
- 实际构建使用：`15`

说明不能只看配置文件字面值，还要看日志中的实际生效值。

### 11.6 这份 1M BuildHead 日志没有出现 Hash table 扩容告警

和 10M 当前构建相比，这份 `sift1m` 的 BuildHead 没有出现 `Hash table is full`。这说明：

- 在 `sift1m` 规模下，BuildHead 使用的默认 `HashTableExponent=2` 足够。
- 但在 `sift10m` 的 BuildHead / Refine 阶段，默认值可能开始显得偏保守。

---

## 12. 一句话总结

这份 `spann_build_16t_nocache.log` 完整展示了一次 `sift1m` SPANN 索引构建流程：

- 先从 100 万向量中选出约 16% 的 head
- 再在这批 head 上构建 BKT + RNG 头索引
- 然后把全部向量组织为磁盘 posting list
- 最后回读并统计 SSD 索引页分布

整体运行稳定、阶段边界清晰、参数生效合理，是一份非常标准且成功的 SPANN 构建日志。
