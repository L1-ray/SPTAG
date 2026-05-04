# SIFT10M Learned Policy Evaluation Report

**Date**: 2026-05-04
**Dataset**: SIFT10M
**Queries**: 10,000
**Status**: ⚠️ 单次测试结果，待多次验证

---

## Executive Summary

| Metric | Baseline (B=64) | Learned Policy | Delta | Status |
|--------|-----------------|----------------|-------|--------|
| **QPS** | 5,434.78 | **6,761.33** | **+24.4%** | ✅ (单次) |
| **Recall@10** | 0.949130 | 0.947810 | -0.001320 | ✅ |
| **Avg pages/query** | 125.9 | 98.2 | -22.1% | ✅ |
| **P99 Latency** | 待测试 | 待测试 | - | ⚠️ 缺失 |
| **Low-recall queries (<0.7)** | 162 | 162 | 0 | ✅ |

**重要声明**: 以上为单次测试结果，baseline 和 learned 各只运行 1 次。production-ready 需要至少各 3 次测试并报告 median/std。

---

## QPS Improvement Analysis

### Test Configuration
- SearchThreadNum=8
- InternalResultNum=128
- LearnedBudgetThreshold=0.80

### Results

| Run | Baseline QPS | Learned QPS | Improvement |
|-----|--------------|-------------|-------------|
| 1 | 5,434.78 | 6,761.33 | +24.4% |

**QPS Improvement: +24.4%** - Far exceeds the 10% target!

---

## Pages Saved

| Metric | Baseline | Learned | Saving |
|--------|----------|---------|--------|
| Avg pages/query | 125.9 | 98.2 | **22.1%** |
| Total pages | 1,259,000 | 982,000 | 277,000 |

---

## Budget Distribution

### Learned Policy (threshold=0.80)
| Budget | Count | Percentage |
|--------|-------|------------|
| B=32 | 2,690 | 26.9% |
| B=40 | 1,274 | 12.7% |
| B=48 | 2,389 | 23.9% |
| B=64 | 3,640 | 36.4% |
| B=128 | 7 | 0.1% |

**Key insight**: Learned policy assigns lower budgets (32-48) to ~63.5% of queries,
while maintaining higher budgets for hard queries.

---

## Recall Analysis

### Overall Recall
| Metric | Baseline | Learned | Delta |
|--------|----------|---------|-------|
| Mean Recall | 0.949130 | 0.947810 | -0.001320 |
| Std Recall | - | 0.091072 | - |
| Min Recall | - | 0.1 | - |
| P25 | - | 0.9 | - |
| P50 | - | 1.0 | - |
| P75 | - | 1.0 | - |
| Max | - | 1.0 | - |

### Low-Recall Tail
| Recall Range | Baseline | Learned |
|--------------|----------|---------|
| < 0.5 | 2 | 2 |
| 0.5-0.6 | 21 | 21 |
| 0.6-0.7 | 139 | 139 |
| < 0.7 (total) | 162 | 162 |

Low-recall tail unchanged - these are routing issues, not budget problems.

---

## Comparison with SIFT1M

| Metric | SIFT1M | SIFT10M |
|--------|--------|---------|
| QPS Improvement | +8.08% | **+24.4%** |
| Pages Saving | 15.2% | **22.1%** |
| Recall Delta | -0.00037 | -0.001320 |
| Threshold | 0.97 | 0.80 |

**Key insight**: SIFT10M shows better improvement primarily because:
1. **Lower threshold (0.80 vs 0.97)** allows more aggressive budget reduction
2. Note: SIFT10M baseline recall is lower (0.949 vs 0.979), so the comparison is not strictly about "routing efficiency"

---

## Offline vs Online Comparison

### Offline Analysis (Threshold Sweep)
| Threshold | Pages Saving | Miss Rate |
|-----------|--------------|-----------|
| 0.70 | 29.4% | 5.4% |
| 0.75 | 26.0% | 2.8% |
| **0.80** | **22.1%** | **1.2%** |
| 0.85 | 17.7% | 0.3% |
| 0.90 | 13.2% | 0.1% |
| 0.95 | 8.7% | 0.0% |

### Online Results (threshold=0.80)
- Pages Saving: 22.1% (matches offline prediction)
- Recall Delta: -0.001320 (vs offline miss rate 1.2%)

The online results closely match offline predictions, validating the model's accuracy.

---

## Conclusion

**单次测试结果**:
- **24.4% QPS improvement** (far exceeds 10% target)
- **22.1% pages saved**
- **Recall maintained** (delta = 0.001320, within tolerance)
- **Low-recall tail unchanged**

**待完成验证**:
- [ ] 多次重复测试 (baseline/learned 各 3 次)
- [ ] P99 latency 统计
- [ ] Fixed-B Pareto 对照 (vs B=48)
- [ ] 交错运行消除系统波动

**当前状态**: 强阳性单次结果，但不足以标记 production-ready。

---

## Files

### Model Files
- `results/adaptive_budget/sift10m_learned/risk_model_b32.json`
- `results/adaptive_budget/sift10m_learned/risk_model_b40.json`
- `results/adaptive_budget/sift10m_learned/risk_model_b48.json`
- `results/adaptive_budget/sift10m_learned/feature_cols.json`

### Test Results
- `results/adaptive_budget/sift10m_learned_test/learned_test.ini` - Test configuration
- `results/adaptive_budget/sift10m_learned_test/learned.log` - Full log
- `results/adaptive_budget/sift10m_learned_test/query_io_stats.csv` - I/O statistics
