# M4-0 Oracle Report: Pre-Dedupe Trace Analysis

**Date**: 2026-05-03
**Dataset**: SIFT1M, UInt8 + DEFAULT
**Trace Source**: `results/m4_oracle/pre_dedupe_trace_st8.csv` (27.3M records, 10K queries)

## Executive Summary

**M4-0 Oracle Result: FAIL**

| Criterion | Threshold | Actual | Status |
|-----------|-----------|--------|--------|
| Duplicate payload bytes in query path | >= 30% | **15.3%** | **FAIL** |
| Primary pages/query reduction | >= 15% | **-959%** (worse than legacy) | **FAIL** |

**Recommendation**: Stop M4. Do not proceed to M4-1 sidecar build.

## Key Findings

### 1. Query-Level Duplicate Ratio is Much Lower Than Expected

**Previous M4 diagnosis (storage perspective)**:
- 99.7% VIDs appear in multiple postings globally
- Average replicas per VID: 6.41
- Storage reduction potential: 76.7%

**M4-0 Pre-dedupe trace (query perspective)**:
- Total records: 27,314,134 (pre-dedupe)
- Records that survived dedupe: 23,142,851
- Records that were duplicates: 4,171,283
- **Query-level duplicate ratio: 15.3%**

### 2. Per-Query Duplicate Ratio Distribution

| Statistic | Value |
|-----------|-------|
| Min | 4.7% |
| Max | 45.7% |
| Mean | 16.3% |
| Median | 14.4% |
| P95 | 30.9% |

Most queries have only 10-20% duplicate VIDs, far below the 30% threshold needed for M4 to be viable.

### 3. Intra-Query VID Replication

For each query, how many VIDs appear in multiple postings?

| Statistic | Value |
|-----------|-------|
| Min | 4.5% |
| Max | 38.3% |
| Mean | 14.4% |
| Median | 13.0% |

### 4. Primary Layout Simulation Results

| Layout | Pages/Query | Candidates/Page | vs Legacy |
|--------|-------------|-----------------|-----------|
| VIDOrder | 2139.97 | 1.08 | -1703.3% |
| PrimaryPostingOrder | 2139.97 | 1.08 | -1703.3% |
| CoHitTraceOrder | 1256.93 | 1.84 | -959.2% |
| HotnessOrder | 2139.97 | 1.08 | -1703.3% |
| Oracle Lower Bound | 72.32 | - | +39.1% |

**Why primary pages are higher than legacy**:
- Legacy: ~64 postings per query, each posting packs multiple VIDs' payloads together → ~119 pages
- M4: ~2314 unique VIDs per query, each VID needs its primary payload → if poorly organized, ~1200-2100 pages
- Even CoHitTraceOrder (best layout) needs ~1257 pages, which is **10x worse than legacy**

### 5. Root Cause Analysis

**Why storage-level duplicate ratio (76.7%) differs from query-level duplicate ratio (15.3%)**:

- **Storage perspective**: A VID appears in ~6.4 postings globally. If we store it once instead of 6.4 times, we save 84% storage.

- **Query perspective**: A query typically accesses ~64 postings. Within these postings:
  - Total VID reads: ~2731 per query
  - Unique VIDs after dedupe: ~2314 per query
  - Duplicate reads: ~417 per query (15.3%)
  
- The key insight: **Query access patterns are localized**. Each query touches a small subset of postings, and within that subset, VID overlap is limited. Global replication doesn't translate to query-level duplication.

### 6. Oracle Lower Bound Analysis

The oracle lower bound (72.32 pages/query) shows that even with perfect locality, M4 can only achieve ~39% page reduction over legacy's 119 pages. But this oracle assumes:
1. All primaries fit in minimum pages (perfect packing)
2. All VIDs needed by a query are on the same pages

In practice, even CoHitTraceOrder can't approach this oracle because:
- The working set of ~2314 VIDs is too scattered
- Co-hit patterns in this specific workload don't provide enough locality

## Conclusion

**M4 is not viable for SIFT1M query workload because**:

1. **Query-level duplicate ratio is too low** (15.3% vs 30% threshold)
   - Most duplicate savings are in postings NOT accessed by the same query
   - Query access patterns are too localized to benefit from global dedupe

2. **Primary payload locality is poor**
   - Even best layout (CoHitTraceOrder) needs 10x more pages than legacy
   - Random access to ~2300 primaries is worse than sequential access to ~64 postings

3. **M4 would increase I/O, not decrease it**
   - Legacy: ~119 sequential posting pages per query
   - M4: ~1200+ random primary pages per query

## Recommendation

**Stop M4.** Do not proceed to M4-1 sidecar build.

The previous M4 storage viability conclusion was based on global replication statistics, which don't translate to query-level I/O benefits. M4 would make query I/O worse, not better.

## Files

- Pre-dedupe trace: `results/m4_oracle/pre_dedupe_trace_st8.csv`
- Run log: `results/m4_oracle/run_trace.log`
- Simulation script: `scripts/m4_oracle_simulation.py`
