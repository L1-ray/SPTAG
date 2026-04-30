# SIFT1M 官方参数对齐与最小页数限制总结

## 1. 本次任务目标

本次工作的目标是：

1. 参考官方配置文件：
   - `Script_AE/iniFile/build_sift1m.ini`
   - `Script_AE/iniFile/store_sift1m/indexloader_sift1m.ini`
2. 在**同名参数尽量与官方保持一致**的前提下，使用当前机器可用的数据集路径重新构建 SIFT1M 的 SPANN 索引；
3. 执行一次新的 **SPANN 搜索阶段细粒度 I/O 性能测试与分析**；
4. 使用新的索引目录和结果目录，避免覆盖已有历史实验结果。

由于本机不存在官方 AE 配置中使用的 `/home/sosp/data/...` 数据目录，因此最终采用的策略是：

- **算法/构建/搜索同名参数对齐官方**；
- **数据路径继续映射到本机已有的 `/home/ray/data/sift1m` 数据**；
- **所有新产物写入新的 namespace**，不覆盖旧索引和旧结果。

---

## 2. 制定的实施计划

实施计划的核心内容如下：

### 2.1 以官方 ini 作为参数真值来源
不修改以下官方文件，仅将其作为参数参考：

- `Script_AE/iniFile/build_sift1m.ini`
- `Script_AE/iniFile/store_sift1m/indexloader_sift1m.ini`

### 2.2 只修改当前 workflow 的规范入口文件
计划修改的入口文件：

- `configs/spann_build_only.ini`
- `configs/spann_search_only.ini`
- `configs/spann_search_io_analysis.ini`
- `TEST_COMMANDS.md`

不改动历史结果快照和历史线程特化配置，例如：

- `configs/spann_search_io_analysis_2t.ini`
- `configs/spann_search_io_analysis_4t.ini`
- `configs/spann_search_io_analysis_8t.ini`
- `configs/spann_search_io_analysis_12t.ini`
- `results/io_analysis/**/config.ini`

### 2.3 使用新 namespace 防止覆盖
本次采用的新命名空间：

- 新索引目录：`/home/ray/data/sift1m/spann_index_official_sift1m_20260429`
- 新临时目录：`/home/ray/data/sift1m/tmp/official_sift1m_20260429`
- 新结果根目录：`/home/ray/code/SPTAG/results/io_analysis/sift1m_official_20260429`
- baseline 运行目录：`/home/ray/code/SPTAG/results/io_analysis/sift1m_official_20260429/baseline_st2_nt40_ir64_pl4`

### 2.4 baseline 测试顺序
1. 修改 build / search / I/O 分析配置；
2. 重建 SPANN 索引；
3. 用 `run_io_analysis.sh` 执行一次 baseline 细粒度 I/O 测试；
4. 保存配置快照与结果文件；
5. 检查 Recall / QPS / latency / read bandwidth 等指标。

---

## 3. 已做出的相关修改

### 3.1 `configs/spann_build_only.ini`

将 build 规范入口改为“官方同名构建参数 + 本机数据路径映射”的版本，主要修改包括：

#### SelectHead 相关参数
- `SelectThreshold=12`
- `SplitFactor=9`
- `SplitThreshold=18`
- `Ratio=0.15`
- `NumberOfThreads=80`
- 同时补入了官方配置里的：
  - `AnalyzeOnly=false`
  - `CalcStd=true`
  - `SelectDynamically=true`
  - `NoOutput=false`
  - `RecursiveCheckSmallCluster=true`
  - `PrintSizeCount=true`

#### BuildHead 相关参数
- `MaxCheck=4096`
- `MaxCheckForRefineGraph=8192`
- `RefineIterations=2`
- `HashTableExponent=2`
- `NumberOfThreads=160`

#### BuildSSDIndex 相关参数
- `InternalResultNum=64`
- `ReplicaCount=8`
- `PostingPageLimit=3`
- `NumberOfThreads=40`
- `TmpDir=/home/ray/data/sift1m/tmp/official_sift1m_20260429`
- `UseSPDK=true`
- `UseDirectIO=true`

#### 输出路径修改
- `IndexDirectory=/home/ray/data/sift1m/spann_index_official_sift1m_20260429`
- `SearchResult=/home/ray/code/SPTAG/results/io_analysis/sift1m_official_20260429/search_results.bin`

### 3.2 `configs/spann_search_only.ini`

将搜索规范入口切到新的 official baseline 口径，主要参数包括：

- `InternalResultNum=64`
- `NumberOfThreads=40`
- `SearchThreadNum=2`
- `ResultNum=10`
- `MaxDistRatio=1000000`
- `SearchPostingPageLimit=4`

同时切换输出路径：

- `IndexDirectory=/home/ray/data/sift1m/spann_index_official_sift1m_20260429`
- `SearchResult=/home/ray/code/SPTAG/results/io_analysis/sift1m_official_20260429/search_results.bin`

### 3.3 `configs/spann_search_io_analysis.ini`

在与 `spann_search_only.ini` 一致的 baseline 搜索参数基础上，保留并更新 I/O 分析相关配置：

- `EnableDetailedIOStats=true`
- `DetailedIOStatsSampleRate=1.0`
- `DetailedIOStatsOutput=/home/ray/code/SPTAG/results/io_analysis/sift1m_official_20260429/baseline_st2_nt40_ir64_pl4/query_io_stats.csv`

### 3.4 线程数优化（基于本机实测）

经过 Build 阶段和 Search 阶段的对比测试，发现官方线程配置在本机上存在严重的线程竞争问题，优化后的线程配置如下：

#### Build 阶段线程优化

| 参数 | 官方配置 | 优化配置 | 加速比 |
| --- | --- | --- | --- |
| `SelectHead.NumberOfThreads` | 80 | 16 | **5.3x** |
| `BuildHead.NumberOfThreads` | 160 | 16 | **3.1x** |
| `BuildSSDIndex.NumberOfThreads` | 40 | 16 | **1.3x** |

**Build 总时间对比**：
- 官方配置（80/160/40）：**12m53s**
- 优化配置（16/16/16）：**2m57s**
- **总体加速：4.4 倍**

#### Search 阶段线程优化

| 参数 | 官方配置 | 优化配置 | 说明 |
| --- | --- | --- | --- |
| `SearchThreadNum` | 2 | 8 | QPS 提升 2.5 倍 |
| `NumberOfThreads` | 40 | 16 | 减少无效开销 |

**Search 性能对比**（ir=64, pl=4）：
- 官方配置（st=2, nt=40）：QPS = 2417, avg latency = 0.827 ms
- 优化配置（st=8, nt=16）：QPS = 5945, avg latency = 1.344 ms
- **QPS 提升：2.5 倍**

#### 优化结论

1. **线程数过多会导致严重的线程竞争和同步开销**
2. **Build 阶段**：三个阶段统一使用 16 线程最优
3. **Search 阶段**：`SearchThreadNum=8, NumberOfThreads=16` 是最优配置
4. **索引结构不受影响**：线程数只影响速度，不影响索引结构和搜索结果

### 3.5 `TEST_COMMANDS.md`

