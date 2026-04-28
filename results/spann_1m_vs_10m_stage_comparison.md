# SPANN 1M vs 10M 构建日志逐阶段对照分析

本文对比两份日志：

- 1M：`results/spann_build_16t_nocache.log`
- 10M：`results/spann10m_build_nocache.log`

目标是回答两个问题：

1. 10M 为什么比 1M 慢很多？
2. 具体慢在哪些阶段？

---

## 1. 总体结论

如果只看总时长：

- 1M 总时长：`646.7s`
- 10M 总时长：`7094.4s`

对应：

- `results/spann_build_16t_nocache.log:640`
- `results/spann10m_build_nocache.log:682`

总放大倍数约为：

- `7094.4 / 646.7 ≈ 10.97x`

看起来像是“数据量从 100 万变成 1000 万，所以时间也约 10 倍”，但如果拆阶段，会发现并不是所有阶段都只是线性变慢。

最重要的结论是：

> **10M 最慢、最超预期的部分不是 SelectHead，也不是纯粹的 SSD 写盘，而是 BuildHead 阶段，尤其是其中的图 refine。**

---

## 2. 核心阶段耗时对照

从程序内部汇总可以直接读到三大阶段时间：

### 1M

- `select head time: 165.00s`
- `build head time: 245.00s`
- `build ssd time: 233.00s`

见：`results/spann_build_16t_nocache.log:632`

### 10M

- `select head time: 1625.00s`
- `build head time: 4569.00s`
- `build ssd time: 892.00s`

见：`results/spann10m_build_nocache.log:674`

整理如下：

| 阶段 | 1M | 10M | 放大倍数 |
|---|---:|---:|---:|
| SelectHead | 165s | 1625s | 9.8x |
| BuildHead | 245s | 4569s | 18.6x |
| BuildSSDIndex | 233s | 892s | 3.8x |
| 总时长 | 647s | 7094s | 11.0x |

从这个表里直接可以看出：

- **SelectHead：接近 10 倍，基本线性放大**
- **BuildHead：18.6 倍，明显超线性，是主要异常瓶颈**
- **BuildSSDIndex：只有 3.8 倍，反而没那么夸张**

---

## 3. 数据规模与 head 规模的变化

### 3.1 原始数据规模

- 1M：`Begin initial data (1000000,128)...`
  - `results/spann_build_16t_nocache.log:63`
- 10M：`Begin initial data (10000000,128)...`
  - `results/spann10m_build_nocache.log:63`

### 3.2 SelectHead 之后的 head 数量

两边都把比例控制在约 16%：

- 1M：`Seleted Nodes: 159997, about 16.00% of total.`
  - `results/spann_build_16t_nocache.log:397`
- 10M：`Seleted Nodes: 1600290, about 16.00% of total.`
  - `results/spann10m_build_nocache.log:399`

这点非常关键：

> 10M 不只是“全量数据 10 倍”，而且 **head 层也从 16 万扩大到 160 万**。

因此：

- `SelectHead` 要处理 10 倍原始数据
- `BuildHead` 要处理 10 倍 head 数据
- `BuildSSDIndex` 也要面对 10 倍全量数据和 10 倍 head 结构

---

## 4. 阶段一：SelectHead 对照分析

---

### 4.1 关键时间对照

#### 1M

- `Invoking BuildTrees used time: 2.60 minutes`
  - `results/spann_build_16t_nocache.log:86`
- `select head time: 165.00s`
  - `results/spann_build_16t_nocache.log:399`

#### 10M

- `Invoking BuildTrees used time: 25.45 minutes`
  - `results/spann10m_build_nocache.log:88`
- `select head time: 1625.00s`
  - `results/spann10m_build_nocache.log:401`

### 4.2 放大倍数

- `1625 / 165 ≈ 9.85x`

### 4.3 解释

SelectHead 阶段的主要工作是：

1. 在全量数据上建一棵临时 BKT
2. 根据树结构动态搜索阈值，选出约 16% 的 head

10M 相比 1M：

- 原始向量数量正好约 10 倍
- 耗时也约 10 倍

这说明：

- **SelectHead 虽然慢，但慢得基本合理**
- 它更像是“工作量自然扩大”的结果，而不是出现了明显退化

### 4.4 结论

> **10M 的 SelectHead 不是主要异常瓶颈。**
>
> 它慢，但基本属于正常线性放大。

---

## 5. 阶段二：BuildHead 对照分析

