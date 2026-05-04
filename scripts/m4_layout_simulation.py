#!/usr/bin/env python3
"""
M4 Layout Simulator

Simulates different primary payload layouts to estimate pages/query
before implementing the online query path.

Input:
  - pre_dedupe_trace.csv: Query trace with all VIDs read (before dedupe)
  - ssdIndex.m4.loc: Location table from M4 sidecar builder

Output:
  - pages/query for each layout
  - Comparison with legacy pages/query
"""

import argparse
import os
import sys
from collections import defaultdict
import struct


def parse_args():
    parser = argparse.ArgumentParser(description='M4 Layout Simulator')
    parser.add_argument('--trace', required=True, help='Pre-dedupe trace CSV file')
    parser.add_argument('--loc', required=True, help='Location table from M4 sidecar')
    parser.add_argument('--legacy-pages', type=float, default=118.7, help='Legacy pages/query')
    parser.add_argument('--output', help='Output report file')
    return parser.parse_args()


def read_location_table(loc_file):
    """Read M4 location table."""
    vid_to_location = {}

    with open(loc_file, 'rb') as f:
        # Read header
        magic = struct.unpack('<I', f.read(4))[0]
        if magic != 0x4D344C4F:  # "M4LO"
            raise ValueError(f"Invalid location table magic: {hex(magic)}")

        entry_count = struct.unpack('<I', f.read(4))[0]
        print(f"Location table: {entry_count} entries")

        # Read entries
        for _ in range(entry_count):
            vid = struct.unpack('<i', f.read(4))[0]
            payload_offset = struct.unpack('<Q', f.read(8))[0]
            page_idx = struct.unpack('<I', f.read(4))[0]
            byte_offset = struct.unpack('<H', f.read(2))[0]
            _ = f.read(2)  # reserved

            vid_to_location[vid] = {
                'payload_offset': payload_offset,
                'page_idx': page_idx,
                'byte_offset': byte_offset
            }

    return vid_to_location


def read_trace(trace_file):
    """Read pre-dedupe trace and build data structures."""
    query_to_vids = defaultdict(set)
    vid_to_postings = defaultdict(set)
    vid_frequency = defaultdict(int)
    posting_to_vids = defaultdict(set)

    print(f"Reading trace file: {trace_file}")

    with open(trace_file, 'r') as f:
        header = f.readline()  # Skip header

        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 6:
                continue

            query_id = int(parts[0])
            posting_id = int(parts[1])
            vid = int(parts[2])
            was_deduped = int(parts[5])

            query_to_vids[query_id].add(vid)
            vid_to_postings[vid].add(posting_id)
            vid_frequency[vid] += 1
            posting_to_vids[posting_id].add(vid)

    print(f"  Queries: {len(query_to_vids)}")
    print(f"  Unique VIDs: {len(vid_to_postings)}")
    print(f"  Total records: {sum(len(v) for v in query_to_vids.values())}")

    return query_to_vids, vid_to_postings, vid_frequency, posting_to_vids


def simulate_vid_order(query_to_vids, vid_to_location):
    """Simulate VIDOrder layout: primaries packed by VID order."""
    total_pages = 0

    for query_id, vids in query_to_vids.items():
        # Get all pages needed for this query
        pages = set()
        for vid in vids:
            if vid in vid_to_location:
                pages.add(vid_to_location[vid]['page_idx'])
        total_pages += len(pages)

    return total_pages / len(query_to_vids)