对文档中的 SIFT1M I/O workflow 做了更新：

- 明确说明当前默认 workflow 为：
  - **官方同名算法参数对齐**
  - **本机 `/home/ray/data/sift1m` 数据路径映射**
- 将 baseline 结果目录改为：
  - `results/io_analysis/sift1m_official_20260429/baseline_st2_nt40_ir64_pl4`
- 将 baseline 搜索参数示例更新为：
  - `InternalResultNum=64`
  - `NumberOfThreads=40`
  - `SearchThreadNum=2`
  - `SearchPostingPageLimit=4`
  - `MaxDistRatio=1000000`
- 补充说明先重建索引，再执行 baseline I/O 分析。

---

## 4. 执行结果

### 4.1 索引重建完成
新的索引目录已成功生成：

- `/home/ray/data/sift1m/spann_index_official_sift1m_20260429`

目录中可见产物包括：

- `DeletedIDs.bin`
- `HeadIndex/`
- `SPTAGFullList.bin`
- `SPTAGHeadVectorIDs.bin`
- `SPTAGHeadVectors.bin`

### 4.2 baseline 细粒度 I/O 测试完成
baseline 运行目录：

- `/home/ray/code/SPTAG/results/io_analysis/sift1m_official_20260429/baseline_st2_nt40_ir64_pl4`

生成文件包括：

- `config.ini`
- `query_io_stats.csv`
- `disk_stats.csv`
- `process_io_stats.csv`
- `cpu_stats.csv`
- `psi_io_stats.csv`
- `report.md`
- `summary.txt`
- `sptag.log`

另外保存了 build 配置快照：

- `/home/ray/code/SPTAG/results/io_analysis/sift1m_official_20260429/build_config.ini`

### 4.3 baseline 指标
根据 `report.md` 与 `sptag.log`：

- `QPS = 151.490`
- `avg_latency_ms = 13.191`
- `p95_latency_ms = 15.256`
- `p99_latency_ms = 16.367`
- `avg_read_bandwidth_mbs = 224.737`
- `Recall@10 = 0.977298`

结论仍为：

- **存在混合瓶颈，I/O 不是唯一主导**

---

## 5. 关于“最小页数限制”的发现

本次最重要的额外发现是：

> 配置中虽然设置了 `PostingPageLimit=3`，但在实际构建和加载过程中，它被内部代码自动提升为了 `15`。

### 5.1 现象
在构建日志中可以看到：

- `Build index with posting page limit:15`

在搜索加载日志中也可以看到：

- `Load index with posting page limit:15`

这说明：

- `PostingPageLimit=3` 并未按字面直接生效；
- 系统内部对 posting page limit 有一个**最小页数约束**；
- build 阶段和 load/search 阶段都会执行这一约束。

### 5.2 本地代码中的相关实现

#### build 阶段约束
文件：`AnnService/inc/Core/SPANN/ExtraStaticSearcher.h`

核心逻辑：

```cpp
p_opt.m_postingPageLimit = max(
    p_opt.m_postingPageLimit,
    static_cast<int>((p_opt.m_postingVectorLimit * vectorInfoSize + PageSize - 1) / PageSize)
);
```

#### load/search 阶段约束
同文件中的加载逻辑：

```cpp
p_opt.m_searchPostingPageLimit = max(
    p_opt.m_searchPostingPageLimit,
    static_cast<int>((p_opt.m_postingVectorLimit * (p_opt.m_dim * sizeof(ValueType) + sizeof(int)) + PageSize - 1) / PageSize)
);
```

#### 默认约束源参数
文件：`AnnService/inc/Core/SPANN/ParameterDefinitionList.h`

```cpp
DefineSSDParameter(m_postingVectorLimit, int, 118, "PostingVectorLimit")
```

也就是说，最小页数来自：

- `PostingVectorLimit=118`
- 当前向量记录大小 `vectorInfoSize`
- 页大小 `PageSize`

### 5.3 为什么会变成 15

当前测试数据不是官方 AE 的 `UInt8 + DEFAULT` 格式，而是：

- `Float`
- `XVEC`
- `Dim=128`

在这种情况下，每条向量记录的大小大约是：

- `128 * sizeof(float) + sizeof(int)`
- 即约 `516 bytes`

因此最小页数约束近似为：

- `ceil(118 * 516 / 4096)`
- 结果约等于 `15`

所以：

- 配置写 `PostingPageLimit=3`
- 实际运行时会被 clamp 到 `15`

### 5.4 为什么数据格式不同仍然可以正常运行

虽然本次把很多“同名参数”对齐到了官方，但**并没有完全复现官方 AE 的数据制式**：

- 官方 AE 使用：`UInt8 + DEFAULT`
- 本次实际运行使用：`Float + XVEC`

之所以仍然能够正常构建和搜索，是因为 SPTAG 本身就支持多种：

- `ValueType`（如 `Float`、`UInt8`）
- `VectorType`（如 `DEFAULT`、`XVEC`）

只要以下三者彼此匹配，程序就可以正常运行：

1. `ValueType`
2. `VectorType`
3. 底层数据文件的真实编码格式

本次运行时，这三者是匹配的：

- `ValueType=Float`
- `VectorType=XVEC`
- 数据文件是 `.fvecs/.ivecs`

因此程序会走 Float/XVEC 的 reader 与距离计算路径，而不是官方 AE 的 UInt8/DEFAULT 路径。

这也是为什么：

- 虽然“同名参数”很多已经对齐官方；
- 但 `vectorInfoSize` 仍然不同；
- clamp 后的最小页数不同；
- 最终 posting 物理布局和 search I/O 特征也会与官方 AE 运行时表现不同。

更准确的说法是：

> **最小页数约束逻辑与官方一致，但在当前 `Float + XVEC` 数据格式下，约束结果会和官方 AE 的 `UInt8 + DEFAULT` 场景不同。**

### 5.5 这段逻辑是否与官方仓库一致

已检查，结论是：

> **当前仓库中的最小页数限制逻辑与官方 `microsoft/SPTAG` 的 `main` 分支一致，并不是本地额外修改出来的行为。**

一致点包括：

1. build 阶段对 `PostingPageLimit` 的 `max(...)` clamp；
2. load/search 阶段对 `SearchPostingPageLimit` 的 `max(...)` clamp；
3. `PostingVectorLimit=118` 的默认值。

因此，本次观察到的 `3 -> 15` 不是本地补丁导致，而是**官方代码逻辑在当前 float/XVEC 数据格式下的自然结果**。

---

## 6. 严格 `UInt8 + DEFAULT` 复现结果

在确认本机已经具备官方数据制式对应文件之后，又进一步执行了一轮更严格的复现：

- `ValueType=UInt8`
- `VectorType=DEFAULT`
- `QueryType=DEFAULT`
- `TruthType=DEFAULT`
- 数据路径使用本机：`/home/ray/data/sift1m`

这次复现与前面的“官方同名参数对齐 + 本机 Float/XVEC 数据映射”不同，关键区别在于：

