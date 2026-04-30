# SPANN SIFT10M 测试计划与执行报告（合并版）

## 文档说明
- 本文档合并自：
- `SIFT10M_Data_Preparation.md`（核心数据准备）
- `SPANN_SIFT10M_Test_Plan.md`（已改为 10k query 口径）
- `SPANN_SIFT10M_Execution_Report.md`（最新执行结果）
- 所有扫频与对照结果均以完整 10k query 为口径（未设置 `QueryCountLimit`）。

## 一、数据准备

### 1. 目标产物
当前 NVMe 机器以 `/home/ray/data/sift10m/` 作为执行入口；旧 `/media/ray/1tb/sift10m/` 仅代表历史 SATA 测试路径，保留用于后续对比。

在 `/home/ray/data/sift10m/` 下准备：
- `sift_base_10m.fvecs`
- `sift_query.fvecs`
- `sift_groundtruth.ivecs`
- `raw/`
- `spann_index/`（复制过来的现有索引，可直接用于搜索测试）
- `spann_index_nvme_sift10m_20260429/`（如需重建，本次 NVMe run 专用索引目录）
- `tmp/nvme_sift10m_20260429/`（本次 BuildSSDIndex 临时目录）

### 2. 数据来源与处理
- 原始来源：BIGANN（TexMex）数据集。
- 实际下载：使用 Hugging Face 镜像（规避当前环境 FTP 数据通道中断）。
- `base`：流式解压并截取前 10M，`dd iflag=fullblock bs=132 count=10000000`。
- `query`：`bigann_query.bvecs.gz` 解压后转 `fvecs`。
- `groundtruth`：`bigann_gnd.tar.gz` 解包后复制 `gnd/idx_10M.ivecs`。
- `bvecs -> fvecs`：逐条 `uint8 -> float32`。

### 3. 验收结果
- `sift_base_10m.fvecs`：`5,160,000,000` bytes，维度 `128`，向量数 `10,000,000`。
- `sift_query.fvecs`：`5,160,000` bytes，维度 `128`，向量数 `10,000`。
- `sift_groundtruth.ivecs`：`40,040,000` bytes，首维 `1000`，query 数 `10,000`。
- 备注：`idx_10M.ivecs` 每 query 提供 `1000` 个近邻，可用于 Recall@10/100。

## 二、测试计划（10k 口径）

### 1. 目标
- 验证 SIFT10M 下 SPANN 的瓶颈形态是否与 SIFT1M 一致。
- 重点观察：QPS、Recall、每 query 读取量、页面数、读带宽、队列深度、Head/Ex 延迟占比。

### 2. 数据与索引路径
```text
/home/ray/data/sift10m/
  sift_base_10m.fvecs
  sift_query.fvecs
  sift_groundtruth.ivecs
  spann_index/
  spann_index_nvme_sift10m_20260429/
  search_results_nvme_sift10m_20260429.bin
  tmp/nvme_sift10m_20260429/

/home/ray/code/SPTAG/results/io_analysis/sift10m_nvme_sift10m_20260429/
  baseline_st4_nt16_ir32_pl15_mc2048/
  ir_sweep/
  page_sweep/
  full10k/
```

### 3. 构建索引配置（核心）
- `BuildSsdIndex=true`
- `InternalResultNum=64`
- `PostingPageLimit=12`
- `NumberOfThreads=14`

### 4. 搜索测试统一约束
- 使用 BATCH_READ 模式。
- 保持 `NumberOfThreads >= SearchThreadNum`。
- 不设置 `QueryCountLimit`（完整 10k query）。

### 5. 搜索实验矩阵
- 并发基线：`SearchThreadNum = 2,4,8`（`NumberOfThreads=16`）
- IR 扫频：`InternalResultNum = 16,24,32,48`（`st=4, nt=16, pl=15, mc=2048`）
- Page 扫频：`SearchPostingPageLimit = 4,8,12,15`（`st=4, nt=16, ir=32, mc=2048`）
- 最终对照矩阵：
1. `st4 nt16 ir32 pl15 mc2048`
2. `st2 nt16 ir32 pl15 mc2048`
3. `st8 nt16 ir32 pl15 mc2048`
4. `st4 nt16 ir24 pl15 mc2048`
5. `st4 nt16 ir32 pl8 mc2048`
6. `st4 nt16 ir24 pl8 mc1536`

