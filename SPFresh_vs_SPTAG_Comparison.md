# SPFresh 仓库与微软官方 SPTAG 仓库对比分析（修订版）

本文档对比 `github.com/SPFresh/SPFresh`（本仓库语义）与 `github.com/microsoft/SPTAG`（官方仓库）。

> 校验基线（本次修订）
> - 校验日期：2026-04-25
> - 代码基线：本地工作区 + `upstream/main`
> - 说明：以下结论以当前可见代码状态为准；历史阶段性差异可能与当前状态不同

---

## 一、仓库基本信息

| 特性 | 微软 SPTAG (官方) | SPFresh (本仓库语义) |
|------|------------------|----------------------|
| GitHub 地址 | `github.com/microsoft/SPTAG` | `github.com/SPFresh/SPFresh` |
| 维护者 | 微软官方 | 论文作者及社区协作 |
| 功能定位 | 通用 ANN 索引库 | 以 SPFresh 论文能力与实验为重点的实现与实践 |
| 论文关联 | 官方仓库本身不绑定单篇论文 | SOSP 2023: SPFresh |
| 提交数量 | 动态变化 | 动态变化 |

说明：提交数量不应在文档中写死，建议以 GitHub 实时页面为准。

---

## 二、目录结构现状

### 2.1 当前状态结论

在当前 `upstream/main` 中，SPFresh 相关核心目录和文件已经可见，不能再简单描述为“仅本仓库独有”。

### 2.2 关键目录存在性（代码可见性）

| 路径 | upstream/main | 本仓库 | 备注 |
|------|---------------|--------|------|
| `AnnService/inc/SPFresh/` | ✅ | ✅ | SPFresh 测试框架头文件 |
| `AnnService/src/SPFresh/` | ✅ | ✅ | `spfresh` 入口源码 |
| `AnnService/src/UsefulTool/` | ✅ | ✅ | 数据/trace 工具源码 |
| `AnnService/inc/Core/SPANN/ExtraDynamicSearcher.h` | ✅ | ✅ | 动态更新核心实现 |
| `AnnService/inc/Core/SPANN/ExtraRocksDBController.h` | ✅ | ✅ | RocksDB Merge Operator |
| `AnnService/inc/Core/SPANN/ExtraSPDKController.h` | ✅ | ✅ | SPDK 后端控制器 |
| `Script_AE/` | ✅ | ✅ | 论文实验脚本目录 |
| `ThirdParty/spdk/` | ✅ | ✅ | SPDK 依赖目录 |
| `ThirdParty/isal-l_crypto/` | ✅ | ✅ | SPDK 相关依赖目录 |

### 2.3 本仓库中的相关规模（2026-04-25 实测）

| 路径 | 实测规模 | 用途 |
|------|----------|------|
| `AnnService/inc/SPFresh/SPFresh.h` | 1246 行 | 流式更新测试框架 |
| `AnnService/src/SPFresh/main.cpp` | 33 行 | `spfresh` 可执行文件入口 |
| `AnnService/src/UsefulTool/main.cpp` | 671 行 | 数据生成与 trace 工具 |
| `AnnService/inc/Core/SPANN/ExtraDynamicSearcher.h` | 2721 行 | 动态更新主逻辑 |
| `Script_AE/` | 58 个文件 | Figure 1/6/7/8/9/10/11 实验复现 |

---

## 三、核心代码对比（当前状态）

### 3.1 动态更新组件（ExtraDynamicSearcher）

下列关键类/函数在当前代码中均可见，且 upstream/main 同样可见：

| 功能类/函数 | 作用 | upstream/main | 本仓库 |
|------------|------|---------------|--------|
| `MergeAsyncJob` | 异步合并 Posting List | ✅ | ✅ |
| `SplitAsyncJob` | 异步分裂 Posting List | ✅ | ✅ |
| `ReassignAsyncJob` | 异步路由重分配 | ✅ | ✅ |
| `MergePostings()` | 合并小 Posting List | ✅ | ✅ |
| `Split()` | Posting List 分裂 | ✅ | ✅ |
| `Reassign()` | 向量路由重分配 | ✅ | ✅ |
| `CheckIsNeedReassign()` | 判断是否需要重分配 | ✅ | ✅ |
| `CollectReAssign()` | 收集待重分配向量 | ✅ | ✅ |
| `QuantifyAssumptionBroken()` | 假设破坏度量 | ✅ | ✅ |

### 3.2 参数定义（ParameterDefinitionList.h）

以下参数在当前代码中可见；示例默认值按实码修正如下：

```cpp
DefineSSDParameter(m_preReassign, bool, false, "PreReassign")
DefineSSDParameter(m_preReassignRatio, float, 0.7f, "PreReassignRatio")
DefineSSDParameter(m_inPlace, bool, true, "InPlace")
DefineSSDParameter(m_reassignThreadNum, int, 0, "ReassignThreadNum")
DefineSSDParameter(m_disableReassign, bool, false, "DisableReassign")
DefineSSDParameter(m_reassignK, int, 0, "ReassignK")
DefineSSDParameter(m_steadyState, bool, false, "SteadyState")
DefineSSDParameter(m_stressTest, bool, false, "StressTest")
DefineSSDParameter(m_mergeThreshold, int, 10, "MergeThreshold")
```