- 不再使用 `Float + XVEC`
- 改为真正使用官方 AE 对应的 `UInt8 + DEFAULT` 数据制式
- 因此 posting 物理布局与 search I/O 行为也更接近官方场景

### 6.1 严格复现使用的数据文件

本次严格复现使用：

- `/home/ray/data/sift1m/bigann1m_base.u8bin`
- `/home/ray/data/sift1m/query.public.10K.u8bin`
- `/home/ray/data/sift1m/bigann-1M.bin`

其中：

- `bigann1m_base.u8bin` 头部与大小匹配 `1000000 x 128` 的 `UInt8 + DEFAULT`
- `query.public.10K.u8bin` 头部与大小匹配 `10000 x 128` 的 `UInt8 + DEFAULT`
- `bigann-1M.bin` 头部为 `10000 x 100`，可作为 `TruthType=DEFAULT` 使用

### 6.2 严格复现对应的配置与命名空间

本次切换后的 canonical 配置文件：

- `configs/spann_build_only.ini`
- `configs/spann_search_only.ini`
- `configs/spann_search_io_analysis.ini`

新的 strict namespace：

- 索引目录：`/home/ray/data/sift1m/spann_index_official_u8default_20260430`
- 临时目录：`/home/ray/data/sift1m/tmp/official_u8default_20260430`
- 结果根目录：`/home/ray/code/SPTAG/results/io_analysis/sift1m_official_u8default_20260430`

baseline 运行目录：

- `results/io_analysis/sift1m_official_u8default_20260430/baseline_st2_nt40_ir64_pl4`

冷缓存重跑目录：

- `results/io_analysis/sift1m_official_u8default_20260430/baseline_st2_nt40_ir64_pl4_rerun`

### 6.3 索引构建结果

严格复现索引已成功生成：

- `/home/ray/data/sift1m/spann_index_official_u8default_20260430`

关键产物包括：

- `DeletedIDs.bin`
- `head_index/`
- `SPTAGFullList.bin`
- `SPTAGHeadVectorIDs.bin`
- `SPTAGHeadVectors.bin`

### 6.4 最小页数行为已切换到官方数据制式口径

在这次 `UInt8 + DEFAULT` 严格复现中，日志显示：

- `Build index with posting page limit:4`
- `Load index with posting page limit:4`

这与前面 `Float + XVEC` 场景下观察到的：

- `Build index with posting page limit:15`
- `Load index with posting page limit:15`

形成了直接对照。

说明：

- 最小页数约束逻辑本身没有变；
- 变化的是数据制式；
- 当数据改为 `UInt8 + DEFAULT` 后，`vectorInfoSize` 变小，因此 `PostingPageLimit=3` 被 clamp 后得到的是 `4`，而不是之前的 `15`。

### 6.5 baseline 搜索阶段细粒度 I/O 结果

首次 strict baseline 结果：

- `QPS = 2451.580`
- `Recall@10 = 0.978319`
- `avg_latency_ms = 0.816`
- `p95_latency_ms = 0.950`
- `p99_latency_ms = 0.997`
- `avg_read_bandwidth_mbs = 898.318`

对应报告目录：

- `results/io_analysis/sift1m_official_u8default_20260430/baseline_st2_nt40_ir64_pl4`

根据 `report.md`，结论为：

- **读取放大显著，且与延迟相关**

### 6.6 清缓存重跑结果

又额外执行了一次显式清缓存后的 search 阶段重跑，运行时日志确认：

- `System page cache cleared.`

重跑后的核心指标为：

- `QPS = 2423.070`
- `Recall@10 = 0.978319`
- `avg_latency_ms = 0.825`
- `p95_latency_ms = 0.961`
- `p99_latency_ms = 1.015`
- `avg_read_bandwidth_mbs = 882.663`

对应目录：

- `results/io_analysis/sift1m_official_u8default_20260430/baseline_st2_nt40_ir64_pl4_rerun`

与首次 strict baseline 相比：

- QPS 与延迟非常接近
- Recall 完全一致
- 结论保持不变

这说明当前 strict `UInt8 + DEFAULT` baseline 在冷缓存口径下具备较好的稳定性。

## 7. 推荐后续实验矩阵

基于当前结果，后续最有价值的实验不再是继续混合修改很多参数，而是围绕 **strict `UInt8 + DEFAULT` baseline** 做受控单变量 sweep。

### 7.1 第一优先级：搜索并发 sweep

目的：区分“单查询 I/O 放大”与“多查询并发隐藏 I/O 延迟”的边界。

建议矩阵：

| 变量 | 建议取值 | 关注指标 |
| --- | --- | --- |
| `SearchThreadNum` | `1, 2, 4, 8` | QPS, avg/p95 latency, read bandwidth, queue depth |
| `NumberOfThreads` | `16, 24, 40, 64` | QPS, CPU 利用率, iowait, queue depth |

判断重点：

- QPS 是否继续随并发增加而提升
- latency 是否在某个并发点后明显恶化
- 磁盘队列深度与读带宽是否接近平台上限

### 7.2 第二优先级：候选规模与页数 sweep

目的：识别“读取放大”主要来自候选 fanout，还是来自单 posting 的 page 粒度开销。

建议矩阵：

| 变量 | 建议取值 | 关注指标 |
| --- | --- | --- |
| `InternalResultNum` | `16, 32, 64, 96` | Recall@10, QPS, requested_read_bytes |
| `SearchPostingPageLimit` | `2, 4, 8` | Recall@10, latency, requested_read_bytes |

判断重点：

- Recall 提升是否值得额外读放大成本
- `requested_read_bytes` 与 latency 的相关性是否继续保持高位
- 较小 page limit 是否会伤害 Recall，还是只是减少无效读取

### 7.3 推荐执行顺序

建议按下面顺序推进：

1. strict `UInt8 + DEFAULT` 的 `SearchThreadNum` sweep
2. strict `UInt8 + DEFAULT` 的 `InternalResultNum` sweep
3. strict `UInt8 + DEFAULT` 的 `SearchPostingPageLimit` sweep

这样能先把官方数据制式下的最优运行区间找出来。

---

## 8. strict `UInt8 + DEFAULT` sweep 总结

基于 strict baseline，已在以下目录完成一组新的 sweep：

- `results/io_analysis/sift1m_official_u8default_20260430/sweeps_20260430_strict_v2`

本轮 sweep 覆盖：

- `SearchThreadNum` sweep
- `NumberOfThreads` sweep
- `InternalResultNum / SearchPostingPageLimit` sweep
- 以及额外验证点：`st20_nt20_ir32_pl4`

### 8.1 `SearchThreadNum` sweep 结论

固定参数：

- `NumberOfThreads=40`
- `InternalResultNum=64`
- `SearchPostingPageLimit=4`

关键结果：

| run | QPS | Recall@10 | avg latency |
| --- | --- | --- | --- |
| `st1_nt40_ir64_pl4` | `1271.13` | `0.978319` | `0.787 ms` |
| `st2_nt40_ir64_pl4` | `2417.21` | `0.978319` | `0.827 ms` |
| `st4_nt40_ir64_pl4` | `4746.08` | `0.978319` | `0.842 ms` |
| `st8_nt40_ir64_pl4` | `5959.48` | `0.978319` | `1.341 ms` |

