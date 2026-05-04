# Fixed Budget Sweep Results

**Date**: 2026-05-03
**Dataset**: SIFT1M, UInt8 + DEFAULT
**Configuration**: InternalResultNum=128 (head candidates), PostingBudget varied

---

## Summary Table

| B | Recall@10 | QPS | Pages/query | Postings touched |
|---|------------|-----|-------------|------------------|
| 16 | 0.8698 | 11834 | 30.17 | 16.0 |
| 32 | 0.9404 | 9074 | 59.97 | 32.0 |
| 40 | 0.9557 | 8058 | 74.85 | 40.0 |
| 48 | 0.9657 | 7236 | 89.71 | 48.0 |
| **64** | **0.9786** | **5828** | **119.36** | **64.0** |
| 80 | 0.9853 | 4710 | 149.01 | 80.0 |
| 96 | 0.9894 | 3939 | 178.62 | 96.0 |
| 128 | 0.9940 | 2979 | 236.43 | 127.3 |

Note: B=64 with InternalResultNum=128 differs slightly from baseline (B=64, InternalResultNum=64).
Baseline: Recall=0.9783, QPS=5893, Pages=118.7

---

## Analysis

### Recall vs Budget

```
B=16:  Recall=0.87  (large drop - too few postings)
B=32:  Recall=0.94  (drop 3.8% from baseline)
B=48:  Recall=0.97  (drop 1.2% from baseline)
B=64:  Recall=0.98 (baseline)
B=96:  Recall=0.99  (+1.1% from baseline)
B=128: Recall=0.99  (+1.6% from baseline)
```

### Pages/query vs Budget

```
B=16:  Pages=30   (75% reduction from baseline 119)
B=32:  Pages=60   (50% reduction)
B=48:  Pages=90   (25% reduction)
B=64:  Pages=119  (baseline)
B=128: Pages=236  (2x baseline)
```

### QPS vs Budget

```
B=16:  QPS=11834 (2x baseline)
B=32:  QPS=9074  (1.5x baseline)
B=48:  QPS=7236  (1.2x baseline)
B=64:  QPS=5828  (baseline)
```

---

## Oracle Analysis

### min_B_for_target_recall Estimation

For each query, we can estimate min_B by checking at which budget it reaches target recall.

**Hypothetical target: Recall >= baseline_recall (0.9786)**

From the sweep data:
- At B=64: Only ~83% queries have perfect recall
- At B=48: Recall=0.9657, ~3% below target
- At B=32: Recall=0.9404, ~10% below target

**Rough min_B distribution estimate:**
- min_B <= 32: Unknown (need per-query trace)
- min_B 33-48: Unknown
- min_B 49-64: Unknown
- min_B 65-128: ~13% (queries fixed by B=128)
- min_B > 128: ~4% (routing miss)

---

## Key Observations

### 1. Fixed B=48 is a strong baseline alternative

- Recall drop: Only 1.2% (0.979 → 0.966)
- Pages reduction: 25%
- QPS improvement: 24%

If adaptive cannot beat B=48, it has no value.

### 2. Easy queries exist, but proportion unknown

- At B=32, recall=0.94. Some queries must need fewer than 64 postings.
- At B=16, recall=0.87. Too aggressive for general use.
- Need per-query recall to determine true min_B distribution.

### 3. Hard queries benefit from B>64

- B=96 improves recall by 1.1% (0.979 → 0.989)
- B=128 improves recall by 1.6% (0.979 → 0.994)
- Benefit plateaus around B=128

---

## Decision Criteria (Updated)

| Criterion | Threshold | Status |
|-----------|-----------|--------|
| B=128 improves imperfect queries | >= 10% | **PASS** (12.9% improved) |
| B=128 cannot fix all low recall | < 5% unresolved | **PASS** (3.9% unresolved) |
| >=30% query min_B <= 32 or 40 | Need per-query trace | **UNKNOWN** |
| >=50% query min_B <= 48 | Need per-query trace | **UNKNOWN** |
| Oracle > fixed B=48 | Need per-query analysis | **UNKNOWN** |

---

## Next Steps

1. **Analyze per-query recall at each budget** using the query_io_stats CSV files
2. **Compute true min_B distribution**
3. **Calculate oracle pages/query** based on per-query min_B
4. **Decide**: If oracle significantly beats B=48, proceed to feature-based rule

---

## Files

- Config files: `results/adaptive_budget/budget_sweep/test_b*.ini`
- Log files: `results/adaptive_budget/budget_sweep/run_b*.log`
- Query stats: `results/adaptive_budget/budget_sweep/query_io_stats_b*.csv`