## 三、执行结果

### 1. 历史 SATA 构建阶段（仅用于对比）
- 索引目录：`/media/ray/1tb/sift10m/spann_index`
- 索引总大小：约 `33G`
- `SPTAGFullList.bin`：约 `31G`
- 构建耗时：
  - `SelectHead: 1625s`
  - `BuildHead: 4569s`
  - `BuildSSDIndex: 892s`
  - 总耗时：`7094s`

### 2. 历史 SATA 搜索结果（10k query）
#### IR Sweep
| run | QPS | Recall@10 | total_ms | readMB/s |
|---|---:|---:|---:|---:|
| ir16_st4_nt16_pl15 | 88.01 | 0.800686 | 45.407 | 32.014 |
| ir24_st4_nt16_pl15 | 86.58 | 0.856146 | 46.159 | 46.983 |
| ir32_st4_nt16_pl15 | 86.65 | 0.889739 | 46.123 | 62.716 |
| ir48_st4_nt16_pl15 | 84.88 | 0.926933 | 47.087 | 92.160 |

#### Page Sweep
| run | QPS | Recall@10 | total_ms | readMB/s |
|---|---:|---:|---:|---:|
| pl12_ir32_st4_nt16 | 86.57 | 0.889739 | 46.165 | 62.690 |
| pl15_ir32_st4_nt16 | 86.60 | 0.889739 | 46.144 | 62.839 |
| pl4_ir32_st4_nt16 | 86.65 | 0.889739 | 46.126 | 62.901 |
| pl8_ir32_st4_nt16 | 83.87 | 0.889739 | 47.655 | 60.986 |

#### Full10k 对照矩阵
| run | QPS | Recall@10 | total_ms | readMB/s |
|---|---:|---:|---:|---:|
| baseline_st4_nt16_ir32_pl15_mc2048 | 84.05 | 0.889739 | 47.550 | 60.904 |
| lowlat_st2_nt16_ir32_pl15_mc2048 | 79.26 | 0.889739 | 25.221 | 57.836 |
| highthr_st8_nt16_ir32_pl15_mc2048 | 85.25 | 0.889739 | 93.670 | 61.760 |
| lowir_st4_nt16_ir24_pl15_mc2048 | 87.23 | 0.856146 | 45.820 | 47.375 |
| lowpl_st4_nt16_ir32_pl8_mc2048 | 86.96 | 0.889739 | 45.957 | 63.048 |
| joint_st4_nt16_ir24_pl8_mc1536 | 94.17 | 0.854156 | 42.437 | 50.816 |

### 3. NVMe 冷缓存搜索结果（2026-04-29）
- 数据路径：`/home/ray/data/sift10m`
- 索引复用：直接复用复制过来的 `spann_index/` 做搜索，避免重建覆盖历史索引。
- 冷缓存方式：每次 run 前使用 `scripts/run_io_analysis.sh -C` 清空 OS page cache。
- 结果目录：`results/io_analysis/sift10m_nvme_sift10m_20260429/`

#### IR Sweep（cold cache）
| run | QPS | Recall@10 | avg_ms | p95_ms | readMB/s | dup_ratio | readKB/query |
|---|---:|---:|---:|---:|---:|---:|---:|
| ir16_st4_nt16_pl15 | 143.180 | 0.800686 | 27.893 | 32.898 | 118.667 | 0.058120 | 418.4 |
| ir24_st4_nt16_pl15 | 147.380 | 0.856146 | 27.096 | 32.166 | 147.250 | 0.075407 | 625.9 |
| ir32_st4_nt16_pl15 | 154.700 | 0.889739 | 25.813 | 30.208 | 178.884 | 0.089056 | 832.3 |
| ir48_st4_nt16_pl15 | 158.480 | 0.926933 | 25.196 | 28.519 | 234.178 | 0.110247 | 1244.5 |