结论：

- `SearchThreadNum` 提升会显著拉高整体吞吐；
- `st=4` 是一个较平衡的点：吞吐远高于 baseline，但延迟只小幅上升；
- `st=8` 吞吐最高，但尾延迟和平均延迟都明显恶化。

### 8.2 `NumberOfThreads` sweep 结论

固定参数：

- `SearchThreadNum=2`
- `InternalResultNum=64`
- `SearchPostingPageLimit=4`

关键结果：

| run | QPS | Recall@10 | avg latency |
| --- | --- | --- | --- |
| `st2_nt16_ir64_pl4` | `2452.18` | `0.978319` | `0.815 ms` |
| `st2_nt24_ir64_pl4` | `2453.99` | `0.978319` | `0.815 ms` |
| `st2_nt40_ir64_pl4` | `2417.21` | `0.978319` | `0.827 ms` |
| `st2_nt64_ir64_pl4` | `2451.58` | `0.978319` | `0.816 ms` |

结论：

- 在当前 strict baseline 下，`NumberOfThreads` 不是主要瓶颈；
- `16~64` 的变化对 QPS 和 latency 几乎没有本质影响；
- 继续增加 `NumberOfThreads` 的收益非常有限。

### 8.3 `InternalResultNum / SearchPostingPageLimit` sweep 结论

固定参数：

- `SearchThreadNum=2`
- `NumberOfThreads=40`

关键结果：

| run | QPS | Recall@10 | avg latency | avg requested read bytes |
| --- | --- | --- | --- | --- |
| `st2_nt40_ir32_pl4` | `3373.82` | `0.939825` | `0.593 ms` | `244270.285` |
| `st2_nt40_ir64_pl4` | `2417.21` | `0.978319` | `0.827 ms` | `486076.826` |
| `st2_nt40_ir96_pl4` | `1941.75` | `0.989219` | `1.030 ms` | `727320.166` |
| `st2_nt40_ir128_pl4` | `1607.20` | `0.993960` | `1.244 ms` | `968433.254` |
| `st2_nt40_ir64_pl8` | `2417.79` | `0.978319` | `0.827 ms` | `486076.826` |
| `st2_nt40_ir96_pl8` | `1937.23` | `0.989219` | `1.032 ms` | `727320.166` |

结论：

- `InternalResultNum` 是这组里最敏感的主变量；
- 当 `InternalResultNum` 增大时：
  - Recall 持续提升；
  - QPS 明显下降；
  - latency 增大；
  - 读放大显著增加；
- `SearchPostingPageLimit` 从 `4` 提到 `8` 基本没有带来可见收益，当前 strict baseline 下它不是主导变量。

### 8.4 额外验证点：`st20_nt20_ir32_pl4`

又额外测试了一组高并发配置：

- `SearchThreadNum=20`
- `NumberOfThreads=20`
- `InternalResultNum=32`
- `SearchPostingPageLimit=4`

结果：

- `QPS = 11655.01`
- `Recall@10 = 0.939825`
- `avg_latency_ms = 1.704`
- `p95_latency_ms = 2.088`
- `avg_read_bandwidth_mbs = 1579.238`

与 `st2_nt40_ir32_pl4` 相比：

- 吞吐进一步大幅上升；
- Recall 保持不变；
- 但单查询延迟明显恶化；
- 报告结论也从“读取放大显著，且与延迟相关”变成了“存在混合瓶颈，I/O 不是唯一主导”。

这说明：

- 当并发进一步拉高后，系统已经不再只是单纯受 query 级读放大约束；
- CPU 调度、并行开销、以及更高层次的混合资源竞争也开始进入主导区间。

### 8.5 当前最有代表性的参数点

如果以当前 sweep 结果作为阶段性结论：

- **更高吞吐且保持较稳延迟**：`st4_nt40_ir64_pl4`
- **更低延迟**：`st2_nt40_ir32_pl4`
- **更高 Recall**：`st2_nt40_ir96_pl4` 或 `st2_nt40_ir128_pl4`
- **极限吞吐探索点**：`st20_nt20_ir32_pl4`

### 8.6 sweep 全量结果表

为便于后续直接查阅，当前已完成的所有 strict sweep run 汇总如下：

| run | QPS | Recall@10 | avg latency | p95 latency | read BW | dup ratio | req bytes | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `st1_nt40_ir64_pl4` | `1271.13` | `0.978319` | `0.787 ms` | `0.898 ms` | `525.110 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st2_nt16_ir64_pl4` | `2452.18` | `0.978319` | `0.815 ms` | `0.949 ms` | `996.311 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st2_nt24_ir64_pl4` | `2453.99` | `0.978319` | `0.815 ms` | `0.950 ms` | `971.371 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st2_nt40_ir32_pl4` | `3373.82` | `0.939825` | `0.593 ms` | `0.679 ms` | `567.720 MB/s` | `0.114702` | `244270.285` | 读取放大显著，且与延迟相关 |
| `st2_nt40_ir64_pl4` | `2417.21` | `0.978319` | `0.827 ms` | `0.966 ms` | `902.175 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st2_nt40_ir64_pl8` | `2417.79` | `0.978319` | `0.827 ms` | `0.964 ms` | `883.529 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st2_nt40_ir96_pl4` | `1941.75` | `0.989219` | `1.030 ms` | `1.214 ms` | `1124.239 MB/s` | `0.194313` | `727320.166` | 读取放大显著，且与延迟相关 |
| `st2_nt40_ir96_pl8` | `1937.23` | `0.989219` | `1.032 ms` | `1.218 ms` | `1088.646 MB/s` | `0.194313` | `727320.166` | 读取放大显著，且与延迟相关 |
| `st2_nt40_ir128_pl4` | `1607.20` | `0.993960` | `1.244 ms` | `1.481 ms` | `1283.921 MB/s` | `0.218413` | `968433.254` | 读取放大显著，且与延迟相关 |
| `st2_nt64_ir64_pl4` | `2451.58` | `0.978319` | `0.816 ms` | `0.955 ms` | `818.333 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st4_nt40_ir64_pl4` | `4746.08` | `0.978319` | `0.842 ms` | `0.998 ms` | `1458.077 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st8_nt16_ir64_pl4` | `5945.30` | `0.978319` | `1.344 ms` | `1.604 ms` | `2031.634 MB/s` | `0.162538` | `486076.826` | 存在混合瓶颈，I/O 不是唯一主导 |
| `st8_nt20_ir64_pl4` | `5931.20` | `0.978319` | `1.347 ms` | `1.611 ms` | `2028.975 MB/s` | `0.162538` | `486076.826` | 存在混合瓶颈，I/O 不是唯一主导 |
| `st8_nt40_ir64_pl4` | `5959.48` | `0.978319` | `1.341 ms` | `1.597 ms` | `1612.909 MB/s` | `0.162538` | `486076.826` | 读取放大显著，且与延迟相关 |
| `st16_nt16_ir64_pl4` | `5920.66` | `0.978319` | `2.695 ms` | `3.249 ms` | `2123.462 MB/s` | `0.162538` | `486076.826` | 存在混合瓶颈，I/O 不是唯一主导 |
| `st16_nt20_ir64_pl4` | `5903.19` | `0.978319` | `2.703 ms` | `3.252 ms` | `1949.263 MB/s` | `0.162538` | `486076.826` | 存在混合瓶颈，I/O 不是唯一主导 |
| `st20_nt20_ir32_pl4` | `11655.01` | `0.939825` | `1.704 ms` | `2.088 ms` | `1579.238 MB/s` | `0.114702` | `244270.285` | 存在混合瓶颈，I/O 不是唯一主导 |
| `st20_nt20_ir64_pl4` | `5885.82` | `0.978319` | `3.387 ms` | `4.357 ms` | `1941.911 MB/s` | `0.162538` | `486076.826` | 存在混合瓶颈，I/O 不是唯一主导 |

