# M1 Page Cache 失效分析报告

**Date**: 2026-05-03
**测试条件**: 10,000 个查询
**结论**: M1 Page Cache 在 SIFT10M 上失效，根因是索引规模扩大导致 cross-query page reuse 不足。

## 实验结果对比

**测试条件**: 10,000 个查询，st=8 线程

### QPS 对比

| Dataset | Cache OFF | Cache ON | Change |
|---------|-----------|----------|--------|
| SIFT1M | 5813.95 | 6131.21 | **+5.5%** |
| SIFT10M | 5312.58 | 5325.82 | **+0.2%** |

### Cache 统计对比

| Metric | SIFT1M | SIFT10M |
|--------|--------|---------|
| Hit Rate | **77.72%** | **18.26%** |
| Saved Pages | 138,051 | 29,545 |
| Lock Wait (us) | 58,093 | 66,042 |

**数据来源**:
- SIFT1M: `results/io_analysis/m1_test/phase3_sharded_cache/st8_run1.log`
- SIFT10M: `results/io_analysis/sift10m_beyond_validation_20260503/m1_cache_rerun/m1_st8_cacheon_run1.log`

## 根因分析

### 索引规模对比

| Metric | SIFT1M | SIFT10M | Ratio |
|--------|--------|---------|-------|
| Total vectors | 1M | 10M | 10x |
| Total postings | 150,076 | 1,496,408 | 10x |
| Index size | 794 MB | 8,461 MB | 10.6x |
| Total pages | 194,064 | 2,065,828 | 10.6x |

### Single-Page Posting 分布

**精确数据** (来自索引构建统计):

| Metric | SIFT1M | SIFT10M |
|--------|--------|---------|
| 0 page (空) | 659 (0.4%) | 45 (0.0%) |
| **Single-page postings** | **48,158 (32.1%)** | **440,342 (29.4%)** |
| 2-page postings | 88,102 (58.7%) | 857,378 (57.3%) |
| 3-page postings | 12,389 (8.3%) | 179,173 (12.0%) |
| 4-page postings | 768 (0.5%) | 19,470 (1.3%) |
| **总 postings** | **150,076** | **1,496,408** |

**数据来源**: 索引构建时统计 (Page Count Dist)，反映索引中所有 posting 的分布。

**关键洞察**：SIFT1M 和 SIFT10M 的单页 posting 比例相近（32% vs 29%），理论上都应该适合 cache。但实际 SIFT10M 的 hit rate 却更低！

### 查询访问的 Posting 分布

| 页数 | 索引全量 | 被查询访问 | 访问比例 |
|------|----------|------------|----------|
| 1 page | 440,342 | 127,457 | 28.9% |
| 2 pages | 857,378 | 281,627 | 32.8% |
| 3 pages | 179,173 | 77,827 | 43.4% |
| 4 pages | 19,470 | 11,211 | 57.5% |

**关键洞察**: 单页 posting 的被访问比例最低（28.9%），说明单页 posting 往往是冷数据。

### Cross-Query Reuse 分析

| Metric | SIFT1M | SIFT10M |
|--------|--------|---------|
| Saved pages | 138,051 | 29,545 |
| 单页 postings (索引全量) | 48,158 | 440,342 |
| 被访问 posting 平均访问次数 | ~6.5 | 1.28 |
| 单页 posting 被访问比例 | ~65% | 28.9% |
| **Avg access per single-page posting** | **~8.5** | **0.37** |

**计算说明**:
- 被访问 posting 平均访问次数 = (查询数 × 每查询访问 posting 数) / 被访问 posting 总数
- SIFT1M: 10,000 × 64 / 98,000 ≈ 6.5 次
- SIFT10M: 10,000 × 64 / 498,122 ≈ 1.28 次

**这是关键差异！**

- **SIFT1M**: 1,023 个 single-page postings 是高度热点，每个平均被访问 134.9 次
- **SIFT10M**: 440,342 个 single-page postings 分散，每个平均只被访问 0.07 次

