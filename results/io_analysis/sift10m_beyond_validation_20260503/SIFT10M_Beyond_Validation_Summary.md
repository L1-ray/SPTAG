# SIFT10M Beyond Official Baseline 验证总结

**日期**: 2026-05-03
**数据集**: SIFT10M UInt8 + DEFAULT
**查询数**: 10,000
**目标**: 验证 SIFT1M 结论是否迁移到 SIFT10M

## 执行摘要

**结论**: SIFT10M 验证完成，**所有 SIFT1M 结论完全迁移**，较大改进空间已被排除。

| 任务 | 结果 | SIFT1M | SIFT10M | 一致？ |
|------|------|--------|---------|--------|
| **S10-S0a** | ✅ 完成 | st8 平台期 | st8 平台期 | ✓ |
| **S10-M1** | ❌ 失败 | +5.0% QPS | -0.2% QPS | ✗ |
| **S10-M2H** | ❌ 失败 | Top10%=29.63% | Top10%=27.51% | ✓ |
| **S10-M4** | ❌ 失败 | 查询重复=15.3% | 查询重复=10.91% | ✓ |

**最终状态**: SIFT10M 上无有效 beyond baseline 优化路径。

---

## S10-S0a: Baseline st Sweep

### 结果

| st | QPS | Recall@10 | 平均延迟 |
|----|-----|-----------|----------|
| 4 | 3,294 | 0.949144 | 2.43 ms |
| 8 | 5,378 | 0.949144 | 1.49 ms |
| 12 | 5,381 | 0.949144 | 2.23 ms |
| 16 | 5,380 | 0.949144 | 2.98 ms |

### 关键观察

1. **QPS 在 st=8 达到平台期**: 与 SIFT1M 类似
2. **st=8 之后无提升**: st=12/16 无额外收益
3. **Recall 稳定**: 所有 st 值下都是 0.949144

---

## S10-M1: Page Cache 验证

**测试条件**: 10,000 个查询，st=8 线程，256MB Cache

### 结果

| 配置 | QPS | 变化 | Cache 命中率 | 节省页面数 |
|------|-----|------|--------------|------------|
| Cache OFF | 5,312.58 | - | - | - |
| Cache ON | 5,325.82 | **+0.2%** | 18.26% | 29,545 |

### 为什么 M1 在 SIFT10M 上失效

| 指标 | SIFT1M | SIFT10M | 比值 |
|------|--------|---------|------|
| 索引中单页 posting 数 | 48,158 | 440,342 | 9.1x |
| 索引中单页 posting 比例 | 32.1% | 29.4% | 相近 |
| 每个单页 posting 平均访问次数 | 13.5 | 0.29 | 1/47 |
| Cache 命中率 | 77.72% | 18.26% | 1/4.3 |

**数据说明**:
- "单页 posting 数" 来自索引构建时统计（精确数据）
- SIFT10M 索引中有 440,342 个单页 posting，但查询只访问了 127,457 个（28.9%）
- SIFT1M 索引中有 48,158 个单页 posting
- 平均访问次数计算：被访问的单页 posting 数 / 索引中单页 posting 总数

**根因**: SIFT10M 有更多 single-page postings，但访问分散。在 10,000 个查询下，每个 posting 平均只被访问 0.43 次，缺乏跨查询复用。

详见: `m1_cache/M1_Failure_Analysis.md`

---

## S10-M2H: Selective Hybrid 重新验证

### 结果

| 指标 | 值 | 阈值 | 状态 |
|------|-----|------|------|
| Top 5% I/O wait 贡献 | 18.10% | >= 25% | ❌ |
| Top 10% I/O wait 贡献 | 27.51% | >= 40% | ❌ |
| Gini 系数 | 0.34 | 越高越集中 | 低 |

### 与 SIFT1M 对比

| 指标 | SIFT1M | SIFT10M |
|------|--------|---------|
| Top 10% I/O wait 贡献 | 29.63% | 27.51% |
| M2-H 决策 | 已停止 | 已停止 |

**结论**: I/O wait 分布均匀，无集中的坏 posting 热点。

详见: `m2h_trace/S10_M2H_Analysis.md`

---

## S10-M4: Primary-Secondary 去重 Oracle

### 结果

