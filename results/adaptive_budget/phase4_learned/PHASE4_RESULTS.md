# Phase 4: Learned Policy Results

**Date**: 2026-05-03
**Dataset**: SIFT1M

---

## Results Summary

| Method | Pages Saving | Miss Rate | Status |
|--------|-------------|-----------|--------|
| Rule-based (margin_16 top 40%) | 9.1% | 1.8% | ⚠️ baseline |
| **Risk-control (threshold=0.97)** | **13.8%** | **1.1%** | ✅ BEST |

---

## Risk-control Method

**Approach**: For each budget B ∈ {32, 40, 48}, train a binary classifier to predict P(recall_at_B >= target). Choose minimum B where P(safe) >= threshold.

**Threshold Selection**: 0.97 provides best trade-off:
- High enough to minimize false positives (predicting safe when actually unsafe)
- Low enough to capture easy queries

**Budget Distribution** (threshold=0.97):
- B=32: 18.6%
- B=40: 8.2%
- B=48: 12.4%
- B=64: 60.8%

---

## Feature Importance (Risk model for B=32)

| Feature | Importance |
|---------|------------|
| margin_64 | 11510 |
| ratio_64 | 3855 |
| margin_16_32_ratio | 2981 |
| margin_4 | 2916 |
| slope_64_96 | 2599 |

**Key insight**: margin_64 is most important for predicting B=32 safety, not margin_16. This suggests the overall distance trend is more informative than a single margin.

---

## Comparison with Rule-based

| Metric | Rule-based | Risk-control | Improvement |
|--------|-----------|--------------|-------------|
| Pages saving | 9.1% | 13.8% | +4.7% |
| Miss rate | 1.8% | 1.1% | -0.7% |
| B=32 assignment | 0% | 18.6% | More aggressive |

**Why risk-control works better**:
1. Uses multiple features (not just margin_16)
2. Predicts safety probability, allowing nuanced decisions
3. Naturally handles uncertainty (threshold tuning)

---

## Files

- `results/adaptive_budget/phase4_learned/risk_model_b32.txt` - LightGBM model for B=32 safety
- `results/adaptive_budget/phase4_learned/risk_model_b40.txt` - LightGBM model for B=40 safety
- `results/adaptive_budget/phase4_learned/risk_model_b48.txt` - LightGBM model for B=48 safety
- `results/adaptive_budget/phase4_learned/train_data.csv` - Training data
- `results/adaptive_budget/phase4_learned/test_data.csv` - Test data
- `results/adaptive_budget/phase4_learned/feature_cols.json` - Feature columns

---

## Next Steps

1. **Online integration**: Implement risk-control policy in C++
2. **Inference optimization**: LightGBM inference is fast, but may need to optimize for latency
3. **Threshold tuning**: Default threshold=0.97, but may need adjustment for different workloads
