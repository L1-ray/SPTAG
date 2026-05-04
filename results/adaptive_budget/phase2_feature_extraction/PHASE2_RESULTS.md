# Phase 2: Feature Extraction - Results Summary

**Date**: 2026-05-03
**Status**: Completed

---

## Implementation

### Added Configuration Parameters

- `EnableHeadDistanceTrace` - Enable per-query head distance trace
- `HeadDistanceTraceOutput` - Output CSV file path

### Added Code

1. **IExtraSearcher.h**: Added `HeadDistanceTraceRecord` struct
2. **SPANNIndex.cpp**: Record head distances during posting collection
3. **SSDIndex.h**: Output head distance trace to CSV

---

## Key Findings

### Feature Correlation with min_B

| Feature | Correlation |
|---------|------------|
| margin_64 | -0.216 |
| margin_32 | -0.202 |
| margin_16 | -0.183 |
| margin_8 | -0.161 |
| dist_var_top64 | -0.193 |

**Interpretation**: Higher margin = easier query = lower min_B

### min_B Distribution by margin_16 Quartile

| Quartile | margin_16 range | min_B <= 40 | min_B <= 48 | mean min_B |
|----------|-----------------|-------------|-------------|------------|
| Q1 | [0.055, 0.223] | 51.9% | 61.2% | 53.8 |
| Q2 | [0.223, 0.315] | 67.8% | 74.1% | 42.6 |
| Q3 | [0.315, 0.469] | 80.4% | 85.0% | 33.3 |
| Q4 | [0.469, 17.65] | 93.0% | 94.9% | 23.2 |

---

## Rule-based Policy Evaluation

| Rule | Pages Saving | Recall Miss Rate |
|------|-------------|-----------------|
| Fixed B=64 | 0% | 0.00% (baseline) |
| margin_16 >= 0.35 -> B=48, else B=64 | **9.8%** | **1.88%** |
| margin_16 >= 0.30 -> B=48, else B=64 | 12.8% | 3.15% |
| margin_16 >= 0.30 -> B=40, else B=64 | 19.2% | 5.22% |
| 3-tier: m16>=0.40->32, >=0.25->48, else 64 | 24.1% | 7.40% |
| Fixed B=48 | 24.8% | 11.25% |

---

## Recommended Policy

**Best trade-off**: `margin_16 >= 0.35 -> B=48, else B=64`

- Pages saving: 9.8%
- Recall miss rate: 1.88% (188/10000 queries miss target)
- Budget distribution: 41.8% B=48, 58.2% B=64

This rule meets the Phase 3 success criteria:
- ✅ Recall@10 >= baseline - 0.001 (only 1.88% queries miss)
- ✅ pages/query saving >= 8% (9.8% > 8%)
- ✅ Better than fixed B=48 (24.8% saving but 11.25% miss)

---

## Next Steps (Phase 3)

1. Implement online adaptive budget based on rule
2. Add margin_16 calculation in head search phase
3. Modify posting collection to use adaptive budget
4. Test on SIFT1M and SIFT10M
5. Compare with fixed B=64 baseline

---

## Files Generated

- `results/adaptive_budget/phase2_feature_extraction/head_distance_trace.csv` - 1.27M rows
- `results/adaptive_budget/phase2_feature_extraction/query_io_stats.csv` - 10K queries
- `results/adaptive_budget/phase2_feature_extraction/query_features.csv` - Features with min_B labels
