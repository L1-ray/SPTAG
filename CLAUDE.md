# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

SPTAG (Space Partition Tree And Graph) 是由 Microsoft Research 和 Microsoft Bing 开发的大规模向量近似最近邻 (ANN) 搜索 C++ 库。包含用于增量就地更新的 SPFresh 系统 (SOSP 2023)。

## 构建命令

### Linux
```bash
mkdir build
cd build && cmake -DSPDK=OFF -DROCKSDB=OFF .. && make
```
编译产物输出到 `Release/` 目录。

### Windows
```bash
mkdir build
cd build && cmake -A x64 -DSPDK=OFF -DROCKSDB=OFF ..
```
然后在 Visual Studio 2019+ 中编译 SPTAGLib.sln。

### 构建选项 (CMake)
- `GPU` - 启用 GPU 支持 (默认 OFF)
- `ROCKSDB` - 启用 RocksDB 存储后端 (OFF)
- `SPDK` - 启用 SPDK 存储后端 (OFF)
- `TBB` - 启用 Intel TBB (默认 ON)
- `URING` - 启用 liburing/io_uring 支持 (OFF)
- `USE_ASAN` - 启用 AddressSanitizer (OFF)

### 依赖要求
- CMake >= 3.12
- GCC >= 5.0 (Linux) 或 MSVC 14+ (Windows)
- Boost >= 1.67
- SWIG >= 4.0.2 (用于 Python 绑定)
- OpenMP (必需)

## 测试

测试框架: Boost.Test

```bash
# 运行所有测试
./Release/SPTAGTest

# 运行特定测试套件
./Release/SPTAGTest --run_test=TestSuiteName

# 详细输出模式
./Release/SPTAGTest --log_level=test_suite
```

测试文件位于 `Test/src/` (AlgoTest.cpp, DistanceTest.cpp, SIMDTest.cpp, SPFreshTest.cpp 等)

## 已知编译问题

### server 目标编译失败

`server` 可执行文件因 Boost 版本兼容问题编译失败：

```
AnnService/inc/Socket/Connection.h:14:10: 致命错误：boost/asio/io_service_strand.hpp：没有那个文件或目录
```

这是 Boost 版本兼容性问题（新版本 Boost 路径变更），与项目代码修改无关。

**解决方案**：
- 当前环境 Boost 版本较新，`io_service_strand.hpp` 路径已变更
- 如需编译 server，需要修改 `Connection.h` 中的 include 路径，或降级 Boost 版本
- 其他目标（`ssdserving`、`indexbuilder`、`indexsearcher`、`spfresh` 等）编译正常

## 代码格式化

使用 clang-format，基于 Microsoft 风格：
- 列宽限制: 120
- 缩进宽度: 4
- C++17 标准
- 自定义花括号换行 (函数、类、命名空间后换行)

```bash
clang-format -i <file>
```

## 架构

### 索引算法
- **BKT** (Balanced K-Means Tree) - 高维数据精度更高
- **KDT** (KD-Tree) - 构建成本更低
- **SPANN** - 面向十亿级向量的磁盘索引

### 核心组件 (`AnnService/inc/Core/`)
- `BKT/` - BKT 索引实现
- `KDT/` - KDT 索引实现
- `SPANN/` - SPANN 磁盘索引，用于十亿级向量
  - `PageCache.h/cpp` - M1 全局页面缓存实现
  - `ExtraStaticSearcher.h` - Legacy posting 搜索路径
  - `IExtraSearcher.h` - 搜索接口定义
  - `Options.h` - SPANN 配置参数
- `Common/` - 公共工具: SIMD (AVX/AVX2/AVX512)、距离计算 (L2, Cosine)、量化 (PQ/OPQ)

### 可执行程序 (`AnnService/src/`)
- `IndexBuilder/` - 构建内存索引
- `IndexSearcher/` - 搜索内存索引
- `Server/` - 基于 Socket 的搜索服务
- `Client/` - 远程搜索客户端
- `Aggregator/` - 分布式搜索聚合器
- `SSDServing/` - SSD 索引构建与搜索
- `SPFresh/` - 增量就地更新系统
- `Quantizer/` - 训练 PQ/OPQ 量化器