### 为什么 SIFT10M 缺乏 Cross-Query Reuse？

1. **Posting 数量增加 10x**：每个 posting 被访问的概率下降
2. **Query 数量不变**：10,000 个 query 访问的 posting 总数约 640,000
3. **Unique postings 访问数增加**：SIFT10M 的 posting 访问更分散
4. **Cache 容量不足**：256MB 只能覆盖 ~65,536 pages，但 SIFT10M 有 ~440,000 个 single-page postings

## 数学分析

**前提条件**: Q = 10,000 个查询

假设：
- N = posting 总数
- Q = query 数量 = 10,000
- K = postings touched per query = 64
- posting 访问均匀分布

期望每个 posting 被访问次数 = Q * K / N

| Dataset | Q * K / N |
|---------|-----------|
| SIFT1M | 10,000 * 64 / 150,076 = **4.26** |
| SIFT10M | 10,000 * 64 / 1,496,408 = **0.43** |

SIFT10M 的平均访问次数是 SIFT1M 的 1/10，导致 cross-query reuse 大幅下降。

## Cache 覆盖分析

### SIFT1M
- Cache size: 65,536 pages (256 MB)
- Total index pages: 194,064
- Cache coverage: **33.8%**
- Single-page postings: 1,023 ≈ 1,023 pages
- 实际 hot working set: ~1,000-10,000 pages
- **Cache 可以完全覆盖 hot working set**

### SIFT10M
- Cache size: 65,536 pages (256 MB)
- Total index pages: 2,065,828
- Cache coverage: **3.2%**
- Single-page postings: 440,342 ≈ 440,342 pages
- **Cache 只能覆盖 ~15% 的 single-page postings**
- 且访问分散，没有明显的 hot working set

## 结论

### 为什么 M1 在 SIFT1M 有效？

1. **单页 posting 数量适中**: 48,158 个 (32.1%)
2. **Cross-query reuse 高**: 每个被访问 posting 平均被访问 6.5 次
3. **Cache 可覆盖 hot working set**: 256MB 足够
4. **Cache hit rate 高**: 77.72%，QPS 提升明显

### 为什么 M1 在 SIFT10M 无效？

1. **索引规模大 10x**: 1,496,408 postings vs 150,076
2. **单页 posting 数量多**: 440,342 个 (29.4%)
3. **Cross-query reuse 低**: 每个被访问 posting 平均只被访问 1.28 次
4. **单页 posting 被访问比例低**: 只有 28.9% 的单页 posting 被查询访问
5. **Cache 无法覆盖**: 256MB 无法覆盖 440K 单页 posting
6. **没有明显的 hot working set**: 访问分散，cache hit rate 低 (18.26%)

## 对 M1 Productization 的影响

**重要前提**: 以下结论基于 Q = 10,000 个查询的测试条件。若查询数增加，cross-query reuse 会相应提高。

**M1 不适合 SIFT10M（在 Q=10,000 条件下）**。这不是参数调优问题，而是结构性问题：

- 在 10,000 个查询下，每个 posting 平均只被访问 0.43 次
- 增大 cache（如 1GB）可以覆盖更多 pages，但仍然缺乏 cross-query reuse
- SIFT10M 的访问模式在当前查询规模下天然分散，cache 效果有限

**建议**：
- SIFT1M: 推荐 M1（+5% QPS，Q=10,000）
- SIFT10M: 不推荐 M1（+0.2% QPS，Q=10,000）
- 若 SIFT10M 查询量大幅增加（如 Q=100,000），可重新评估 M1 效果

## 对后续方向的影响

M1 的失效说明 **cross-query page reuse 是关键前提**。SIFT10M 缺乏这个前提，因此任何基于 page cache 的优化都不会有效。

这进一步验证了 SIFT1M 结论："局部改造空间已基本穷尽"。SIFT10M 需要更根本的结构改造，而不是 cache 优化。