这是 10M 最大的瓶颈。

---

### 5.1 总体耗时对照

- 1M：`245s`
  - `results/spann_build_16t_nocache.log:570`
- 10M：`4569s`
  - `results/spann10m_build_nocache.log:612`

放大倍数：

- `4569 / 245 ≈ 18.6x`

这已经显著高于“数据 10x”的直觉。

也就是说：

> **BuildHead 在 10M 上出现了明显超线性放大。**

---

### 5.2 BuildHead 子步骤拆分

BuildHead 可以大致拆成三块：

1. 在 head 集合上重新建 BKT
2. 构造初始图（BuildInitKNNGraph）
3. 多轮 refine

#### 5.2.1 重新建 head BKT

- 1M：`Build Tree time (s): 11`
  - `results/spann_build_16t_nocache.log:418`
- 10M：`Build Tree time (s): 124`
  - `results/spann10m_build_nocache.log:422`

放大倍数：

- `124 / 11 ≈ 11.3x`

解释：

- 这一步处理的是 head 集合
- head 数量从约 16 万扩大到约 160 万
- 时间也约扩大 11 倍，属于比较正常的规模放大

结论：

> **重新建 BKT 不是 BuildHead 最痛的点。**

---

#### 5.2.2 初始图构造（BuildInitKNNGraph）

- 1M：`BuildInitKNNGraph time (s): 36`
  - `results/spann_build_16t_nocache.log:512`
- 10M：`BuildInitKNNGraph time (s): 421`
  - `results/spann10m_build_nocache.log:516`

放大倍数：

- `421 / 36 ≈ 11.7x`

解释：

- 这一步已经开始比纯粹 10 倍数据略重
- 主要是因为在 10M 下：
  - head 点更多
  - TPT 分区内点数更高
  - 候选连接与局部图组织更拥挤

但整体还没有到最夸张的程度。

结论：

> **初始图构造已经明显变重，但仍不是最大瓶颈。**

---

#### 5.2.3 Refine —— 10M 最慢的根因

##### 1M refine

- `Refine RNG time (s): 83 Graph Acc: 0.999531`
  - `results/spann_build_16t_nocache.log:514`
- `Refine RNG time (s): 75 Graph Acc: 0.999844`
  - `results/spann_build_16t_nocache.log:516`
- `Refine RNG time (s): 37 Graph Acc: 1.000000`
  - `results/spann_build_16t_nocache.log:518`

总 refine 时间约：

- `83 + 75 + 37 = 195s`

##### 10M refine

- `Refine RNG time (s): 1779 Graph Acc: 0.997656`
  - `results/spann10m_build_nocache.log:536`
- `Refine RNG time (s): 1646 Graph Acc: 1.000000`
  - `results/spann10m_build_nocache.log:554`
- `Refine RNG time (s): 591 Graph Acc: 0.998125`
  - `results/spann10m_build_nocache.log:560`

总 refine 时间约：

- `1779 + 1646 + 591 = 4016s`

##### 放大倍数

- `4016 / 195 ≈ 20.6x`

这已经是 BuildHead 里最明显的超线性部分。

---

### 5.3 为什么 10M 的 refine 会超线性变慢

#### 原因 1：head 图规模扩大 10 倍，但图遍历代价不是纯线性的

Refine 做的事情不是简单扫一遍点，而是会反复：

- 扩展候选
- 去重
- 计算距离
- 更新近邻关系
- 再次扩展

当 head 点从约 16 万变成约 160 万时：

- 候选路径会更多
- 重复命中的点会更多
- 每一轮图优化的局部搜索会更重

因此它天然比“单次建树”更容易出现超线性放大。

#### 原因 2：10M 出现了哈希去重表扩容告警，1M 没有

10M 在 refine 中出现了大量：

- `Hash table is full! Set HashTableExponent to larger value ... NewHashTableExponent=3`

见：

- `results/spann10m_build_nocache.log:518-528`
- `results/spann10m_build_nocache.log:530-554`

而 1M 的 BuildHead 日志中没有这类告警。

这说明：

- 1M 下默认的 visited/deduper 哈希表够用
- 10M 下相同默认值已经开始吃紧

10M 回读 head index 时还能看到：

- `Setting HashTableExponent with value 2`
  - `results/spann10m_build_nocache.log:604`

这意味着：

- BuildHead 初始还是从较小的 `HashTableExponent=2` 起步
- refine 中不断因为冲突/容量不足而触发扩容