说明：这些参数属于当前代码能力的一部分，不能再表述为“仅 SPFresh 仓库独有”。

### 3.3 RocksDB Merge Operator

`AnnMergeOperator`、`FullMergeV2`、`PartialMergeMulti` 在当前代码中可见；文档中“支持追加合并”的方向性描述正确。

### 3.4 VersionLabel

`VersionLabel`（含软删除与版本递增逻辑）在当前代码中可见，路径为 `AnnService/inc/Core/Common/VersionLabel.h`。

---

## 四、可执行文件与入口代码

### 4.1 可执行目标（本仓库 CMake）

本仓库当前 CMake 中可见：`spfresh` 与 `usefultool` 目标。

### 4.2 `spfresh` 入口（修正后示例）

```cpp
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        SPTAGLIB_LOG(Helper::LogLevel::LL_Error, "spfresh storePath\n");
        exit(-1);
    }

    auto ret = SSDServing::SPFresh::UpdateTest(argv[1]);
    return ret;
}
```

---

## 五、实验复现框架（Script_AE）

`Script_AE/` 包含 Figure1、Figure6、Figure7、Figure8、Figure9、Figure10、Figure11 相关脚本和 ini 配置，文档原有目录结构描述总体正确。

需要修正的是“本仓库独有”这一措辞：在当前 upstream/main 中也可见同类目录与脚本。

---

## 六、依赖与构建选项（修正）

### 6.1 依赖与开关的准确表述

| 依赖/组件 | 当前状态（本仓库 CMake） | 说明 |
|-----------|--------------------------|------|
| Boost | 必需 | 构建失败会直接报错 |
| OpenMP | 必需 | 构建失败会直接报错 |
| TBB | 默认开启（`TBB=ON`），可关闭 | 非绝对必需 |
| RocksDB | 默认关闭（`ROCKSDB=OFF`） | 按需启用 |
| SPDK | 默认关闭（`SPDK=OFF`） | 按需启用 |
| isal-l_crypto | 与 SPDK 路径相关 | 通常在启用 SPDK 时需要 |
| zstd | 仓库内置 ThirdParty | 默认参与构建 |
| jemalloc/snappy | 可选 | 常见于特定 RocksDB 配置 |

### 6.2 关于“修改版 RocksDB”

“使用修改版 RocksDB”是特定实验/性能路径的做法，不应写成所有场景下的硬性前提。

---

## 七、构建方式（建议写法）

### 7.1 默认构建（不开启 RocksDB/SPDK）

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
```

### 7.2 按需开启扩展组件

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DROCKSDB=ON -DSPDK=ON ..
make -j
```

说明：当开启 SPDK 时，需要确保 `ThirdParty/spdk`、`ThirdParty/isal-l_crypto` 及其依赖已正确构建。

---

## 八、运行示例（路径修正）

### 8.1 流式更新测试示例

```bash
# 1. 构建索引（示例）
./Release/ssdserving Script_AE/iniFile/build_sift1m.ini

# 2. 复制配置文件（示例）
cp Script_AE/iniFile/store_sift1m/indexloader_sift1m.ini \
   /path/to/store_sift1m/indexloader.ini

# 3. 运行流式更新测试
./Release/spfresh /path/to/store_sift1m
```

---

## 九、功能对比总结（按当前代码状态）

| 功能 | upstream/main | 本仓库 |
|------|---------------|--------|
| BKT/KDT/RNG 与内存索引能力 | ✅ | ✅ |
| SPANN（含动态更新相关实现） | ✅ | ✅ |
| 向量插入/软删除/重分配相关代码 | ✅ | ✅ |
| RocksDB Merge Operator 相关实现 | ✅ | ✅ |
| SPDK 控制器相关实现 | ✅ | ✅ |
| Script_AE 论文复现脚本 | ✅ | ✅ |
| 本地扩展文档与测试脚本（如部分 benchmark 文档） | 部分或无 | ✅ |

---

## 十、关系总结（修正版）

历史上，SPFresh 曾作为对 SPTAG 的显著功能扩展路径存在；
但在当前可见代码状态下，SPFresh 的大部分核心能力已经并入 upstream/main。

因此，当前更准确的表述是：

- “SPFresh 思路与实现对 SPTAG 主线演进有重要贡献”；
- “本仓库与官方主线在核心代码能力上已高度收敛”；
- “本仓库仍保留/补充了部分本地化文档、脚本与实验资产”。

不再建议将当前状态简单归纳为“本仓库是官方仓库的超集”。

---

**文档版本**: 2.0  
**修订日期**: 2026-04-25  
**修订说明**: 基于当前代码与 upstream/main 的可见性核验，修正了“独有/超集”结论、参数默认值、行数统计、依赖必需性与示例路径。