### 关键数据结构
- `VectorSet` - 向量存储
- `MetadataSet` - 向量关联的元数据
- `NeighborhoodGraph` - ANN 搜索的图结构
- `WorkSpace` - 搜索工作空间管理
- `GlobalPageCache` - M1 全局页面缓存

### 支持的距离度量
- L2 (欧氏距离)
- Cosine (余弦距离)

### 支持的向量类型
- Float, Int8, Int16, UInt8

### 文件格式
- DEFAULT (二进制): `<向量数><维度><原始数据>`
- TXT: `<元数据>\t<v1>|<v2>|...|`
- XVEC: .fvecs/.ivecs 格式

## Python 绑定

Python 封装通过 SWIG 构建。使用示例见 `docs/Tutorial.ipynb` 和 `docs/GettingStart.md`。

```python
import SPTAG
index = SPTAG.AnnIndex('BKT', 'Float', dimension)
index.SetBuildParam("DistCalcMethod", "L2", "Index")
index.Build(vectors, num_vectors, False)
index.Save('output_folder')
```

## 配置文件

索引构建和搜索使用 INI 格式配置文件。关键参数：
- `DistCalcMethod` - 距离度量 (L2 或 Cosine)
- `MaxCheck` - 每次查询访问的节点数 (影响召回率/延迟)
- `NumberOfThreads` - 构建时的线程数
- `NeighborhoodSize` - 图中每个节点的邻居数

完整参数文档见 `docs/Parameters.md`。

### M1 Page Cache 配置参数

在 `[SearchSSDIndex]` section 中配置：
- `EnablePageCache` - 是否启用页面缓存 (默认 false)
- `PageCacheMaxBytes` - 缓存最大字节数 (默认 256MB)

推荐配置：
```ini
[SearchSSDIndex]
EnablePageCache=true
PageCacheMaxBytes=268435456
```

**注意**: 当前实现只缓存单页 posting (32% 的 posting)，256MB 足够。如需缓存 1-2 页 posting，需要先实现分片锁架构。

## 项目文档与记忆文件

以下文档记录了 SPANN 优化工作的进展和状态：

### SPANN_Beyond_Official_Baseline_Plan_20260502.md
**历史规划文档**，定义了在官方 strict `UInt8 + DEFAULT` baseline 上超越官方的目标和路径。
- M1: Global I/O broker + page cache **(已完成: st8 +5%, st16 +13.6%)**
- M2-H: Hybrid selective code-first **(已证伪: I/O wait 分布均匀)**
- M4: Primary-secondary payload dedupe **(已证伪: 见下方结论)**

### SPANN_M4_Storage_Optimized_Profile_Plan_20260503.md
**M4 存储优化方案 - 已完成评估**
- 目标：存储优化（非 QPS 优化）
- **最终结论**：M4 仅作为 Storage-only Sidecar，不实现在线查询路径
- 存储节省 73.1% ✅，但在线查询 pages/query 增加 10x ❌
- 根因：Legacy Posting 结构提供 Spatial Locality，M4 Primary Store 打破这种 locality
- 推荐用途：归档存储、分发打包、冷数据存储

### SPANN_Query_Aware_Adaptive_Posting_Budget_Plan_20260503.md
**当前主线规划文档**，定义 Query-aware Adaptive Posting Budget 方案。
- 目标：在相同/近似 Recall 下减少不必要的 posting 读取
- 核心思想：Easy query 少读、Hard query 多读
- 执行顺序：Offline Oracle → Rule-based Policy → Learned Policy

### SIFT1M_Official_Alignment_Summary.md
官方参数对齐与 strict `UInt8 + DEFAULT` baseline 测试结果总结。
- 包含完整的 `SearchThreadNum`、`InternalResultNum`、`NumberOfThreads` sweep 结果
- 关键结论：st=4~8 是吞吐有效区间，ir=64~96 是 Recall/QPS 平衡区间

## spann_analysis/ 目录分析报告

