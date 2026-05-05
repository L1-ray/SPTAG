# SIFT10M Timing Test Results

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
| **Baseline** | - | - | **4.624s** | 10.101s | 2.758s |
| **A3 (Learned Policy)** | - | - | **3.942s** | 9.374s | 2.301s |

**Wall Time Improvement**: 3.942s vs 4.624s = **-14.8%** (A3 faster)

## QPS Comparison

| Mode | Actual QPS | Finish Sending Time |
|------|------------|---------------------|
| **Baseline** | 5,382 | 1.858s |
| **A3 (Learned Policy)** | 5,838 | 1.713s |

**QPS Improvement**: 5,838 vs 5,382 = **+8.5%**

## Recall Comparison

| Mode | Recall@10 | MRR@10 |
|------|-----------|--------|
| **Baseline** | 0.949144 | 1.000000 |
| **A3 (Learned Policy)** | 0.946884 | 1.000000 |

**Recall Delta**: -0.002260 (within 0.003 threshold)

## I/O Statistics Comparison

| Metric | Baseline | A3 (Learned) | Delta |
|--------|----------|--------------|-------|
| **Postings touched** | 64.00 | 50.29 | **-21.4%** |
| **Posting saving** | - | - | **21.4%** |

## Latency Distribution Comparison (ms)

| Metric | Baseline | A3 (Learned) | Delta |
|--------|----------|--------------|-------|
| P50 Postings | 64 | 56 | -12.5% |
| P90 Postings | 64 | 64 | 0% |
| P95 Postings | 64 | 64 | 0% |
| P99 Postings | 64 | 64 | 0% |

## A3 Configuration Details

A3 uses tiered thresholds for different budgets:
- B=32: threshold 0.95
- B=40: threshold 0.92
- B=48: threshold 0.88
- B=56: threshold 0.82
- Default: B=64

Note: SIFT10M uses lower thresholds than SIFT1M due to different data characteristics.

## Summary

| Metric | Value |
|--------|-------|
| **Wall Time Improvement** | 14.8% |
| **QPS Improvement** | **8.5%** |
| **Postings Reduction** | **21.4%** |
| **Recall Delta** | -0.00226 (acceptable) |

The A3 learned policy with tiered thresholds achieves:
1. **8.5% QPS improvement** (5,382 → 5,838)
2. **21.4% postings reduction** (64.00 → 50.29)
3. **Acceptable recall loss** (-0.00226, within tolerance)

## Previous Learned Policy Comparison

| Policy | QPS | Recall | Postings | QPS Improvement |
|--------|-----|--------|----------|-----------------|
| Baseline | 5,382 | 0.9491 | 64.00 | - |
| Learned (single threshold) | 5,824 | 0.9472 | ~51 | +8.2% |
| **A3 (tiered thresholds)** | **5,838** | 0.9469 | 50.29 | **+8.5%** |

**Key insight**: A3 (tiered thresholds) provides similar QPS improvement to single threshold, but with more aggressive postings reduction.

## Comparison with SIFT1M Results

| Metric | SIFT1M | SIFT10M |
|--------|--------|---------|
| **Wall Time Improvement** | 11.4% | 14.8% |
| **QPS Improvement** | **19.0%** | **8.5%** |
| **Postings Reduction** | 24.2% | 21.4% |
| **Recall Delta** | -0.00189 | -0.00226 |
| **A3 Thresholds** | 0.98/0.96/0.93/0.88 | 0.95/0.92/0.88/0.82 |

**Key Observations**:
1. SIFT1M shows better QPS improvement (19.0%) than SIFT10M (8.5%)
2. Both datasets show significant postings reduction (~21-24%)
3. SIFT10M uses more aggressive thresholds to achieve similar postings reduction
4. Recall delta is within acceptable range for both datasets