### 8.7 高并发区间深入分析

基于新增的 `st8/st16` 与 `nt16/nt20` 组合测试，可以进一步确认：

#### 8.7.1 `st=8` 是当前配置下的最优并发点

| run | SearchThreadNum | NumberOfThreads | QPS | avg latency | read BW |
| --- | --- | --- | --- | --- | --- |
| `st8_nt16_ir64_pl4` | 8 | 16 | `5945.30` | `1.344 ms` | `2031.634 MB/s` |
| `st8_nt20_ir64_pl4` | 8 | 20 | `5931.20` | `1.347 ms` | `2028.975 MB/s` |
| `st8_nt40_ir64_pl4` | 8 | 40 | `5959.48` | `1.341 ms` | `1612.909 MB/s` |

**结论**：
- 当 `SearchThreadNum=8` 时，`NumberOfThreads` 在 `16~40` 区间内对 QPS 和延迟几乎没有影响；
- QPS 稳定在约 `5.9k`，延迟稳定在约 `1.34 ms`；
- `st=8` 是当前 `ir64` 配置下的最佳并发点。

#### 8.7.2 `st=16` 延迟翻倍但 QPS 不增

| run | SearchThreadNum | avg latency | QPS |
| --- | --- | --- | --- |
| `st8_nt16_ir64_pl4` | 8 | `1.344 ms` | `5945.30` |
| `st16_nt16_ir64_pl4` | 16 | `2.695 ms` | `5920.66` |

**结论**：
- 并发从 8 提到 16，QPS 几乎不变，但延迟翻倍；
- 说明 `st=8` 已接近当前配置下的系统饱和点；
- 继续增加并发只会增加调度开销和资源竞争，不会带来吞吐提升。

#### 8.7.3 `NumberOfThreads` 减少反而让 read BW 上升

| run | NumberOfThreads | read BW |
| --- | --- | --- |
| `st8_nt40_ir64_pl4` | 40 | `1612.909 MB/s` |
| `st8_nt20_ir64_pl4` | 20 | `2028.975 MB/s` |
| `st8_nt16_ir64_pl4` | 16 | `2031.634 MB/s` |

**结论**：
- 减少 `NumberOfThreads` 反而让 read BW 上升约 25%；
- 可能原因：更少的后台线程减少了 I/O 竞争，使测量窗口内的 I/O 更集中；
- 但 QPS 和延迟并未因此改善，说明 read BW 的增加只是测量效应，不是有效吞吐提升。

#### 8.7.4 高并发极端点的瓶颈画像变化

从 `st=8` 开始，报告结论从"读取放大显著，且与延迟相关"转变为"存在混合瓶颈，I/O 不是唯一主导"：

| run | SearchThreadNum | 结论 |
| --- | --- | --- |
| `st4_nt40_ir64_pl4` | 4 | 读取放大显著，且与延迟相关 |
| `st8_nt40_ir64_pl4` | 8 | 读取放大显著，且与延迟相关 |
| `st8_nt16_ir64_pl4` | 8 | 存在混合瓶颈，I/O 不是唯一主导 |
| `st16_nt16_ir64_pl4` | 16 | 存在混合瓶颈，I/O 不是唯一主导 |
| `st20_nt20_ir64_pl4` | 20 | 存在混合瓶颈，I/O 不是唯一主导 |

**结论**：
- 当并发超过一定阈值（约 8）后，I/O 不再是唯一瓶颈；
- CPU 调度、锁竞争、内存带宽等开始参与限制；
- 这与 QPS 不再随并发提升、延迟却持续恶化的观察一致。

### 8.8 参数与性能指标关系分析

基于表中所有 sweep 数据，系统分析 **st（SearchThreadNum）、nt（NumberOfThreads）、ir（InternalResultNum）** 与各项性能指标之间的关系。

#### 8.8.1 st（SearchThreadNum）对各指标的影响

固定 nt=40, ir=64, pl=4 时的 st 变化：

| st | QPS | avg latency | p95 latency | read BW | req bytes |
| --- | --- | --- | --- | --- | --- |
| 1 | `1271.13` | `0.787 ms` | `0.898 ms` | `525.110 MB/s` | `486076.826` |
| 2 | `2417.21` | `0.827 ms` | `0.966 ms` | `902.175 MB/s` | `486076.826` |
| 4 | `4746.08` | `0.842 ms` | `0.998 ms` | `1458.077 MB/s` | `486076.826` |
| 8 | `5959.48` | `1.341 ms` | `1.597 ms` | `1612.909 MB/s` | `486076.826` |

关系总结：

| 指标 | 与 st 的关系 |
| --- | --- |
| **QPS** | 近似线性增长直到 st=4，之后边际收益递减；st=8 → st=1 提升约 4.7 倍 |
| **avg latency** | st=1~4 时基本稳定（~0.8 ms），st=8 时明显恶化（1.34 ms） |
| **p95 latency** | 随 st 增加缓慢上升，st=8 时显著恶化 |
| **read BW** | 随 st 近似线性增长，st=8 时接近设备上限 |
| **dup ratio** | 不受 st 影响，保持 `0.162538` |
| **req bytes** | 不受 st 影响，保持 `486076.826` |

**结论**：st 主要影响 QPS 和 read BW，但对单查询 I/O 量（req bytes）和重复读比例（dup ratio）无影响。延迟在 st≤4 时稳定，st>4 后恶化。

#### 8.8.2 nt（NumberOfThreads）对各指标的影响

固定 st=2, ir=64, pl=4 时的 nt 变化：

| nt | QPS | avg latency | p95 latency | read BW | req bytes |
| --- | --- | --- | --- | --- | --- |
| 16 | `2452.18` | `0.815 ms` | `0.949 ms` | `996.311 MB/s` | `486076.826` |
| 24 | `2453.99` | `0.815 ms` | `0.950 ms` | `971.371 MB/s` | `486076.826` |
| 40 | `2417.21` | `0.827 ms` | `0.966 ms` | `902.175 MB/s` | `486076.826` |
| 64 | `2451.58` | `0.816 ms` | `0.955 ms` | `818.333 MB/s` | `486076.826` |

关系总结：