以下三个分析文件提供了深入的性能洞察：

### Query_Level_Performance_Report.md
查询级性能特征分析。
- **关键发现**：SPANN 延迟增长亚线性 (10x 规模 → 11% 延迟增长)
- I/O 主导：Batch Read 占 63-68% 延迟
- ~99.6% 扫描元素被丢弃 (posting/page 粒度读放大)
- M1 Cache 规模敏感性：SIFT10M 失效因 cross-query reuse 不足
- 低 recall 查询的延迟并不高，问题出在 routing 而非 I/O

### IO_Pattern_Analysis.md
I/O 模式分析报告，通过实测 diskstats 确定测试时的 I/O 特征。
- **结论**：4-16KB 小块 posting 粒度随机读为主
- 证据：平均请求 7.87KB，合并率 0.01%，IOPS 137,932
- 对比：NVMe 顺序读吞吐 3938 MB/s，SPANN 仅 1060 MB/s (27%)
- 根因：Routing 由距离决定，不遵循 posting_id 顺序，物理位置分散

### Posting_List_Analysis_Report.md
Posting List 级别性能瓶颈分析。
- **关键发现**：I/O Wait 分布均匀 (Gini ~0.3)，无集中热点
- 77% posting 只被访问 1 次 (SIFT10M, Q=10,000)
- Cross-query reuse 公式：R = (Q × K) / N
- SIFT10M reuse = 1.28 次/posting (SIFT1M = 6.5 次)
- 单页 posting 被访问比例最低 (28.9%)，多页 posting 被访问比例更高 (~50%)

## 当前进度 (2026-05-03)

**主线**: Query-aware Adaptive Posting Budget

### 已完成工作

| 优先级 | 任务 | 状态 | 结果 |
|--------|------|------|------|
| S0 | Trace-only diagnosis | **已完成** | Cross-query page reuse 92.4% |
| M1 | ShardedPageCache | **已完成** | st8 QPS +5.0%，st16 +13.6% |
| M2-H | Selective Hybrid | **已证伪** | I/O wait 分布均匀 (Gini ~0.3) |
| M4 | Storage-Optimized Sidecar | **已完成** | 存储 -73%，在线查询不可行 |

### M4 最终结论

| 阶段 | 状态 | 结果 |
|------|------|------|
| A0: Legacy baseline replay | ✅ 完成 | QPS=743, Recall=0.9778 |
| A1: Storage-only sidecar builder | ✅ 完成 | 存储减少 **73.1%** |
| A2: Offline query simulator | ✅ 完成 | VIDOrder: 2135, CoHit: 1257 pages/query |
| A3: Pure online M4 prototype | ❌ 不执行 | A2 结果超出阈值 3.5x |
| A4: Hybrid online M4 prototype | ❌ 不执行 | A2 结果不符合决策规则 |

**结论**：M4 仅作为 Storage-only Sidecar，用于归档/分发/冷存储。

### Query-aware Adaptive Budget 当前进度

| 阶段 | 任务 | 状态 |
|------|------|------|
| Phase 1 | Offline Oracle | 待开始 |
| Phase 2 | Feature Extraction | 待开始 |
| Phase 3 | Rule-based Policy | 待开始 |
| Phase 4 | Learned Policy | 待开始 |

详细计划见: `SPANN_Query_Aware_Adaptive_Posting_Budget_Plan_20260503.md`

## 常用测试命令

```bash
# 运行 M1 缓存测试
cd /home/ray/code/SPTAG
./Release/ssdserving results/io_analysis/m1_test/m1_test_cache_enabled.ini

# 查看 cache 统计
grep "Page cache stats" results/io_analysis/m1_test/*.log

# 对比 baseline
./Release/ssdserving results/io_analysis/m1_test/m1_test_st8.ini
```

## 配置文件注意事项

### SimpleIniReader 限制

**重要**：`SimpleIniReader` 只支持 `;` 作为注释符，**不支持 `#`、`//` 等**。

```ini
; 正确 - 使用分号注释
; EnablePageCache=true

# 错误 - 井号不是注释符，会导致解析失败
# This will break parsing
EnablePostingTrace=true
```

