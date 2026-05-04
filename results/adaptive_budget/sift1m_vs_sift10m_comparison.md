# SIFT1M vs SIFT10M Oracle Comparison

**Date**: 2026-05-03
**Status**: 结论在两个数据集上均成立

---

## Summary Comparison

| Metric | SIFT1M | SIFT10M | Trend |
|--------|--------|---------|-------|
| **min_B <= 40** | 81.6% | 71.0% | ↓ |
| **min_B <= 48** | 88.8% | 81.8% | ↓ |
| **Oracle saving** | 50.8% | 42.2% | ↓ |
| **Baseline recall** | 0.9786 | 0.9491 | ↓ |

---

## min_B Distribution Comparison

### SIFT1M

| min_B | Count | % | Cum |
|-------|-------|---|-----|
| 16 | 4,353 | 43.5% | 43.5% |
| 32 | 2,866 | 28.7% | 72.2% |
| 40 | 937 | 9.4% | 81.6% |
| 48 | 719 | 7.2% | 88.8% |
| 64 | 1,125 | 11.2% | 100% |

### SIFT10M

| min_B | Count | % | Cum |
|-------|-------|---|-----|
| 16 | 3,025 | 30.2% | 30.2% |
| 32 | 2,887 | 28.9% | 59.1% |
| 40 | 1,193 | 11.9% | 71.0% |
| 48 | 1,074 | 10.7% | 81.8% |
| 64 | 1,821 | 18.2% | 100% |

---

## Budget Sweep Comparison

### SIFT1M

| B | Recall | QPS | Pages |
|---|--------|-----|-------|
| 16 | 0.8698 | 11834 | 30 |
| 32 | 0.9404 | 9074 | 60 |
| 48 | 0.9657 | 7236 | 90 |
| **64** | **0.9786** | **5828** | **119** |
| 128 | 0.9940 | 2979 | 236 |

### SIFT10M

| B | Recall | QPS | Pages |
|---|--------|-----|-------|
| 16 | 0.8000 | 11050 | 32 |
| 32 | 0.8898 | 8251 | 63 |
| 48 | 0.9279 | 6702 | 95 |
| **64** | **0.9491** | **5459** | **126** |
| 128 | 0.9804 | 2787 | 250 |

---

## Key Observations

### 1. 结论在两个数据集上均成立 ✅

| Criterion | Threshold | SIFT1M | SIFT10M |
|-----------|-----------|--------|---------|
| >=30% min_B <= 40 | 30% | **81.6%** ✅ | **71.0%** ✅ |
| >=50% min_B <= 48 | 50% | **88.8%** ✅ | **81.8%** ✅ |
| Oracle saving >= 15% | 15% | **50.8%** ✅ | **42.2%** ✅ |

### 2. SIFT10M 的 easy query 比例较低

- SIFT1M: 43.5% queries need only B=16
- SIFT10M: 30.2% queries need only B=16
- 差异: SIFT10M 的 routing 更分散，需要更多 postings

### 3. SIFT10M 的 baseline recall 较低

- SIFT1M: 0.9786
- SIFT10M: 0.9491
- 原因: SIFT10M 规模更大，routing 难度更高

### 4. Oracle saving 都很显著

- SIFT1M: 50.8% pages saving
- SIFT10M: 42.2% pages saving
- 都远超 fixed B=48 的 24.8% saving

---

## 规模敏感性分析

| 规模比 | SIFT10M/SIFT1M |
|--------|----------------|
| 向量数 | 10x |
| min_B<=16 比例 | 0.69x (30.2%/43.5%) |
| Oracle saving | 0.83x (42.2%/50.8%) |
| Baseline recall | 0.97x (0.949/0.979) |

**结论**: Adaptive budget 在更大规模数据集上仍然有效，但效果略减。这符合预期：
- 规模越大，routing 越分散
- 需要更多 postings 来覆盖相同比例的近邻
- 但 oracle saving 仍然显著 (42%)

---

## Decision

**结论在 SIFT10M 上验证通过**，可以进入 Phase 2 (feature extraction)。

需要关注：
1. Feature-based rule 需要在两个数据集上都有效
2. 可能需要数据集相关的参数调整
3. 建议在 SIFT10M 上也采集 head distance trace