def simulate_primary_posting_order(query_to_vids, vid_to_postings, vid_to_location, posting_to_vids):
    """Simulate PrimaryPostingOrder: primary = min posting for each VID."""
    # Assign primary posting for each VID (min posting ID)
    vid_to_primary = {}
    for vid, postings in vid_to_postings.items():
        vid_to_primary[vid] = min(postings)

    # Group VIDs by primary posting
    posting_to_primary_vids = defaultdict(list)
    for vid, primary in vid_to_primary.items():
        posting_to_primary_vids[primary].append(vid)

    # Sort VIDs within each posting
    for posting in posting_to_primary_vids:
        posting_to_primary_vids[posting].sort()

    # Assign pages based on primary posting order
    # This simulates packing primaries sequentially by posting
    vid_to_simulated_page = {}
    current_page = 0
    current_offset = 0
    bytes_per_vid = 132  # 128 bytes payload + 4 bytes VID

    # Sort postings and assign pages
    for posting in sorted(posting_to_primary_vids.keys()):
        for vid in posting_to_primary_vids[posting]:
            vid_to_simulated_page[vid] = current_page
            current_offset += bytes_per_vid
            if current_offset >= 4096:  # Page size
                current_page += 1
                current_offset = 0

    # Simulate queries
    total_pages = 0
    for query_id, vids in query_to_vids.items():
        pages = set()
        for vid in vids:
            if vid in vid_to_simulated_page:
                pages.add(vid_to_simulated_page[vid])
        total_pages += len(pages)

    return total_pages / len(query_to_vids)


def simulate_hotness_order(query_to_vids, vid_frequency, vid_to_location):
    """Simulate HotnessOrder: sort primaries by access frequency."""
    # Sort VIDs by frequency (descending)
    sorted_vids = sorted(vid_frequency.keys(), key=lambda v: -vid_frequency[v])

    # Assign pages based on hotness order
    vid_to_simulated_page = {}
    current_page = 0
    current_offset = 0
    bytes_per_vid = 132

    for vid in sorted_vids:
        vid_to_simulated_page[vid] = current_page
        current_offset += bytes_per_vid
        if current_offset >= 4096:
            current_page += 1
            current_offset = 0

    # Simulate queries
    total_pages = 0
    for query_id, vids in query_to_vids.items():
        pages = set()
        for vid in vids:
            if vid in vid_to_simulated_page:
                pages.add(vid_to_simulated_page[vid])
        total_pages += len(pages)

    return total_pages / len(query_to_vids)


def simulate_cohit_order(query_to_vids, vid_frequency):
    """Simulate CoHitTraceOrder: group VIDs that are accessed together.

    Simplified version: Use query-level locality instead of pairwise co-hit.
    Assign VIDs to pages based on which queries access them together.
    """
    # Build: for each VID, which queries access it
    vid_to_queries = defaultdict(set)
    for query_id, vids in query_to_vids.items():
        for vid in vids:
            vid_to_queries[vid].add(query_id)

    # Sort VIDs by frequency (descending) for initial ordering
    sorted_vids = sorted(vid_frequency.keys(), key=lambda v: -vid_frequency[v])

    # Greedy packing: try to keep VIDs accessed by same queries together
    vid_to_simulated_page = {}
    current_page = 0
    current_offset = 0
    bytes_per_vid = 132
    page_to_queries = defaultdict(set)  # Track which queries each page serves

    for vid in sorted_vids:
        # Find best page (one that already serves similar queries)
        vid_queries = vid_to_queries[vid]
        best_page = None
        best_overlap = -1

        # Only check recent pages (limit search)
        for page in range(max(0, current_page - 10), current_page + 1):
            overlap = len(vid_queries & page_to_queries[page])
            if overlap > best_overlap:
                best_overlap = overlap
                best_page = page

        # Use best page if it has good overlap and space
        if best_overlap > 0 and best_page in vid_to_simulated_page.values():
            # Check if page has space (simplified check)
            vids_on_page = sum(1 for v, p in vid_to_simulated_page.items() if p == best_page)
            if vids_on_page < 31:  # Max VIDs per page
                vid_to_simulated_page[vid] = best_page
                page_to_queries[best_page].update(vid_queries)
                continue

        # Otherwise, assign to current page
        vid_to_simulated_page[vid] = current_page
        page_to_queries[current_page].update(vid_queries)
        current_offset += bytes_per_vid
        if current_offset >= 4096:
            current_page += 1
            current_offset = 0

    # Simulate queries
    total_pages = 0
    for query_id, vids in query_to_vids.items():
        pages = set()
        for vid in vids:
            if vid in vid_to_simulated_page:
                pages.add(vid_to_simulated_page[vid])
        total_pages += len(pages)

    return total_pages / len(query_to_vids)