| 指标 | 与 nt 的关系 |
| --- | --- |
| **QPS** | 几乎不受 nt 影响，波动范围 < 2% |
| **avg latency** | 几乎不受 nt 影响 |
| **p95 latency** | 几乎不受 nt 影响 |
| **read BW** | 反而有轻微下降趋势（nt↑ → read BW↓） |
| **dup ratio** | 不受 nt 影响 |
| **req bytes** | 不受 nt 影响 |

固定 st=8, ir=64, pl=4 时的 nt 变化：

| nt | QPS | avg latency | read BW |
| --- | --- | --- | --- |
| 16 | `5945.30` | `1.344 ms` | `2031.634 MB/s` |
| 20 | `5931.20` | `1.347 ms` | `2028.975 MB/s` |
| 40 | `5959.48` | `1.341 ms` | `1612.909 MB/s` |

**结论**：nt 在当前测试区间（16~64）内对各指标影响极小。甚至出现 nt↓ → read BW↑ 的反直觉现象，说明 nt 不是当前配置下的瓶颈变量。

#### 8.8.3 ir（InternalResultNum）对各指标的影响

固定 st=2, nt=40, pl=4 时的 ir 变化：

| ir | QPS | Recall@10 | avg latency | p95 latency | read BW | dup ratio | req bytes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 32 | `3373.82` | `0.939825` | `0.593 ms` | `0.679 ms` | `567.720 MB/s` | `0.114702` | `244270.285` |
| 64 | `2417.21` | `0.978319` | `0.827 ms` | `0.966 ms` | `902.175 MB/s` | `0.162538` | `486076.826` |
| 96 | `1941.75` | `0.989219` | `1.030 ms` | `1.214 ms` | `1124.239 MB/s` | `0.194313` | `727320.166` |
| 128 | `1607.20` | `0.993960` | `1.244 ms` | `1.481 ms` | `1283.921 MB/s` | `0.218413` | `968433.254` |

关系总结：

| 指标 | 与 ir 的关系 |
| --- | --- |
| **QPS** | ir↑ → QPS↓，近似反比关系；ir 翻倍 → QPS 约减半 |
| **Recall@10** | ir↑ → Recall↑，边际收益递减；ir=64→128 时 Recall 仅从 0.978 → 0.994 |
| **avg latency** | ir↑ → latency↑，近似线性增长 |
| **p95 latency** | ir↑ → p95↑ |
| **read BW** | ir↑ → read BW↑，但不是线性（因为 QPS 在下降） |
| **dup ratio** | ir↑ → dup ratio↑，从 0.115 → 0.218 |
| **req bytes** | ir↑ → req bytes↑，近似线性；ir 翻倍 → req bytes 翻倍 |

关键发现：

- **ir 是唯一同时影响 Recall 和性能的变量**
- ir=32：最低 Recall（0.940），最高 QPS（3374），最低延迟（0.59 ms）
- ir=128：最高 Recall（0.994），最低 QPS（1607），最高延迟（1.24 ms）
- **Recall-QPS tradeoff**：ir 从 32 → 128，Recall 提升 5.8%，QPS 下降 52%

#### 8.8.4 pl（SearchPostingPageLimit）对各指标的影响

固定 st=2, nt=40 时的 pl 对比：

| ir | pl | QPS | Recall@10 | avg latency | read BW | req bytes |
| --- | --- | --- | --- | --- | --- | --- |
| 64 | 4 | `2417.21` | `0.978319` | `0.827 ms` | `902.175 MB/s` | `486076.826` |
| 64 | 8 | `2417.79` | `0.978319` | `0.827 ms` | `883.529 MB/s` | `486076.826` |
| 96 | 4 | `1941.75` | `0.989219` | `1.030 ms` | `1124.239 MB/s` | `727320.166` |
| 96 | 8 | `1937.23` | `0.989219` | `1.032 ms` | `1088.646 MB/s` | `727320.166` |

**结论**：pl 从 4 → 8 对所有指标均无显著影响。这与当前 strict UInt8+DEFAULT 场景下 posting page 已经较紧凑有关。

#### 8.8.5 参数与指标关系总结

| 参数 | QPS | Recall | avg latency | p95 latency | read BW | dup ratio | req bytes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **st↑** | ↑↑（到饱和） | — | —（st≤4）→ ↑（st>4） | ↑ | ↑↑ | — | — |
| **nt↑** | — | — | — | — | ↓（轻微） | — | — |
| **ir↑** | ↓↓ | ↑↑ | ↑↑ | ↑↑ | ↑ | ↑↑ | ↑↑ |
| **pl↑** | — | — | — | — | — | — | — |

图例：
- `↑↑`：显著正相关
- `↓↓`：显著负相关
- `↑`：正相关
- `↓`：负相关
- `—`：无明显影响

#### 8.8.6 实践建议

1. **调整 QPS**：优先调整 st，st=4~8 是高效区间
2. **调整 Recall**：唯一有效旋钮是 ir，ir=64~96 是平衡区间
3. **调整延迟**：降低 ir 可显著降低延迟；st 过高会恶化延迟
4. **nt 和 pl**：当前场景下不是主要影响因素，可保持默认

### 8.9 SPANN SIFT1M 在 NVMe 上的瓶颈深度分析

基于测试数据和代码分析，详细剖析 SPANN SIFT1M 数据集在 NVMe 硬盘上的瓶颈演化和具体构成。

#### 8.9.1 瓶颈演化的三个阶段

根据测试数据，瓶颈随并发增加呈现明显的三阶段演化：

**阶段一：低并发（st=1~2）—— I/O 是绝对瓶颈**

| run | st | QPS | read BW | avg latency | 特征 |
| --- | --- | --- | --- | --- | --- |
| `st1_nt40_ir64_pl4` | 1 | `1271` | `525 MB/s` | `0.787 ms` | QPS∝st，延迟稳定 |
| `st2_nt40_ir64_pl4` | 2 | `2417` | `902 MB/s` | `0.827 ms` | NVMe 远未饱和 |

**瓶颈本质**：单线程/低并发下，I/O 请求无法充分利用 NVMe 的并行能力，属于 **I/O 请求级并行不足**。

**阶段二：中并发（st=4~8）—— I/O 接近饱和，边际收益递减**

| run | st | QPS | read BW | avg latency | 特征 |
| --- | --- | --- | --- | --- | --- |
| `st4_nt40_ir64_pl4` | 4 | `4746` | `1458 MB/s` | `0.842 ms` | QPS 增速放缓 |
| `st8_nt40_ir64_pl4` | 8 | `5959` | `1613 MB/s` | `1.341 ms` | 延迟开始恶化 |

**瓶颈本质**：I/O 带宽接近 NVMe 实际可达上限，进入 **I/O 带宽饱和区**。继续增加并发，QPS 提升有限但延迟恶化明显。

**阶段三：高并发（st≥8，尤其是 st≥16）—— 混合瓶颈**