虽然扩容不会让构建失败，但会增加额外成本：

1. 重新分配更大的哈希表
2. 重新插入已有 visited 元素
3. 带来更多内存访问和 cache miss

这会把 refine 进一步拖慢。

#### 原因 3：BuildHead 参数没有随 10M 规模同步放大

1M 与 10M 的 BuildHead 参数几乎相同：

- `NeighborhoodSize = 32`
- `MaxCheck = 8192`
- `MaxCheckForRefineGraph = 8192`
- `RefineIterations = 3`
- `TPTNumber = 32`

对应：

- 1M：`results/spann_build_16t_nocache.log:401-408`
- 10M：`results/spann10m_build_nocache.log:403-410`

也就是说：

- 数据规模大了 10 倍
- head 数量大了 10 倍
- 但很多控制分区粒度、候选搜索、visited 容量的参数没有同步调整

这会导致：

- 每个 TPT 分区平均更大
- 候选图遍历更拥挤
- visited 哈希表更容易不够用

#### 原因 4：10M 的 BuildHead 内存压力大得多

监控摘要显示：

##### 1M

- `RSS 峰值 766.07 MB`
  - `results/spann_build_16t_nocache.log:644`

##### 10M

- `RSS 峰值 7152.09 MB`
  - `results/spann10m_build_nocache.log:686`

几乎是：

- `7152 / 766 ≈ 9.3x`

这说明 10M 的 BuildHead 期间，工作集已经非常大：

- 图结构更大
- visited 集更大
- 候选队列更大
- 内存带宽和 CPU cache 压力更明显

因此 refine 的实际执行速度会进一步下降。

---

### 5.4 BuildHead 阶段总结

如果拆 BuildHead：

- 重新建 BKT：11s → 124s，基本正常
- 初始图构造：36s → 421s，明显变重
- refine：195s → 4016s，严重超线性

所以最核心的结论是：

> **10M 慢的头号原因是 BuildHead，而 BuildHead 里最慢的就是 refine。**

---

## 6. 阶段三：BuildSSDIndex 对照分析

相比 BuildHead，这一段虽然也慢，但没有那么“异常”。

---

### 6.1 总体耗时对照

- 1M：`233s`
  - `results/spann_build_16t_nocache.log:632`
- 10M：`892s`
  - `results/spann10m_build_nocache.log:674`

放大倍数：

- `892 / 233 ≈ 3.8x`

这比 10 倍小很多。

### 6.2 为什么只慢了 3.8 倍

因为线程数变了：

- 1M：`Setting NumberOfThreads with value 1`
  - `results/spann_build_16t_nocache.log:572`
- 10M：`Setting NumberOfThreads with value 14`
  - `results/spann10m_build_nocache.log:614`

也就是说：

- 数据量从 100 万变成 1000 万
- 但 BuildSSDIndex 从 1 线程提高到了 14 线程

因此很多规模增长被并行吞掉了。

结论：

> **BuildSSDIndex 并没有像 BuildHead 那样严重退化。**

---

### 6.3 Candidate searching 对照

#### 1M

- `Searching replicas ended. Search Time: 3.82 mins`
  - `results/spann_build_16t_nocache.log:582`

#### 10M

- `Searching replicas ended. Search Time: 13.78 mins`
  - `results/spann10m_build_nocache.log:624`

放大倍数：

- `13.78 / 3.82 ≈ 3.6x`

说明：

- 数据量 10x
- 但由于 10M 这里用了 14 线程，时间只变成了 3.6 倍

这也是为什么 BuildSSDIndex 总体没有成为最突出的瓶颈。

---

### 6.4 HeadIndex 候选质量下降

#### 1M

- `HeadIndex acc @64:0.987656`
  - `results/spann_build_16t_nocache.log:579`

#### 10M

- `HeadIndex acc @64:0.940938`
  - `results/spann10m_build_nocache.log:621`

这说明：

- 同样的 `InternalResultNum = 64`
- 在 10M 的 head 层上，候选质量明显下降

可以理解为：

- 160 万个 head 比 16 万个 head 更难搜
- 同样只保留 64 个内部候选时，10M 的 head 层搜索就显得偏紧

这也会带来额外代价：

- 候选搜索中的无效尝试更多
- 后续 replica 组织压力更大

对应地，`RNG failed count` 也更高：

- 1M：`28910963`
  - `results/spann_build_16t_nocache.log:580`
