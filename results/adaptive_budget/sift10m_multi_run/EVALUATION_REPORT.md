# SIFT10M Learned Policy Evaluation Report

**Date**: 2026-05-04
**Dataset**: SIFT10M
**Queries**: 10,000
**Status**: ✅ 多次测试验证通过

---

## Executive Summary

| Metric | Baseline (B=64) | Learned Policy | Delta | Target | Status |
|--------|-----------------|----------------|-------|--------|--------|
| **QPS** | 5,305.98 ± 3.5 | **5,972.58 ± 17.6** | **+12.56%** | >= 10% | ✅ |
| **Recall@10** | 0.949134 | 0.947824 | -0.001310 | <= 0.002 | ✅ |
| **P99 Latency** | 1.935ms | 1.915ms | -1.0% | <= 5% | ✅ |
| **Avg pages/query** | 125.9 | 98.1 | -22.1% | >= 12% | ✅ |
| **Low-recall queries (<0.7)** | 162 | 162 | 0 | not worse | ✅ |

**结论**: 所有 Productize criteria 满足，Learned Policy 验证通过。

---

## Pareto Analysis (vs Fixed Budget)

| Policy | QPS | Recall@10 | Pages/Query | Notes |
|--------|-----|-----------|-------------|-------|
| Fixed B=64 (baseline) | 5,306.0 | 0.949134 | 125.9 | Reference |
| Fixed B=48 | 6,504.8 | 0.927921 | 94.7 | **High recall loss (-2.1%)** |
| **Learned Policy** | 5,972.6 | 0.947824 | 98.2 | **Near-baseline recall (-0.13%)** |

### Key Finding

**Fixed B=48** achieves higher QPS (+22.6%) but sacrifices significant recall (-2.1%):
- This is NOT a viable alternative because recall loss is unacceptable
- Recall drops from 94.9% to 92.8%

**Learned Policy** maintains near-baseline recall (-0.13%) while improving QPS (+12.6%):
- Query-aware: identifies easy queries and reduces their budget
- Hard queries still get sufficient budget (B=64)
- This is the TRUE improvement over naive budget reduction

### Pareto Interpretation

While Fixed B=48 has higher QPS than Learned Policy:
- It's **not a fair comparison** because recall is 2.1% lower
- If user can tolerate 2.1% recall loss, they could use Fixed B=48
- Learned Policy is for users who want QPS improvement **without sacrificing recall**

**✅ Learned Policy is the right solution for recall-sensitive applications.**

---

## Multi-run Test Details

### Test Configuration
- Runs: 3x baseline, 3x learned (interleaved)
- SearchThreadNum=8
- InternalResultNum=128
- LearnedBudgetThreshold=0.80

### QPS Results

| Run | Baseline QPS | Learned QPS |
|-----|--------------|-------------|
| 1 | 5,305.04 | 5,970.15 |
| 2 | 5,310.67 | 5,952.38 |
| 3 | 5,302.23 | 5,995.20 |
| **Mean** | **5,305.98** | **5,972.58** |
| **Std** | 3.51 | 17.57 |

**QPS Improvement: +12.56%**

### P99 Latency Results

| Run | Baseline P99 (ms) | Learned P99 (ms) |
|-----|-------------------|------------------|
| 1 | 1.958 | 1.912 |
| 2 | 1.909 | 1.929 |
| 3 | 1.938 | 1.905 |
| **Mean** | **1.935** | **1.915** |

**P99 Change: -1.0%** (slight improvement, within noise)

### Pages/Query Results

| Run | Baseline Pages | Learned Pages | Saving |
|-----|----------------|---------------|--------|
| 1 | 125.9 | 98.1 | 22.1% |
| 2 | 125.9 | 98.2 | 22.1% |
| 3 | 125.9 | 98.2 | 22.1% |
| **Mean** | **125.9** | **98.1** | **22.1%** |

---

## Budget Distribution

### Learned Policy (threshold=0.80)
| Budget | Percentage |
|--------|------------|
| B=32 | 26.9% |
| B=40 | 12.7% |
| B=48 | 23.9% |
| B=64 | 36.4% |
| B=128 | 0.1% |

---

## Comparison with Single-run Result

| Metric | Single-run | Multi-run Mean | Delta |
|--------|------------|----------------|-------|
| QPS Improvement | +24.4% | **+12.56%** | -11.8pp |
| Pages Saving | 22.1% | 22.1% | 0% |
| Recall Delta | -0.00132 | -0.00131 | ~0 |

**Key Finding**: Single-run QPS improvement was inflated due to system variance. Multi-run test shows more realistic 12.56% improvement, which still exceeds the 10% target.

---

## Productize Criteria

| Criteria | Target | Actual | Status |
|----------|--------|--------|--------|
| QPS uplift | >= 10% | 12.56% | ✅ |
| Recall delta | <= 0.002 | 0.00131 | ✅ |
| P99 increase | <= 5% | -1.0% | ✅ |
| Low-recall ratio | not worse | same | ✅ |
| Config default off | required | ✅ | ✅ |
| Policy overhead | < 1% | ~0.6-0.7% | ✅ |

**Overall**: 6/6 criteria met.

---

## Files

### Model Files
- `results/adaptive_budget/sift10m_learned/risk_model_b32.json`
- `results/adaptive_budget/sift10m_learned/risk_model_b40.json`
- `results/adaptive_budget/sift10m_learned/risk_model_b48.json`
- `results/adaptive_budget/sift10m_learned/feature_cols.json`

### Test Results
- `results/adaptive_budget/sift10m_multi_run/baseline_run{1,2,3}.csv`
- `results/adaptive_budget/sift10m_multi_run/learned_run{1,2,3}.csv`
- `results/adaptive_budget/sift10m_multi_run/baseline_run{1,2,3}.log`
- `results/adaptive_budget/sift10m_multi_run/learned_run{1,2,3}.log`

---

## Conclusion

SIFT10M Learned Policy 验证通过：
- **12.56% QPS 提升** (超过 10% 目标)
- **22.1% Pages 节省**
- **P99 Latency 保持** (-1.0% 变化)
- **Recall 保持** (delta = 0.00131)

**Recommendation**: 满足所有产品化标准，可考虑在 SIFT10M 规模数据集上启用。