| run | st | QPS | avg latency | read BW | 特征 |
| --- | --- | --- | --- | --- | --- |
| `st8_nt16_ir64_pl4` | 8 | `5945` | `1.344 ms` | `2032 MB/s` | 报告结论变化 |
| `st16_nt16_ir64_pl4` | 16 | `5921` | `2.695 ms` | `2123 MB/s` | QPS 不增，延迟翻倍 |
| `st20_nt20_ir64_pl4` | 20 | `5886` | `3.387 ms` | `1942 MB/s` | 资源竞争白热化 |

**瓶颈本质**：系统进入 **混合瓶颈区**，I/O 不再是唯一主导因素。

#### 8.9.2 高并发混合瓶颈的具体构成

**1. CPU 调度开销**

| st | QPS | avg latency | 延迟变化 |
| --- | --- | --- | --- |
| 8 | `5945` | `1.344 ms` | 基准 |
| 16 | `5921` | `2.695 ms` | +100% |
| 20 | `5886` | `3.387 ms` | +152% |

**延迟增加但吞吐不变**，这是典型的 **CPU 调度竞争** 特征：
- 更多的线程竞争 CPU 时间片
- 上下文切换开销增加
- 线程等待时间变长

**2. 锁竞争**

SPANN 的搜索路径涉及多处共享资源：
- **WorkSpace 池管理**：多个搜索线程从共享池获取 WorkSpace
- **I/O 请求队列**：`BatchReadFileAsync` 中的请求提交和完成回调
- **结果合并**：多线程搜索结果需要合并排序

当并发超过一定阈值后，这些共享资源的锁竞争会成为瓶颈。

**3. 内存带宽**

以 `st8_nt40_ir64_pl4` 为例：
- 每查询平均请求读字节：~486 KB
- 理论内存带宽需求：`5945 × 486 KB ≈ 2.9 GB/s`

再加上：
- 解压缩（DecompressPosting）
- 距离计算（L2/余弦）
- 结果排序

实际内存带宽需求可能达到 **5-10 GB/s**，在某些平台上可能成为瓶颈。

**4. I/O 请求级放大（重复向量读取）**

| ir | dup ratio | 说明 |
| --- | --- | --- |
| 32 | `0.115` | 11.5% 重复读 |
| 64 | `0.163` | 16.3% 重复读 |
| 96 | `0.194` | 19.4% 重复读 |
| 128 | `0.218` | 21.8% 重复读 |

**重复向量读取比例** 说明：
- 同一向量可能被多个 posting list 引用
- 即使 posting page 已经紧凑，仍存在约 16% 的重复读
- 这是 SPANN 架构层面的固有放大

#### 8.9.3 为什么 NVMe 的高带宽没有完全转化为 QPS？

**理论计算**：

以 `st8_nt40_ir64_pl4` 为例：
- QPS = 5959
- req bytes = 486,077
- 理论 read BW = `5959 × 486077 / 1024² ≈ 2764 MB/s`
- 实际 read BW = 1613 MB/s

**差距约 42%**，说明存在显著的 I/O 效率损失。

**损失来源分析**：

1. **Batch I/O 的同步等待开销**

SPANN 的 `BatchReadFileAsync` 流程：
- 提交所有 I/O 请求（`io_submit`）
- 等待所有完成（`io_getevents`）
- 回调处理：解压、计算距离（同步执行）

问题：
- 所有请求提交后，等待最慢的一个完成
- 回调处理在 `io_getevents` 后同步执行
- 高并发时，单个慢请求会阻塞整批请求

2. **Page 对齐导致的额外读取**

Posting list 以 4KB page 为单位存储，但单个 posting 可能只使用部分 page：
- 实际需要的向量数据可能只占 page 的 70-80%
- 其余是 padding 或未使用空间
- 这部分"无效读取"也计入 read BW

3. **重复向量读取**

dup ratio = 16.3% 意味着约 16% 的读是重复的，这部分带宽被浪费。

**NVMe 并行能力未充分利用**：

| st | read BW | NVMe 利用率（相对 3000 MB/s） |
| --- | --- | --- |
| 1 | `525 MB/s` | ~17% |
| 8 | `1613 MB/s` | ~54% |
| 16 | `2123 MB/s` | ~71% |

即使 st=16，也只达到 NVMe 理论带宽的 **~70%**。

原因：
1. **I/O 队列深度不足**：SPANN 的 batch read 每批请求数有限
2. **同步回调阻塞**：解压和计算在 I/O 完成后同步执行，阻塞了下一批 I/O 的提交
3. **请求粒度不均**：部分 posting 很大，部分很小，难以形成均匀的 I/O 负载

#### 8.9.4 ir（InternalResultNum）对瓶颈的影响

| ir | QPS | Recall | req bytes | read BW | dup ratio | 边际 Recall 收益 |
| --- | --- | --- | --- | --- | --- | --- |
| 32 | `3374` | 0.940 | 244 KB | 568 MB/s | 0.115 | — |
| 64 | `2417` | 0.978 | 486 KB | 902 MB/s | 0.163 | +3.9% |
| 96 | `1942` | 0.989 | 727 KB | 1124 MB/s | 0.194 | +1.1% |
| 128 | `1607` | 0.994 | 968 KB | 1284 MB/s | 0.218 | +0.5% |

**分析**：

1. **ir 增大 → 单查询 I/O 量增大 → QPS 下降**：直接因果关系
2. **ir 增大 → dup ratio 增大**：更多候选 → 更多 posting list → 向量重叠概率增加
3. **ir 增大 → 边际 Recall 收益递减**：ir>96 后，额外 I/O 成本换来的 Recall 提升很小

**结论**：ir=64~96 是 Recall 与性能的较平衡区间。

#### 8.9.5 nt（NumberOfThreads）为何影响很小？

| nt | QPS | read BW | 变化 |
| --- | --- | --- | --- |
| 16 | `2452` | `996 MB/s` | 基准 |
| 24 | `2454` | `971 MB/s` | -2.5% |
| 40 | `2417` | `902 MB/s` | -9.4% |
| 64 | `2452` | `818 MB/s` | -17.9% |

**原因分析**：

1. **nt 主要影响后台 worker 池大小**：用于 posting 处理、解压缩等并行任务
2. **当前测试中后台计算不是瓶颈**：I/O 和调度才是主要瓶颈
3. **后台 worker 过多反而增加调度开销**

**建议**：当 `st ≤ 8` 时，`nt=16~24` 已足够，`nt > st × 2` 的配置收益很小。

#### 8.9.6 瓶颈演化总结图

```
并发级别        瓶颈类型              特征
────────────────────────────────────────────────────
st=1~2         I/O 请求级并行不足    QPS∝st，延迟稳定
                                      NVMe 带宽远未饱和

st=4~8         I/O 带宽饱和区        QPS 增速放缓
                                      延迟开始恶化
                                      read BW 接近实际上限

st≥8~16        混合瓶颈区            QPS 不再提升
              - CPU 调度竞争         延迟显著恶化
              - 锁竞争               read BW 波动
              - 内存带宽             报告结论变化
              - I/O 请求级放大

st≥16          纯粹调度开销区        QPS 下降
                                      延迟翻倍
                                      资源竞争白热化
```

#### 8.9.7 优化建议

