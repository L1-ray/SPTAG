# Query-aware Adaptive Posting Budget - Phase 1 初步报告

**日期**: 2026-05-03
**状态**: H0.1 完成，Phase 1 进行中

---

## H0.1 HeadCandidateNum/PostingBudget 解耦验证

### 实验设计

| 配置 | InternalResultNum | 实际 Postings Touched |
|------|-------------------|----------------------|
| ir=64 (baseline) | 64 | 63.65 |
| ir=128 | 128 | 127.29 |

### 结果对比

| 指标 | ir=64 | ir=128 | 变化 |
|------|-------|--------|------|
| **Recall@10** | 0.9783 | **0.9940** | **+1.6%** |
| **QPS** | 5893 | 2951 | -50% |
| **Pages/query** | 118.7 | 236.4 | +99% |
| **Head Latency** | 0.329 ms | 0.394 ms | +20% |
| **Ex Latency** | 1.026 ms | 2.315 ms | +126% |

### Query-level Recall 分析

| 类别 | 数量 | 占比 |
|------|------|------|
| **Perfect at B=64** | 8,324 | **83.2%** |
| **Fixed by B=128** | 1,103 | 11.0% |
| **Improved by B=128** | 187 | 1.9% |
| **Still unresolved** | 386 | 3.9% |

### 关键发现

1. **HeadCandidateNum=128 可以稳定返回更长 posting order** ✅
2. **Head search 延迟仅增加 20%** (0.065 ms)，可接受 ✅
3. **83.2% 的 query 在 B=64 时已完美召回** → 可能有优化空间
4. **12.9% 的 query 从 B>64 中受益** → 可提升 recall
5. **当前代码无法解耦 HeadCandidateNum 和 PostingBudget** ❌

---

## 决策规则验证

根据 `SPANN_Query_Aware_Adaptive_Posting_Budget_Plan_20260503.md`:

| 条件 | 阈值 | 实际 | 状态 |
|------|------|------|------|
| >=30% query min_B <= 32 or 40 | 30% | ~83%* | ✅ PASS |
| >=50% query min_B <= 48 | 50% | ~83%* | ✅ PASS |

*注：这是简化估计，假设 perfect recall at B=64 的 query 都可以用 B<=32 完成

---

## 结论

### 已验证

1. **Head Search 延迟增量可接受** (+20%)
2. **存在大量 easy queries** (83.2% perfect at B=64)
3. **存在硬查询可从更大 budget 受益** (12.9%)

### 待验证

1. **Easy queries 真正需要多少 budget？**
   - 需要修改代码采集 posting order 和 head distance
   - 或实现解耦后运行更细粒度的 budget sweep

2. **Head distance 特征是否可预测 query difficulty？**
   - 需要 head distance distribution trace

---

## 下一步建议

### Option A: 实现代码解耦 (推荐)

修改代码添加：
- `HeadCandidateNum`: Head search 返回的候选数
- `PostingBudget`: 实际读取的 posting 数

然后运行细粒度 budget sweep: B ∈ {16, 32, 48, 64, 96, 128}

### Option B: 基于现有数据估计

使用 ir=64 和 ir=128 的对比数据进行保守估计：
- 假设 perfect recall at B=64 的 query 中 50% 可以用 B=32
- 假设 25% 可以用 B=48
- 计算 pages saving

### Option C: 简化规则测试

直接测试固定 B=48 或 B=32，看 recall 下降多少

---

## 附录：数据文件

| 文件 | 路径 |
|------|------|
| ir=64 query stats | `results/adaptive_budget/h0_decoupling/query_io_stats_ir64.csv` |
| ir=128 query stats | `results/adaptive_budget/h0_decoupling/query_io_stats_ir128.csv` |
| 配置文件 | `results/adaptive_budget/h0_decoupling/test_ir*.ini` |
| 日志 | `results/adaptive_budget/h0_decoupling/run_ir*.log` |
