# SIFT1M Timing Test Results

**Test Date**: 2026-05-05

**Configuration** (aligned with SPANN_Query_Aware_Adaptive_Posting_Budget_Plan_20260503.md):
- SearchThreadNum=8
- NumberOfThreads=16
- InternalResultNum=64
- SearchPostingPageLimit=4
- MaxDistRatio=1000000

## Wall Clock Time Comparison

| Mode | Start Time | End Time | Wall Time | User Time | Sys Time |
|------|------------|----------|-----------|-----------|----------|
| **Baseline** | - | - | **2.381s** | 8.928s | 1.009s |
| **A3 (Learned Policy)** | - | - | **2.110s** | 8.195s | 0.813s |

**Wall Time Improvement**: 2.110s vs 2.381s = **-11.4%** (A3 faster)

## QPS Comparison

| Mode | Actual QPS | Finish Sending Time |
|------|------------|---------------------|
| **Baseline** | 5,692 | 1.757s |
| **A3 (Learned Policy)** | 6,770 | 1.477s |

**QPS Improvement**: 6,770 vs 5,692 = **+19.0%**

## Recall Comparison

| Mode | Recall@10 | MRR@10 |
|------|-----------|--------|
| **Baseline** | 0.978319 | 1.000000 |
| **A3 (Learned Policy)** | 0.976431 | 1.000000 |

**Recall Delta**: -0.001888 (within 0.002 threshold)

## I/O Statistics Comparison

| Metric | Baseline | A3 (Learned) | Delta |
|--------|----------|--------------|-------|
| **Postings touched** | 63.65 | 48.26 | **-24.2%** |
| **Posting saving** | - | - | **24.2%** |

## Latency Distribution Comparison (ms)

| Metric | Baseline | A3 (Learned) | Delta |
|--------|----------|--------------|-------|
| P50 Postings | 64 | 48 | -25% |
| P90 Postings | 64 | 56 | -12.5% |
| P95 Postings | 64 | 64 | 0% |
| P99 Postings | 64 | 64 | 0% |

## A3 Configuration Details

A3 uses tiered thresholds for different budgets:
- B=32: threshold 0.98
- B=40: threshold 0.96
- B=48: threshold 0.93
- B=56: threshold 0.88
- Default: B=64

This allows more aggressive budget reduction for easier queries while maintaining safety for harder queries.

## Summary

| Metric | Value |
|--------|-------|
| **Wall Time Improvement** | 11.4% |
| **QPS Improvement** | **19.0%** |
| **Postings Reduction** | **24.2%** |
| **Recall Delta** | -0.00189 (acceptable) |

The A3 learned policy with tiered thresholds achieves:
1. **19.0% QPS improvement** (5,692 → 6,770)
2. **24.2% postings reduction** (63.65 → 48.26)
3. **Acceptable recall loss** (-0.00189, within 0.002 threshold)

## Previous Learned Policy Comparison

| Policy | QPS | Recall | Postings | QPS Improvement |
|--------|-----|--------|----------|-----------------|
| Baseline | 5,692 | 0.9783 | 63.65 | - |
| Learned (single threshold) | 6,402 | 0.9775 | ~51 | +12.5% |
| **A3 (tiered thresholds)** | **6,770** | 0.9764 | 48.26 | **+19.0%** |

**Key insight**: Tiered thresholds provide significantly better QPS improvement (+19.0%) compared to single threshold (+12.5%).
