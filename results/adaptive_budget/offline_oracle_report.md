# Offline Oracle Analysis - Final Report

**Date**: 2026-05-03
**Dataset**: SIFT1M, UInt8 + DEFAULT
**Status**: Phase 1 Oracle **PASS**

---

## Executive Summary

**Oracle Result: Strong PASS** - Adaptive budget has significant potential.

| Criterion | Threshold | Result | Status |
|-----------|-----------|--------|--------|
| >=30% query min_B <= 40 | 30% | **81.6%** | ✅ **PASS** |
| >=50% query min_B <= 48 | 50% | **88.8%** | ✅ **PASS** |
| Oracle pages saving | >=15% | **50.8%** | ✅ **PASS** |

---

## min_B Distribution

| min_B | Count | % | Cumulative |
|-------|-------|---|------------|
| **16** | 4,353 | **43.5%** | 43.5% |
| **32** | 2,866 | **28.7%** | 72.2% |
| **40** | 937 | 9.4% | 81.6% |
| **48** | 719 | 7.2% | 88.8% |
| 64 | 1,125 | 11.2% | 100% |
| >64 | 0 | 0% | 100% |

**Key insight**: 
- **72.2%** queries need B <= 32 to match baseline recall
- **88.8%** queries need B <= 48 to match baseline recall
- Only **11.2%** queries actually need B=64

---

## Oracle vs Fixed Budget Comparison

| Strategy | Pages/query | Saving | Avg Recall |
|----------|-------------|--------|------------|
| Baseline (B=64) | 119.36 | 0% | 0.9786 |
| **Oracle** | **58.78** | **50.8%** | **0.9786** |
| Fixed B=48 | 89.70 | 24.8% | 0.9653 |
| Fixed B=32 | 59.97 | 49.8% | 0.9404 |

**Critical finding**:
- Oracle saves **50.8%** pages (twice as much as fixed B=48's 24.8%)
- Fixed B=32 matches oracle pages but loses 3.8% recall
- Adaptive can potentially save 50% pages with NO recall loss

---

## Per-Budget Performance

| B | Recall | QPS | Pages | Postings |
|---|--------|-----|-------|----------|
| 16 | 0.8698 | 11834 | 30 | 16 |
| 32 | 0.9404 | 9074 | 60 | 32 |
| 40 | 0.9557 | 8058 | 75 | 40 |
| 48 | 0.9657 | 7236 | 90 | 48 |
| **64** | **0.9786** | **5828** | **119** | **64** |
| 80 | 0.9853 | 4710 | 149 | 80 |
| 96 | 0.9894 | 3939 | 179 | 96 |
| 128 | 0.9940 | 2979 | 236 | 127 |

---

## Conclusion

### Oracle demonstrates significant adaptive budget potential

1. **72.2% queries** can use B <= 32 (saving ~50% pages)
2. **Oracle pages/query = 58.78** (50.8% saving over baseline 119)
3. **Fixed B=48 only saves 24.8%**, far below oracle

### Next Steps

1. ✅ **Phase 1 Oracle: PASS** - Proceed to Phase 2
2. **Phase 2**: Collect head distance trace (margin_16/32/64, entropy)
3. **Phase 3**: Develop feature-based rule to predict min_B
4. **Phase 4**: Only if rule-based works, consider learned model

### Open Questions

- Can head distance margin/entropy predict query difficulty?
- What is the cost of wrong prediction (over-budget vs under-budget)?
- Should we implement conservative fallback to B=64?

---

## Files

- Budget sweep configs: `results/adaptive_budget/budget_sweep/test_b*.ini`
- Budget sweep logs: `results/adaptive_budget/budget_sweep/run_b*.log`
- Query stats: `results/adaptive_budget/budget_sweep/query_io_stats_b*.csv`
- Analysis script: `scripts/compute_min_b.py`
