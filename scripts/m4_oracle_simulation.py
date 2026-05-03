#!/usr/bin/env python3
"""
M4-0 Oracle: Simulate different primary payload layouts to estimate
primary pages/query reduction for M4 primary-secondary payload dedupe.

Input: pre_dedupe_trace_st8.csv (from Phase 1)

Output:
- Duplicate payload bytes analysis
- Primary pages/query estimation for 4 layouts
- PASS/FAIL decision based on M4-0 criteria

Success criteria:
1. Duplicate payload bytes >= 30% of total payload bytes read
2. Best layout reduces primary pages/query >= 15% vs legacy
"""

import argparse
import csv
import sys
from collections import defaultdict, Counter
from pathlib import Path
from typing import Dict, Set, List, Tuple

PAGE_SIZE = 4096
PAYLOAD_BYTES = 128  # SIFT1M: 128 dimensions * 1 byte (UInt8) = 128 bytes


def load_trace(trace_path: str, max_queries: int = None) -> Tuple[
    Dict[int, Set[int]],  # vid_to_postings: VID -> set of posting_ids
    Dict[int, Set[int]],  # query_to_vids: query_id -> set of VIDs (after dedupe)
    Dict[int, Set[int]],  # query_to_all_vids: query_id -> all VIDs (before dedupe)
    Counter,              # vid_frequency: VID -> access count
    int,                  # total_records
    int,                  # deduped_records
]:
    """Load pre-dedupe trace and build data structures."""
    vid_to_postings = defaultdict(set)
    query_to_vids = defaultdict(set)      # VIDs that survived dedupe
    query_to_all_vids = defaultdict(set)   # All VIDs read from disk
    vid_frequency = Counter()
    total_records = 0
    deduped_records = 0

    with open(trace_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            query_id = int(row['query_id'])
            posting_id = int(row['posting_id'])
            vector_id = int(row['vector_id'])
            was_deduped = int(row['was_deduped']) == 0  # 0 = NOT deduped (survived)

            vid_to_postings[vector_id].add(posting_id)
            query_to_all_vids[query_id].add(vector_id)
            vid_frequency[vector_id] += 1
            total_records += 1

            if was_deduped:
                query_to_vids[query_id].add(vector_id)
                deduped_records += 1

            if max_queries and query_id >= max_queries:
                break

    return vid_to_postings, query_to_vids, query_to_all_vids, vid_frequency, total_records, deduped_records


def assign_primaries_vid_order(vid_to_postings: Dict[int, Set[int]]) -> Dict[int, int]:
    """Layout 1: Assign primary to smallest posting ID, order by VID."""
    primary_assignment = {}
    for vid in sorted(vid_to_postings.keys()):
        primary_assignment[vid] = min(vid_to_postings[vid])
    return primary_assignment


def assign_primaries_primary_posting_order(vid_to_postings: Dict[int, Set[int]]) -> Dict[int, int]:
    """Layout 2: Same assignment as VIDOrder, but will be clustered by posting."""
    return assign_primaries_vid_order(vid_to_postings)


def assign_primaries_hotness(vid_to_postings: Dict[int, Set[int]], vid_frequency: Counter) -> Dict[int, int]:
    """Layout 4: Order by access frequency (hottest first)."""
    primary_assignment = {}
    for vid, _ in vid_frequency.most_common():
        if vid in vid_to_postings:
            primary_assignment[vid] = min(vid_to_postings[vid])
    return primary_assignment


def simulate_global_page_packing(
    primary_assignment: Dict[int, int],
    vid_to_postings: Dict[int, Set[int]],
    page_size: int = PAGE_SIZE,
    payload_bytes: int = PAYLOAD_BYTES
) -> Dict[int, int]:
    """
    Simulate GLOBAL page packing: all primaries are packed into a single global store.

    This is the M4 model: one global primary payload store with all VIDs.

    Returns: vid -> global_page_id
    """
    vids_per_page = page_size // payload_bytes

    # Sort VIDs for deterministic packing
    sorted_vids = sorted(primary_assignment.keys())

    vid_to_page = {}
    for idx, vid in enumerate(sorted_vids):
        page_idx = idx // vids_per_page
        vid_to_page[vid] = page_idx

    return vid_to_page


def simulate_page_packing_cohit(
    primary_assignment: Dict[int, int],
    vid_to_postings: Dict[int, Set[int]],
    query_to_vids: Dict[int, Set[int]],
    vid_frequency: Counter,
    page_size: int = PAGE_SIZE,
    payload_bytes: int = PAYLOAD_BYTES
) -> Dict[int, int]:
    """
    Layout 3: CoHitTraceOrder - order primaries by query co-access frequency.

    Vectors that appear together in the same query should be placed on the same page.
    """
    # Build co-hit graph: for each VID, count how often it appears with other VIDs
    # This is expensive, so we use a simplified greedy approach:
    # 1. Sort VIDs by frequency (hot first)
    # 2. For each query, try to place its VIDs together

    vids_per_page = page_size // payload_bytes

    # Get all VIDs sorted by frequency
    sorted_vids = [vid for vid, _ in vid_frequency.most_common()]

    # Greedy page assignment: try to keep query VIDs together
    vid_to_page = {}
    current_page = 0
    current_vids_on_page = 0

    for vid in sorted_vids:
        if vid in vid_to_page:
            continue

        # Start a new page with this VID
        vid_to_page[vid] = current_page
        current_vids_on_page = 1

        # Try to fill this page with VIDs from queries that contain this VID
        # Find queries containing this VID
        queries_with_vid = [qid for qid, vids in query_to_vids.items() if vid in vids]

        for qid in queries_with_vid:
            for other_vid in query_to_vids[qid]:
                if other_vid not in vid_to_page and current_vids_on_page < vids_per_page:
                    vid_to_page[other_vid] = current_page
                    current_vids_on_page += 1

            if current_vids_on_page >= vids_per_page:
                break

        # Move to next page
        current_page += 1

    return vid_to_page


def compute_metrics(
    primary_assignment: Dict[int, int],
    vid_to_page: Dict[int, int],
    query_to_vids: Dict[int, Set[int]],
    query_to_all_vids: Dict[int, Set[int]],
    page_size: int = PAGE_SIZE,
    payload_bytes: int = PAYLOAD_BYTES
) -> Tuple[float, float, float, float]:
    """
    Compute metrics for a layout.

    Returns:
    - avg_primary_pages_per_query: unique primary pages touched per query
    - avg_candidates_per_page: average candidates per primary page
    - avg_legacy_pages_per_query: legacy posting pages per query (approximate)
    - total_payload_bytes: total payload bytes read
    """
    # Compute primary pages per query
    total_primary_pages = 0
    total_candidates = 0
    total_legacy_pages = 0
    total_payload_bytes = 0
    query_count = 0

    # For legacy pages estimation, we need to count unique postings per query
    # Each posting spans multiple pages (approx 1-4 pages based on size)
    # We'll use the actual page count from the trace if available
    # For now, estimate based on avg posting size

    for qid, vids in query_to_vids.items():
        # Primary pages: unique pages for VIDs that survived dedupe
        primary_pages = set()
        for vid in vids:
            if vid in vid_to_page:
                primary_pages.add(vid_to_page[vid])
        total_primary_pages += len(primary_pages)
        total_candidates += len(vids)
        query_count += 1

    # For all VIDs (before dedupe), compute legacy payload bytes
    for qid, all_vids in query_to_all_vids.items():
        total_payload_bytes += len(all_vids) * payload_bytes

    avg_primary_pages = total_primary_pages / query_count if query_count > 0 else 0
    avg_candidates_per_page = total_candidates / total_primary_pages if total_primary_pages > 0 else 0

    # Legacy pages estimation: each posting is ~2 pages on average (from trace stats)
    # Actually, we should use the actual pages from the trace
    # For now, estimate based on payload bytes
    avg_legacy_pages = (total_payload_bytes / query_count) / page_size if query_count > 0 else 0

    return avg_primary_pages, avg_candidates_per_page, avg_legacy_pages, total_payload_bytes


def main():
    parser = argparse.ArgumentParser(description='M4-0 Oracle: Primary Layout Simulation')
    parser.add_argument('trace_path', help='Path to pre-dedupe trace CSV')
    parser.add_argument('--max-queries', type=int, default=None, help='Limit queries for faster testing')
    args = parser.parse_args()

    print("=" * 60)
    print("M4-0 Oracle: Primary Payload Layout Simulation")
    print("=" * 60)
    print()

    # Load trace
    print(f"Loading trace from {args.trace_path}...")
    vid_to_postings, query_to_vids, query_to_all_vids, vid_frequency, total_records, deduped_records = load_trace(
        args.trace_path, args.max_queries
    )
    print(f"  Total records: {total_records:,}")
    print(f"  Deduped records: {deduped_records:,}")
    print(f"  Duplicate records: {total_records - deduped_records:,}")
    print(f"  Unique VIDs: {len(vid_to_postings):,}")
    print(f"  Unique queries: {len(query_to_vids):,}")
    print()

    # Duplicate payload analysis
    # was_deduped=1 means the VID was filtered out as a duplicate
    # was_deduped=0 means the VID survived dedupe
    # Total payload bytes = all records * payload_bytes
    total_payload_bytes = total_records * PAYLOAD_BYTES
    # Duplicate payload bytes = records where was_deduped=1
    duplicate_payload_bytes = (total_records - deduped_records) * PAYLOAD_BYTES
    unique_payload_bytes = deduped_records * PAYLOAD_BYTES
    duplicate_ratio = duplicate_payload_bytes / total_payload_bytes if total_payload_bytes > 0 else 0

    print("=" * 60)
    print("Duplicate Payload Analysis")
    print("=" * 60)
    print(f"  Total payload bytes read: {total_payload_bytes / 1024 / 1024:.2f} MB")
    print(f"  Unique payload bytes (after dedupe): {unique_payload_bytes / 1024 / 1024:.2f} MB")
    print(f"  Duplicate payload bytes: {duplicate_payload_bytes / 1024 / 1024:.2f} MB ({duplicate_ratio*100:.1f}%)")
    print()

    # Legacy baseline: estimate pages per query
    # From the trace: avg pages per query = 118.671
    # But for primary layout, we care about how many unique pages are needed for primaries
    legacy_pages_per_query = 118.671  # From the actual run log

    # Simulate layouts
    print("=" * 60)
    print("Layout Simulation Results")
    print("=" * 60)
    print()

    results = []

    # Layout 1: VIDOrder
    print("Simulating VIDOrder layout...")
    primary_assignment = assign_primaries_vid_order(vid_to_postings)
    vid_to_page = simulate_global_page_packing(primary_assignment, vid_to_postings)
    avg_pages, avg_cand_pp, _, _ = compute_metrics(
        primary_assignment, vid_to_page, query_to_vids, query_to_all_vids
    )
    reduction = (legacy_pages_per_query - avg_pages) / legacy_pages_per_query * 100
    results.append(("VIDOrder", avg_pages, avg_cand_pp, reduction))
    print(f"  Avg primary pages/query: {avg_pages:.2f}")
    print(f"  Avg candidates/page: {avg_cand_pp:.2f}")
    print(f"  Reduction vs legacy: {reduction:.1f}%")
    print()

    # Layout 2: PrimaryPostingOrder
    print("Simulating PrimaryPostingOrder layout...")
    # Same assignment as VIDOrder, but the page packing already clusters by posting
    # So this gives the same result as VIDOrder in our simplified model
    results.append(("PrimaryPostingOrder", avg_pages, avg_cand_pp, reduction))
    print(f"  (Same as VIDOrder in simplified model)")
    print()

    # Layout 3: CoHitTraceOrder
    print("Simulating CoHitTraceOrder layout...")
    vid_to_page_cohit = simulate_page_packing_cohit(
        primary_assignment, vid_to_postings, query_to_vids, vid_frequency
    )
    avg_pages_cohit, avg_cand_pp_cohit, _, _ = compute_metrics(
        primary_assignment, vid_to_page_cohit, query_to_vids, query_to_all_vids
    )
    reduction_cohit = (legacy_pages_per_query - avg_pages_cohit) / legacy_pages_per_query * 100
    results.append(("CoHitTraceOrder", avg_pages_cohit, avg_cand_pp_cohit, reduction_cohit))
    print(f"  Avg primary pages/query: {avg_pages_cohit:.2f}")
    print(f"  Avg candidates/page: {avg_cand_pp_cohit:.2f}")
    print(f"  Reduction vs legacy: {reduction_cohit:.1f}%")
    print()

    # Layout 4: HotnessOrder
    print("Simulating HotnessOrder layout...")
    primary_assignment_hot = assign_primaries_hotness(vid_to_postings, vid_frequency)
    vid_to_page_hot = simulate_global_page_packing(primary_assignment_hot, vid_to_postings)
    avg_pages_hot, avg_cand_pp_hot, _, _ = compute_metrics(
        primary_assignment_hot, vid_to_page_hot, query_to_vids, query_to_all_vids
    )
    reduction_hot = (legacy_pages_per_query - avg_pages_hot) / legacy_pages_per_query * 100
    results.append(("HotnessOrder", avg_pages_hot, avg_cand_pp_hot, reduction_hot))
    print(f"  Avg primary pages/query: {avg_pages_hot:.2f}")
    print(f"  Avg candidates/page: {avg_cand_pp_hot:.2f}")
    print(f"  Reduction vs legacy: {reduction_hot:.1f}%")
    print()

    # Oracle lower bound: ideal packing where each page is fully utilized
    # This assumes perfect locality: all VIDs needed by a query fit in minimum pages
    oracle_pages_per_query = unique_payload_bytes / len(query_to_vids) / PAGE_SIZE
    oracle_reduction = (legacy_pages_per_query - oracle_pages_per_query) / legacy_pages_per_query * 100
    results.append(("Oracle Lower Bound", oracle_pages_per_query, "-", oracle_reduction))

    # Summary table
    print("=" * 60)
    print("Summary")
    print("=" * 60)
    print(f"{'Layout':<25} {'Pages/Q':>10} {'Cand/Page':>12} {'vs Legacy':>12}")
    print("-" * 60)
    for name, pages, cand, reduction in results:
        cand_str = f"{cand:.2f}" if isinstance(cand, float) else str(cand)
        print(f"{name:<25} {pages:>10.2f} {cand_str:>12} {reduction:>11.1f}%")
    print()

    # Decision
    print("=" * 60)
    print("M4-0 Oracle Decision")
    print("=" * 60)

    # Criterion 1: Duplicate payload bytes >= 30%
    criterion1 = duplicate_ratio >= 0.30
    print(f"  Criterion 1: Duplicate payload bytes >= 30%")
    print(f"    Result: {duplicate_ratio*100:.1f}% {'PASS' if criterion1 else 'FAIL'}")

    # Criterion 2: Best layout reduces pages >= 15%
    best_reduction = max(r[3] for r in results[:-1])  # Exclude oracle
    criterion2 = best_reduction >= 15.0
    print(f"  Criterion 2: Best layout reduces primary pages >= 15%")
    print(f"    Result: {best_reduction:.1f}% {'PASS' if criterion2 else 'FAIL'}")

    print()
    overall = criterion1 and criterion2
    print(f"  Overall: {'PASS - Proceed to M4-1' if overall else 'FAIL - Stop M4'}")
    print()

    return 0 if overall else 1


if __name__ == '__main__':
    sys.exit(main())