#### Page Sweep（cold cache）
| run | QPS | Recall@10 | avg_ms | p95_ms | readMB/s | dup_ratio | readKB/query |
|---|---:|---:|---:|---:|---:|---:|---:|
| pl4_ir32_st4_nt16 | 151.020 | 0.889739 | 26.441 | 30.983 | 172.606 | 0.089056 | 832.3 |
| pl8_ir32_st4_nt16 | 143.610 | 0.889739 | 27.807 | 33.576 | 167.441 | 0.089056 | 832.3 |
| pl12_ir32_st4_nt16 | 155.940 | 0.889739 | 25.609 | 29.956 | 182.898 | 0.089056 | 832.3 |
| pl15_ir32_st4_nt16 | 156.060 | 0.889739 | 25.586 | 29.506 | 180.595 | 0.089056 | 832.3 |

#### Full10k 对照矩阵（cold cache）
| run | QPS | Recall@10 | avg_ms | p95_ms | readMB/s | dup_ratio | readKB/query |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline_st4_nt16_ir32_pl15_mc2048 | 146.190 | 0.889739 | 27.314 | 32.265 | 171.076 | 0.089056 | 832.3 |
| lowlat_st2_nt16_ir32_pl15_mc2048 | 151.410 | 0.889739 | 13.197 | 15.443 | 176.756 | 0.089056 | 832.3 |
| highthr_st8_nt16_ir32_pl15_mc2048 | 143.740 | 0.889739 | 55.499 | 68.212 | 169.500 | 0.089056 | 832.3 |
| lowir_st4_nt16_ir24_pl15_mc2048 | 148.960 | 0.856146 | 26.808 | 31.327 | 149.908 | 0.075407 | 625.9 |
| lowpl_st4_nt16_ir32_pl8_mc2048 | 156.440 | 0.889739 | 25.525 | 29.429 | 182.389 | 0.089056 | 832.3 |
| joint_st4_nt16_ir24_pl8_mc1536 | 170.610 | 0.854156 | 23.403 | 26.742 | 165.660 | 0.075634 | 626.8 |

## 四、合并版结论
- 已完全移除 1k query 口径，SIFT10M 结果统一为 10k query。
- 冷缓存 NVMe 下，SIFT10M baseline 从历史 SATA 的 `84.05 QPS` 提升到 `146.19 QPS`，Recall@10 保持 `0.889739` 不变，平均延迟从 `47.55 ms` 降到 `27.31 ms`。
- `IR` 提升在 NVMe 上仍然有效：`IR=16 -> 48` 时 QPS 从 `143.18` 升到 `158.48`，Recall@10 从 `0.800686` 升到 `0.926933`，同时每 query 读取量从 `418KB` 增到 `1244KB`，说明 NVMe 能承接更高随机读吞吐，但代价仍是更高读放大。
- `PageLimit` 对当前 NVMe 冷缓存结果影响不大，`pl12` 与 `pl15` 基本持平，Recall@10 都保持 `0.889739`，`pl8` 反而更慢。
- `st8` 仍然不是好点位：`highthr_st8_nt16_ir32_pl15_mc2048` 的 Recall@10 仍是 `0.889739`，但只有 `143.74 QPS`，平均延迟却升到 `55.50 ms`，说明更高 query 并发没有有效转成吞吐。
- 当前这批 NVMe 冷缓存结果里，吞吐最佳点是 `joint_st4_nt16_ir24_pl8_mc1536`（`170.61 QPS`，Recall@10=`0.854156`），最低延迟点是 `lowlat_st2_nt16_ir32_pl15_mc2048`（`13.20 ms`，Recall@10=`0.889739`）。
- 整体上，NVMe 明显抬高了 SIFT10M 的 I/O 上限，但并未改变“高并发增益有限、线程调度和查询侧串行工作仍会放大延迟”的根本形态。
