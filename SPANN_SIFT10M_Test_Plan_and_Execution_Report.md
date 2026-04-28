# SPANN SIFT10M 测试计划与执行报告（合并版）

## 文档说明
- 本文档合并自：
- `SIFT10M_Data_Preparation.md`（核心数据准备）
- `SPANN_SIFT10M_Test_Plan.md`（已改为 10k query 口径）
- `SPANN_SIFT10M_Execution_Report.md`（最新执行结果）
- 所有扫频与对照结果均以完整 10k query 为口径（未设置 `QueryCountLimit`）。

## 一、数据准备

### 1. 目标产物
在 `/media/ray/1tb/sift10m/` 下准备：
- `sift_base_10m.fvecs`
- `sift_query.fvecs`
- `sift_groundtruth.ivecs`
- `spann_index/`
- `raw/`

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
/media/ray/1tb/sift10m/
  sift_base_10m.fvecs
  sift_query.fvecs
  sift_groundtruth.ivecs
  spann_index/
  search_results.bin
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

### 1. 构建阶段
- 索引目录：`/media/ray/1tb/sift10m/spann_index`
- 索引总大小：约 `33G`
- `SPTAGFullList.bin`：约 `31G`
- 构建耗时：
- `SelectHead: 1625s`
- `BuildHead: 4569s`
- `BuildSSDIndex: 892s`
- 总耗时：`7094s`

### 2. IR Sweep（10k query）
| run | QPS | Recall@10 | total_ms | head_ms | head_pct | ex_ms | batch_ms | pages | readKB | readMB/s | queueDepth | cpuIdle | iowait |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ir16_st4_nt16_pl15 | 88.01 | 0.800686 | 45.407 | 34.437 | 75.8% | 10.970 | 3.053 | 104.6 | 418.4 | 32.014 | 2.313 | 73.432 | 0.041 |
| ir24_st4_nt16_pl15 | 86.58 | 0.856146 | 46.159 | 33.849 | 73.3% | 12.310 | 4.304 | 156.5 | 626.0 | 46.983 | 4.639 | 73.330 | 0.092 |
| ir32_st4_nt16_pl15 | 86.65 | 0.889739 | 46.123 | 32.595 | 70.7% | 13.528 | 5.616 | 208.1 | 832.3 | 62.716 | 7.968 | 74.012 | 0.044 |
| ir48_st4_nt16_pl15 | 84.88 | 0.926933 | 47.087 | 30.048 | 63.8% | 17.039 | 9.571 | 311.1 | 1244.5 | 92.160 | 18.112 | 75.084 | 0.458 |

### 3. Page Sweep（10k query）
| run | QPS | Recall@10 | total_ms | head_ms | head_pct | ex_ms | batch_ms | pages | readKB | readMB/s | queueDepth | cpuIdle | iowait |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| pl12_ir32_st4_nt16 | 86.57 | 0.889739 | 46.165 | 32.627 | 70.7% | 13.538 | 5.556 | 208.1 | 832.3 | 62.690 | 7.869 | 73.846 | 0.091 |
| pl15_ir32_st4_nt16 | 86.60 | 0.889739 | 46.144 | 32.544 | 70.5% | 13.600 | 5.671 | 208.1 | 832.3 | 62.839 | 8.120 | 73.764 | 0.049 |
| pl4_ir32_st4_nt16 | 86.65 | 0.889739 | 46.126 | 32.654 | 70.8% | 13.471 | 5.514 | 208.1 | 832.3 | 62.901 | 7.788 | 73.928 | 0.047 |
| pl8_ir32_st4_nt16 | 83.87 | 0.889739 | 47.655 | 33.758 | 70.8% | 13.897 | 5.693 | 208.1 | 832.3 | 60.986 | 7.906 | 71.703 | 0.060 |

### 4. Full10k 对照矩阵
| run | QPS | Recall@10 | total_ms | head_ms | head_pct | ex_ms | batch_ms | pages | readKB | readMB/s | queueDepth | cpuIdle | iowait |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline_st4_nt16_ir32_pl15_mc2048 | 84.05 | 0.889739 | 47.550 | 33.646 | 70.8% | 13.904 | 5.792 | 208.1 | 832.3 | 60.904 | 8.084 | 71.589 | 0.067 |
| lowlat_st2_nt16_ir32_pl15_mc2048 | 79.26 | 0.889739 | 25.221 | 15.979 | 63.4% | 9.243 | 5.395 | 208.1 | 832.3 | 57.836 | 7.012 | 82.415 | 0.065 |
| highthr_st8_nt16_ir32_pl15_mc2048 | 85.25 | 0.889739 | 93.670 | 70.082 | 74.8% | 23.588 | 6.596 | 208.1 | 832.3 | 61.760 | 9.531 | 44.590 | 0.084 |
| lowir_st4_nt16_ir24_pl15_mc2048 | 87.23 | 0.856146 | 45.820 | 33.582 | 73.3% | 12.238 | 4.278 | 156.5 | 626.0 | 47.375 | 4.633 | 73.675 | 0.082 |
| lowpl_st4_nt16_ir32_pl8_mc2048 | 86.96 | 0.889739 | 45.957 | 32.506 | 70.7% | 13.451 | 5.539 | 208.1 | 832.3 | 63.048 | 7.859 | 74.188 | 0.073 |
| joint_st4_nt16_ir24_pl8_mc1536 | 94.17 | 0.854156 | 42.437 | 33.984 | 80.1% | 8.453 | 4.321 | 156.7 | 626.8 | 50.816 | 5.043 | 72.271 | 0.047 |

## 四、合并版结论
- 已完全移除 1k query 口径，SIFT10M 结果统一为 10k query。
- 在 SATA SSD 下，`IR=32` 时每 query 读取量约 `832KB`、页数约 `208`，读带宽约 `60-63MB/s`，表现出稳定的 I/O 路径上限特征。
- `IR` 与 `PageLimit` 优化可以改变吞吐和召回折中，但并未改变“高并发增益有限、延迟明显放大”的整体形态。
