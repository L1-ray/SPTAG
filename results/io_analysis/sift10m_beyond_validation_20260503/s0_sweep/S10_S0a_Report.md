# S10-S0a: Baseline SearchThreadNum Sweep Results

**Date**: 2026-05-03
**Dataset**: SIFT10M UInt8 + DEFAULT
**Configuration**: nt=16, ir=64, pl=4, cache=off

## Executive Summary

**SIFT10M QPS 在 st=8 达到平台期**，st=8/12/16 QPS 几乎相同（~5378-5381），与 SIFT1M 不同。

## QPS Results

| st | QPS Run 1 | QPS Run 2 | QPS Run 3 | QPS Avg | QPS Std | Recall@10 |
|----|-----------|-----------|-----------|---------|---------|-----------|
| 4 | 3303.60 | 3295.98 | 3282.99 | **3294.19** | 8.51 | 0.949144 |
| 8 | 5405.41 | 5420.05 | 5307.86 | **5377.77** | 49.80 | 0.949144 |
| 12 | 5385.03 | 5376.34 | 5382.13 | **5381.17** | 3.61 | 0.949144 |
| 16 | 5373.46 | 5379.24 | 5387.93 | **5380.21** | 5.95 | 0.949144 |

## Key Metrics (st=8)

| Metric | Avg | P50 | P90 | P95 | P99 | P99.9 | Max |
|--------|-----|-----|-----|-----|-----|-------|-----|
| Requested Bytes/Query | 515 KB | 524 KB | 600 KB | 620 KB | 648 KB | 684 KB | 720 KB |
| Pages Read/Query | 125.9 | 131 | 150 | 155 | 162 | 171 | 180 |
| Postings Touched/Query | 64.0 | 64 | 64 | 64 | 64 | 64 | 64 |
| Duplicate Vector Read Ratio | 0.120 | 0.108 | 0.198 | 0.231 | 0.289 | 0.362 | 0.403 |
| Distance Eval Ratio | 0.880 | 0.892 | 0.942 | 0.951 | 0.964 | 0.976 | 0.982 |
| Total Latency (ms) | 1.478 | 1.464 | 1.689 | 1.752 | 1.930 | 5.053 | 21.576 |
| Batch Read Total (ms) | 1.009 | 1.003 | 1.232 | 1.291 | 1.434 | 4.213 | 6.889 |

## Comparison with Plan Baseline

| Metric | Plan Baseline | S10-S0a Result | Match |
|--------|---------------|----------------|-------|
| QPS | 5608.52 | ~5378 | Close (4% diff) |
| Recall@10 | 0.949144 | 0.949144 | ✓ |
| Bytes/Query | 515772 | 515772 | ✓ |
| Pages/Query | ~125.9 | 125.9 | ✓ |
| Duplicate Ratio | 0.1195 | 0.120 | ✓ |

**Note**: QPS 略低于计划文档中的 5608.52，可能是 run-to-run 波动或环境差异。

## Key Findings

### 1. QPS Plateau at st=8

- **st=4 → st=8**: QPS 提升 63% (3294 → 5378)
- **st=8 → st=12**: QPS 几乎不变 (5378 → 5381, +0.06%)
- **st=12 → st=16**: QPS 不变 (5381 → 5380)

这与 SIFT1M 不同：
- **SIFT1M**: st=16 比 st=8 有明显提升（M1 测试中 st=16 达到 5835 QPS）
- **SIFT10M**: st=8 已经达到吞吐极限

### 2. Recall Stability

所有配置 Recall@10 = 0.949144，稳定且符合预期。

### 3. I/O Pattern

- Postings Touched/Query = 64（固定，由 InternalResultNum=64 决定）
- Duplicate Vector Read Ratio ~12%（比 SIFT1M 的 ~16% 略低）
- Pages/Query ~126（比 SIFT1M 的 ~119 略高）

### 4. Latency Distribution

- Avg latency ~1.48ms
- P99 ~1.93ms
- P99.9 ~5.05ms（有长尾）

## Implications for M1/M2-H/M4

### M1 Page Cache

- **结论**: SIFT10M st=8 已达 QPS 平台期，高并发 (st>8) 不会有更多收益
- **建议**: M1 应重点验证 st=8 的 cache 效果，而非 st=16
- **预期**: 如果 cache 有效，st=8 应该能超过 baseline

### M2-H Selective Hybrid

- Duplicate ratio ~12%（比 SIFT1M 的 ~16% 更低）
- 需要运行 posting trace 确认 I/O wait 分布

### M4 Primary-Secondary Dedupe

- Duplicate ratio ~12% 远低于 30% 阈值
- 可能 M4 在 SIFT10M 也不可行

## Next Steps

1. **S10-S0b**: 可跳过，因为 st=8 已达平台期
2. **S10-S0c**: 运行 ir=32/64/96 建立 recall/QPS Pareto
3. **S10-M1**: 重点验证 st=8 的 cache 效果
4. **S10-M2H**: 运行 posting trace
5. **S10-M4**: 运行 pre-dedupe trace

## Files

- Logs: `results/io_analysis/sift10m_beyond_validation_20260503/s0_sweep/s10_s0a_st*_run*.log`
- Configs: `results/io_analysis/sift10m_beyond_validation_20260503/s0_sweep/s10_s0a_st*_run*.ini`
- Query I/O Stats: `results/io_analysis/sift10m_beyond_validation_20260503/s0_sweep/query_io_stats_st*_run*.csv`