使用 `#` 注释会导致：
1. 该行解析失败
2. 后续参数可能被跳过或错位
3. 错误是**静默的**，不会报错但参数不会被设置

### Debug 配置解析问题

如果参数没有被正确设置：

1. 检查注释符是否正确（必须用 `;`）
2. 检查 section 名称是否正确（`[Base]`、`[SearchSSDIndex]` 等）
3. 检查参数名拼写和大小写（虽然 `StrEqualIgnoreCase` 会忽略大小写）
4. 在 `Options::SetParameter` 中添加日志确认解析路径

## LightGBM 模型转换 JSON 格式踩坑（2026-05-08）

### 问题描述

C++ 代码 (`AdaptiveBudgetModel.h`) 需要加载 JSON 格式的 LightGBM 模型，但直接从 LightGBM 转换时踩了坑。

### 错误方法：从 dump_model() 嵌套结构转换

```python
# 错误方法 - dump_model() 返回嵌套树结构
dump = model.dump_model()
# tree_structure 是嵌套的 dict:
# {"split_feature": 0, "left_child": {...}, "right_child": {...}}
```

**问题**：手动将嵌套结构扁平化时，`split_feature` 数组使用前序遍历，但 `left_child`/`right_child` 索引基于后序遍历，导致索引不匹配，预测结果完全错误。

**症状**：
- JSON 模型预测值与 LightGBM 预测值差异巨大（如 0.09 vs 0.87）
- 节点 0 的 `left_child` 应该是内部节点索引，却指向叶子节点

### 正确方法：从 save_model() 文本格式解析

```python
# 正确方法 - save_model() 输出已扁平化的文本格式
model.save_model('model.txt')
# 文本格式已包含正确扁平化的数组:
# split_feature=11 23 12 ...
# left_child=1 4 -1 ...
# right_child=2 5 -2 ...
```

使用 `scripts/export_lgbm_to_json.py` 直接解析这个格式：

```bash
python3 scripts/export_lgbm_to_json.py model.txt model.json
```

### JSON 格式要求

C++ 代码期望的 JSON 结构：

```json
{
  "feature_names": ["d1", "d2", ...],
  "num_features": 24,
  "trees": [
    {
      "num_leaves": 31,
      "split_feature": [11, 23, 12, ...],
      "threshold": [0.385, 0.752, ...],
      "left_child": [1, -1, 3, ...],
      "right_child": [2, -2, 4, ...],
      "leaf_value": [0.871, 0.902, ...]
    }
  ]
}
```

**关键点**：
- `left_child`/`right_child` 中负数表示叶子节点，索引为 `-(value) - 1`
- C++ 遍历时：`while (node >= 0) { ... }` 然后用 `leaf_idx = -(node) - 1` 获取叶子值
- 所有树的叶子值求和后经过 sigmoid：`1 / (1 + exp(-sum))`

### 验证方法

```python
import json
import lightgbm as lgb
import numpy as np

def predict_single_tree(tree, x):
    node = 0
    while node >= 0:
        if x[tree['split_feature'][node]] <= tree['threshold'][node]:
            node = tree['left_child'][node]
        else:
            node = tree['right_child'][node]
    leaf_idx = -node - 1
    return tree['leaf_value'][leaf_idx]

def predict_model(json_model, features):
    total = sum(predict_single_tree(t, features) for t in json_model['trees'])
    return 1.0 / (1.0 + np.exp(-total))

# 对比验证
with open('model.json') as f:
    json_model = json.load(f)
lgb_model = lgb.Booster(model_file='model.txt')

json_pred = predict_model(json_model, features)
lgb_pred = lgb_model.predict(features.reshape(1, -1))[0]
assert abs(json_pred - lgb_pred) < 1e-6, "转换错误！"
```

### 已转换的正确模型位置

- SIFT1M 模型：`results/adaptive_budget/strict_train_test/risk_model_b*.json`
- SIFT10M 模型：`results/adaptive_budget/sift10m_specific/risk_model_b*.json`