- 10M：`179597107`
  - `results/spann10m_build_nocache.log:622`

它不表示失败退出，但说明图搜索中的尝试成本明显增加。

---

### 6.5 排序与输出在 10M 上开始变得很贵

#### 排序 selections

- 1M：`Time to sort selections:0.58 sec`
  - `results/spann_build_16t_nocache.log:583`
- 10M：`Time to sort selections:12.99 sec`
  - `results/spann10m_build_nocache.log:625`

放大倍数：

- `12.99 / 0.58 ≈ 22.4x`

排序本身不是总时间主导项，但已经开始明显变重。

#### 最终索引大小

- 1M：`IndexSize: 3113305612`（约 3.11 GB）
  - `results/spann_build_16t_nocache.log:597`
- 10M：`IndexSize: 33149462544`（约 33.15 GB）
  - `results/spann10m_build_nocache.log:639`

放大倍数约：

- `33.15 / 3.11 ≈ 10.6x`

#### 写盘时间

- 1M：`Time to write results:4.77 sec`
  - `results/spann_build_16t_nocache.log:602`
- 10M：`Time to write results:104.19 sec`
  - `results/spann10m_build_nocache.log:644`

放大倍数约：

- `104.19 / 4.77 ≈ 21.8x`

这说明：

- 到 10M 时，输出不再是“很小的尾巴”
- 33GB 级别的 posting list 文件已经让写盘时间明显可见

但即便如此，BuildSSDIndex 总耗时仍然明显小于 BuildHead。

---

## 7. 磁盘与内存层面的补充对照

### 7.1 内存

#### 1M

- `RSS 平均 476.9 MB, 峰值 766.07 MB`
  - `results/spann_build_16t_nocache.log:643-645`

#### 10M

- `RSS 平均 2898.9 MB, 峰值 7152.09 MB`
  - `results/spann10m_build_nocache.log:685-687`

解释：

- 10M 的工作集大得多
- 这对 BuildHead 尤其不利，因为 refine 是典型的内存访问密集型工作

### 7.2 磁盘写入

#### 1M

- `累计写入: 180 MB`
  - `results/spann_build_16t_nocache.log:647-649`

#### 10M

- `累计写入: 26587 MB`
  - `results/spann10m_build_nocache.log:689-691`

说明：

- 10M 的磁盘输出规模已经非常显著
- 但整体总时间上，磁盘不是最主要的慢点，真正最重的阶段仍然是 BuildHead refine

---

## 8. 最终归因：10M 为什么慢，慢在哪

可以把结论归纳成三层。

### 8.1 第一层：基础规模放大

这是所有阶段变慢的底层原因：

- 原始数据：100 万 → 1000 万
- head 数量：16 万 → 160 万
- SSD 索引体积：约 3.11GB → 33.15GB

这会让所有阶段都有“自然放大”。

### 8.2 第二层：BuildHead refine 出现超线性退化

这是最核心的原因：

- BuildHead 总时长：245s → 4569s（18.6x）
- refine 总时长：195s → 4016s（20.6x）
- 10M 还出现大量 `Hash table is full` 扩容告警

说明：

- head 图 refine 在 160 万 head 上不再是简单线性代价
- visited/deduper 哈希表也开始不够用
- 这部分带来了 10M 的主要额外时间

### 8.3 第三层：BuildSSDIndex 虽然没爆炸，但输出代价变大

10M 的 BuildSSDIndex：

- 候选搜索时间明显增长
- `HeadIndex acc @64` 下降
- 排序更贵
- 写盘从几秒增长到 100 秒级

但因为线程从 1 提高到 14，整体没有像 BuildHead 一样失控。

---

## 9. 一句话总结

> **10M 比 1M 慢，最根本的原因不是“磁盘慢了”，而是 head 层也扩大了 10 倍，导致 BuildHead 尤其是图 refine 超线性变重；与此同时，BuildSSDIndex 靠更多线程压住了计算时间，但 33GB 级别的输出仍然让排序和写盘变得明显昂贵。**

---

## 10. 如果只记三个结论

1. **SelectHead 慢得基本正常，接近线性放大，不是主要异常点。**
2. **BuildHead 是最大瓶颈，尤其 refine 从 195s 膨胀到 4016s，是 10M 变慢的核心。**
3. **BuildSSDIndex 不是最坏的问题，但 10M 的大规模输出已经把排序和写盘成本显著抬高。**