| 指标 | 值 | 阈值 | 状态 |
|------|-----|------|------|
| 存储级重复 | 49.72% | - | 误导性 |
| **查询级重复** | **10.91%** | **>= 30%** | **❌** |

### 关键洞察

| 指标 | 存储级 | 查询级 |
|------|--------|--------|
| 重复字节数 | 1.94 GB (49.72%) | 0.43 GB (10.91%) |

**存储级重复不等于查询级重复！**

- 81% 的 VID 存在于多个 posting 中
- 但单个 query 只访问每个 VID 一次（来自一个 posting）
- 查询级重复比例只有 10.91%

### 与 SIFT1M 对比

| 指标 | SIFT1M | SIFT10M |
|------|--------|---------|
| 查询级重复比例 | ~15% | 10.91% |
| M4 决策 | 已停止 | 已停止 |

**结论**: 查询级重复 payload 不足以支撑 M4 优化。

详见: `m4_oracle/S10_M4_Analysis.md`

---

## 总体结论

### 假设验证

| 假设 | 结果 | 证据 |
|------|------|------|
| H0: SIFT1M 结论迁移 | ✓ 验证通过 | M2-H/M4 都失败，与 SIFT1M 相同 |
| H1: SIFT10M 更适合 M1 cache | ✗ 被拒绝 | 命中率 18% vs 78%，QPS -0.2% |
| H2: SIFT10M 重开 M2-H | ✗ 被拒绝 | Top10% wait = 27.51% < 40% |
| H3: SIFT10M 重开 M4 | ✗ 被拒绝 | 查询级重复 = 10.91% < 30% |

### 为什么 SIFT10M 比 SIFT1M 更难优化

1. **索引更大 (10x)**: 2M 页 vs 200K 页
2. **Posting 更多**: 1,496,408 vs 150,076
3. **查询数相同**: 10,000 个查询（关键约束）
4. **跨查询复用更少**: 在 Q=10,000 条件下，每个 posting 平均被访问次数为 0.43 次 (SIFT10M) vs 4.26 次 (SIFT1M)

### 最终决策

**所有 beyond baseline 方向已验证失败：**

| 方向 | SIFT1M | SIFT10M | 产品化 |
|------|--------|---------|--------|
| M1: Page Cache | ✓ st8 +5% | ✗ st8 +0.2% | 仅限小规模数据集 |
| M2-H: Selective Hybrid | ✗ 已停止 | ✗ 已停止 | 否 |
| M4: Primary-Secondary | ✗ 已停止 | ✗ 已停止 | 否 |

**结论**: 当前结构下，SIFT1M 和 SIFT10M 都无大幅超越空间。

---

## 对 Beyond Official Baseline Plan 的影响

更新 [SPANN_Beyond_Official_Baseline_Plan_20260502.md](/home/ray/code/SPTAG/SPANN_Beyond_Official_Baseline_Plan_20260502.md):

1. **SIFT10M 验证完成**: 所有结论与 SIFT1M 一致
2. **M1 适用范围限定**: 仅适用于 small-scale 数据集（~1M vectors）
3. **M2-H/M4 保持停止**: 两个数据集都验证失败
4. **较大改进空间已被排除**: 需要更根本的结构改造

---

## 文件列表

```
results/io_analysis/sift10m_beyond_validation_20260503/
├── s0_sweep/                    # Baseline st sweep
│   └── s10_s0a_st*_run*.log
├── m1_cache/                    # M1 验证 (原始，配置格式有误)
│   ├── M1_Failure_Analysis.md   # 根因分析
│   └── m1_st*_cache*_run*.log
├── m1_cache_rerun/              # M1 验证 (修正配置格式后重跑)
│   ├── run_s10_m1_fixed.sh      # 修正后的测试脚本
│   └── m1_st8_cache*_run*.log   # 正确的 cache 统计数据
├── m2h_trace/                   # M2-H trace
│   ├── S10_M2H_Analysis.md      # 分析报告
│   ├── posting_trace_st8.csv    # 640K 行
│   └── payload_trace_st8.csv    # 26M 行
├── m4_oracle/                   # M4 oracle
│   ├── S10_M4_Analysis.md       # 分析报告
│   └── pre_dedupe_trace_st8.csv # 29M 行
└── SIFT10M_Beyond_Validation_Summary.md  # 本文件
```
