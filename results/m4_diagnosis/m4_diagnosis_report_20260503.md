# M4 Diagnosis Report: Primary-Secondary Payload Dedupe Feasibility

**Date**: 2026-05-03
**Dataset**: SIFT1M, UInt8 + DEFAULT
**Trace Source**: `results/m2h/phase1/payload_trace_st8.csv` (23M records, 10K queries)

## Executive Summary

**M4 is VIABLE based on trace analysis.**

| Metric | Value | M4 Implication |
|--------|-------|----------------|
| VIDs in multiple postings | 99.7% | Strong duplicate evidence |
| Average replicas per VID | 6.41 | High replication factor |
| Storage overhead | 84.4% | Significant reduction potential |
| Per-posting duplicate ratio | 99.9% | Almost all VIDs are duplicates |

## Key Findings

### 1. Physical Duplicate Storage

```
Current payload storage:  686.11 MB (with replicas)
M4 payload storage:       159.63 MB (primary + pointers)
Potential saving:         526.48 MB (76.7%)
```

**Evidence**:
- 847,000 out of 849,815 VIDs (99.7%) appear in multiple postings
- Each VID is replicated ~6.4 times on average
- 135,674 out of 138,249 postings (98.1%) have 100% duplicate VIDs

### 2. Replica Count Distribution

| Replica Count | VIDs | Percentage |
|---------------|------|------------|
| 1 posting | 2,815 | 0.3% |
| 2 postings | 14,240 | 1.7% |
| 3 postings | 37,657 | 4.4% |
| 4 postings | 67,029 | 7.9% |
| 5 postings | 98,283 | 11.6% |
| 6 postings | 139,719 | 16.4% |
| 7 postings | 212,400 | 25.0% |
| 8 postings | 277,672 | 32.7% |

Most VIDs are highly replicated (6-8 copies), confirming M4's target use case.

### 3. Physical Page Distribution

Each VID's payload is stored across multiple physical pages:

```
Mean unique pages per VID: 6.41
Median unique pages per VID: 7.0
VIDs on single page: 2,816 (0.3%)
```

This means the same VID's payload exists in ~6 different physical locations.

### 4. I/O Impact Analysis

**Critical observation**: Query-level dedupe happens AFTER disk read.

Code flow in `ExtraStaticSearcher.h`:
1. Read entire posting from disk (all pages)
2. Parse each VID in posting
3. Check `m_deduper.CheckAndSet(vectorID)` - dedupe
4. If not duplicate, compute distance

**Implication**: M4 CAN reduce disk I/O, not just storage.

When a query reads N postings containing the same VID:
- **Current**: VID's payload read N times from disk (different physical locations)
- **M4**: VID's payload read once from primary, secondaries use pointer

## Limitations of Current Analysis

1. **Trace is post-dedupe**: `payload_trace` only records VIDs that passed the dedupe check. We cannot directly measure how many duplicate payload bytes were read from disk.

2. **No build-time information**: We don't know which posting is the "primary" for each VID. Build-time analysis would help estimate locality.

3. **I/O benefit estimation requires pre-dedupe trace**: To fully quantify I/O reduction, we would need to trace before the dedupe step.

## M4 Viability Assessment

| Criterion | Status | Evidence |
|-----------|--------|----------|
| Duplicate VID evidence is strong | ✓ PASS | 99.7% VIDs in multiple postings |
| Storage reduction potential | ✓ PASS | 76.7% reduction (526 MB) |
| I/O reduction potential | ⚠ LIKELY | Dedupe happens after disk read |
| Primary payload locality | ⚠ UNKNOWN | Needs build-time analysis |

## Recommended Next Steps

### Phase 1: Pre-Dedupe Trace (Optional)

To quantify I/O benefit more precisely:

1. Add trace BEFORE `CheckAndSet` to capture all VID reads
2. Measure duplicate payload bytes per query
3. Compare to post-dedupe unique VID count

### Phase 2: Design Primary Selection Policy

If proceeding with M4:

1. **VID-to-Primary mapping**: Decide which posting is primary for each VID
   - Options: First assignment, smallest posting, hottest posting
2. **Secondary format**: VID + pointer to primary + optional compact code
3. **Build-time changes**: Record primary assignment during index build
4. **Search-time changes**: On secondary hit, fetch from primary location

### Phase 3: Validate Primary Locality

Critical for M4 success:

- Primary payloads should be clustered by access pattern
- If primaries are randomly distributed, random reads could negate savings
- Consider ordering primaries by CoHit trace or hotness

## Conclusion

**M4 is viable for SIFT1M dataset:**

- Strong duplicate evidence (99.7% VIDs replicated)
- Significant storage reduction potential (76.7%)
- I/O reduction likely (dedupe after disk read)
- **Recommendation**: Proceed with M4 design, starting with primary selection policy

**Risk**: Primary locality is unknown. If primary payloads are randomly distributed, random reads could reduce or negate I/O benefits. Need to validate before full implementation.