def main():
    args = parse_args()

    print("=" * 60)
    print("M4 Layout Simulator")
    print("=" * 60)
    print()

    # Read input files
    vid_to_location = read_location_table(args.loc)
    query_to_vids, vid_to_postings, vid_frequency, posting_to_vids = read_trace(args.trace)

    print()
    print("Simulating layouts...")
    print()

    results = {}

    # 1. VIDOrder (using actual location table)
    print("[1/4] VIDOrder layout...")
    results['VIDOrder'] = simulate_vid_order(query_to_vids, vid_to_location)

    # 2. PrimaryPostingOrder
    print("[2/4] PrimaryPostingOrder layout...")
    results['PrimaryPostingOrder'] = simulate_primary_posting_order(
        query_to_vids, vid_to_postings, vid_to_location, posting_to_vids
    )

    # 3. HotnessOrder
    print("[3/4] HotnessOrder layout...")
    results['HotnessOrder'] = simulate_hotness_order(query_to_vids, vid_frequency, vid_to_location)

    # 4. CoHitOrder (simplified)
    print("[4/4] CoHitTraceOrder layout...")
    results['CoHitTraceOrder'] = simulate_cohit_order(query_to_vids, vid_frequency)

    # Print results
    print()
    print("=" * 60)
    print("Simulation Results")
    print("=" * 60)
    print()

    print(f"{'Layout':<25} {'Pages/Query':>12} {'vs Legacy':>12}")
    print("-" * 50)

    for layout, pages_per_query in sorted(results.items(), key=lambda x: x[1]):
        change = (pages_per_query - args.legacy_pages) / args.legacy_pages * 100
        print(f"{layout:<25} {pages_per_query:>12.2f} {change:>+11.1f}%")

    print("-" * 50)
    print(f"{'Legacy':<25} {args.legacy_pages:>12.2f} {'baseline':>12}")

    # Oracle lower bound
    # Assume all VIDs fit in minimum pages
    total_vids = len(vid_frequency)
    vids_per_page = 4096 // 132  # ~31 VIDs per page
    oracle_pages = total_vids / vids_per_page
    oracle_per_query = oracle_pages / len(query_to_vids)  # Very rough estimate

    print()
    print("=" * 60)
    print("Analysis")
    print("=" * 60)
    print()

    best_layout = min(results, key=results.get)
    best_pages = results[best_layout]

    print(f"Best layout: {best_layout}")
    print(f"Best pages/query: {best_pages:.2f}")
    print(f"Legacy pages/query: {args.legacy_pages:.2f}")
    print(f"Change: {(best_pages - args.legacy_pages) / args.legacy_pages * 100:+.1f}%")
    print()

    # Decision criteria
    print("Decision Criteria (from plan):")
    print(f"  Continue to online pure M4 if pages/query <= legacy * 3")
    threshold = args.legacy_pages * 3
    if best_pages <= threshold:
        print(f"    ✓ PASS: {best_pages:.2f} <= {threshold:.2f}")
    else:
        print(f"    ✗ FAIL: {best_pages:.2f} > {threshold:.2f}")

    print()
    print(f"  Continue to online hybrid M4 if pages/query <= legacy * 1.2")
    threshold = args.legacy_pages * 1.2
    if best_pages <= threshold:
        print(f"    ✓ PASS: {best_pages:.2f} <= {threshold:.2f}")
    else:
        print(f"    ✗ FAIL: {best_pages:.2f} > {threshold:.2f}")

    # Write output if specified
    if args.output:
        with open(args.output, 'w') as f:
            f.write("M4 Layout Simulation Results\n")
            f.write("=" * 40 + "\n\n")
            f.write(f"Legacy pages/query: {args.legacy_pages:.2f}\n\n")
            f.write(f"{'Layout':<25} {'Pages/Query':>12} {'vs Legacy':>12}\n")
            f.write("-" * 50 + "\n")
            for layout, pages_per_query in sorted(results.items(), key=lambda x: x[1]):
                change = (pages_per_query - args.legacy_pages) / args.legacy_pages * 100
                f.write(f"{layout:<25} {pages_per_query:>12.2f} {change:>+11.1f}%\n")
        print(f"\nResults written to: {args.output}")


if __name__ == '__main__':
    main()