**并发配置**：
- st=4~8 是最优区间，st>8 无益
- nt=16~24 已足够，过多反而有害

**Recall-性能权衡**：
- ir=64~96 是平衡点，ir>96 边际收益很小

**I/O 效率优化方向**：
1. 增大 batch size，提高单次 I/O 请求的并行度
2. 异步化解压和计算，避免阻塞 I/O 提交
3. 预取/缓存热门 posting list

**架构层面**：
1. 减少重复向量读取（优化 posting list 组织方式）
2. 压缩 posting list 减少 I/O 量
3. 考虑内存缓存层

### 8.10 SIFT10M 优化线程配置细粒度 I/O 测试补充

在完成 SIFT1M strict `UInt8 + DEFAULT` 分析之后，又对 `/home/ray/data/sift10m` 数据集做了同口径补充测试：

- 先将 `bigann_base_10m.bvecs` 转换为 `UInt8 + DEFAULT` 格式：`bigann10m_base.u8bin`
- 将 `bigann_query.bvecs` 转换为 `UInt8 + DEFAULT` 格式：`query.public.10K.u8bin`
- 将 `idx_10M.ivecs` 转换为 SPTAG 可直接读取的 `TruthType=DEFAULT` 格式：`bigann-10M.bin`

对应测试配置采用本机线程优化版本：

- `SearchThreadNum=8`
- `NumberOfThreads=16`
- `InternalResultNum=64`
- `SearchPostingPageLimit=4`

结果目录：

- `results/io_analysis/sift10m_official_u8default_20260430/baseline_st8_nt16_ir64_pl4`

#### 8.10.1 Build 结果

优化线程配置下，SIFT10M build 阶段完成情况如下：

| 阶段 | 时间 |
| --- | --- |
| `SelectHead` | `1091 s` |
| `BuildHead` | `719 s` |
| `BuildSSDIndex` | `290 s` |
| **总计** | **约 35 分钟** |

索引统计：

- head 数：`1,496,408`（约 `14.96%`）
- 总页数：`2,061,442`
- 索引大小：约 `8.4 GB`
- 总元素数：`63,692,544`

#### 8.10.2 冷缓存细粒度 I/O 结果

在显式清缓存后，使用 `run_io_analysis.sh` 生成完整结构化结果文件：

- `config.ini`
- `cpu_stats.csv`
- `disk_stats.csv`
- `process_io_stats.csv`
- `psi_io_stats.csv`
- `query_io_stats.csv`
- `report.md`
- `summary.txt`
- `sptag.log`
- `SIFT10M_IO_Analysis_Report.md`

核心指标：

- `QPS = 5608.52`
- `Recall@10 = 0.949144`
- `avg_latency_ms = 1.425`
- `p95_latency_ms = 1.655`
- `p99_latency_ms = 1.821`
- `avg_read_bandwidth_mbs = 1608.594`
- `avg_requested_read_bytes = 515772.006`
- `avg_duplicate_vector_read_ratio = 0.119526`
- `avg_distance_eval_ratio = 0.880474`
- `avg_final_result_ratio = 0.003625`

系统级摘要：

- `process_read_bytes_delta ≈ 6.79 GB`
- `avg_queue_depth ≈ 102.9`
- `peak_queue_depth = 278`
- `cpu_iowait_percent ≈ 0.41%`

#### 8.10.3 更保守、更准确的结论

这轮 SIFT10M 结果可以支持以下结论：

1. **主要成本集中在 Ex / Batch Read + posting 处理路径**
   - `Head Latency ≈ 0.286 ms`
   - `Ex Latency ≈ 1.136 ms`
   - Ex 阶段约占总延迟 80%

2. **存在明显的 posting 读放大与扫描放大**
   - 单 query 平均读取约 `516 KB`
   - 单 query 平均访问约 `125.9` 个 page
   - 单 query 平均扫描约 `2955.6` 个 posting elements
   - 但最终只返回 Top-10，`final_result_ratio ≈ 0.0036`

3. **当前还不能严格拆分纯 I/O 与纯 CPU 占比**
   - 当前走的是 `BATCH_READ` 路径
   - `Batch Read Total` 混合了 I/O wait、callback、posting parse 和 distance calc
   - 因此不能仅凭当前统计直接断言“纯 I/O 已到硬件上限”或“CPU 调度/锁竞争已成为主瓶颈”

4. **与 SIFT1M 相比，单 query 成本增长较小，但 Recall 有所下降**
   - SIFT1M `st=8` 时：
     - `QPS ≈ 5945`
     - `Recall@10 ≈ 0.978`
     - `avg_requested_read_bytes ≈ 486 KB`
   - SIFT10M `st=8` 时：
     - `QPS ≈ 5609`
     - `Recall@10 ≈ 0.949`
     - `avg_requested_read_bytes ≈ 516 KB`

这说明：

- 在 SIFT 同分布、head 数同比扩展的条件下，单 query posting 成本保持相对稳定；
- 但数据规模从 1M 增加到 10M 后，Recall 下降仍然需要纳入质量-性能权衡。

#### 8.10.4 后续更合理的优化方向

相比直接下“CPU 调度/锁竞争已是瓶颈”的结论，更合理的下一步是：

1. 优先降低 `requested bytes/query`
2. 优先降低 `scanned elements/query`
3. 拆分 `BATCH_READ` 计时口径，区分：
   - I/O wait
   - callback
   - posting parse
   - distance calc
4. 对 SIFT10M 补做 sweep：
   - `SearchThreadNum = 4 / 8 / 12 / 16`
   - `NumberOfThreads = 16 / 24 / 40 / 64`
   - `InternalResultNum = 32 / 64 / 96`

---

## 9. 最终结论

本次工作已经完成了以下事项：

1. 制定并执行了“官方同名参数对齐 + 本机数据路径映射”的 SIFT1M 重建与 I/O 分析方案；
2. 更新了 SIFT1M 的规范入口配置与测试文档；
3. 重新构建了新的 official baseline 索引；
4. 完成了一次新的 baseline 细粒度 I/O 分析；
5. 发现并确认：
   - `PostingPageLimit=3` 在 `Float + XVEC` 场景下会被内部最小页数逻辑提升为 `15`；
   - 该逻辑与官方 `microsoft/SPTAG` 仓库一致，不是本地额外修改；
6. 在此基础上又完成了更严格的 `UInt8 + DEFAULT` 数据制式复现，并确认：
   - 在 strict 官方数据制式下，`PostingPageLimit=3` 会被 clamp 为 `4`；
   - strict baseline 的搜索性能约为 `2.4k QPS`，`Recall@10` 约为 `0.9783`；
   - 清缓存重跑后结果仍然稳定，结论仍为“读取放大显著，且与延迟相关”；
7. 进一步完成了一组 strict sweep，并确认：
   - `SearchThreadNum` 是推动吞吐提升的最显著变量；
   - `NumberOfThreads` 在当前区间内影响很小；
   - `InternalResultNum` 是 Recall / latency / 读放大的关键 tradeoff 旋钮；
   - 高并发极端点会把瓶颈画像推进到“混合瓶颈，I/O 不是唯一主导”。
