#!/usr/bin/env python3
"""
Lightweight M4 Layout Validator - With CoHitTraceOrder

Validates the offline query simulation results including CoHitTraceOrder.
Uses streaming approach to avoid memory issues.
"""

import struct
import sys
from collections import defaultdict


def read_location_table(loc_file):
    """Read M4 location table."""
    vid_to_page = {}

    with open(loc_file, 'rb') as f:
        magic = struct.unpack('<I', f.read(4))[0]
        if magic != 0x4D344C4F:
            raise ValueError(f"Invalid magic: {hex(magic)}")

        entry_count = struct.unpack('<I', f.read(4))[0]
        print(f"Location table: {entry_count} entries")

        for _ in range(entry_count):
            vid = struct.unpack('<i', f.read(4))[0]
            _ = f.read(8)   # payload_offset
            page_idx = struct.unpack('<I', f.read(4))[0]
            _ = f.read(4)   # byte_offset + reserved
            vid_to_page[vid] = page_idx

    return vid_to_page


def analyze_trace_streaming(trace_file):
    """Analyze trace using streaming."""
    query_vids = defaultdict(set)
    vid_frequency = defaultdict(int)
    vid_to_queries = defaultdict(set)  # For CoHitTraceOrder

    print(f"Reading trace (streaming): {trace_file}")

    with open(trace_file, 'r') as f:
        header = f.readline()
        line_count = 0

        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 6:
                continue

            query_id = int(parts[0])
            vid = int(parts[2])

            query_vids[query_id].add(vid)
            vid_frequency[vid] += 1
            vid_to_queries[vid].add(query_id)
            line_count += 1

            if line_count % 5000000 == 0:
                print(f"  Processed {line_count/1e6:.1f}M lines...")

    print(f"  Total lines: {line_count}")
    print(f"  Unique queries: {len(query_vids)}")
    print(f"  Unique VIDs: {len(vid_frequency)}")

    return query_vids, vid_frequency, vid_to_queries


def simulate_vid_order(query_vids, vid_to_page):
    """Simulate VIDOrder layout using actual location table."""
    total_pages = 0
    missing_count = 0

    for query_id, vids in query_vids.items():
        pages = set()
        for vid in vids:
            if vid in vid_to_page:
                pages.add(vid_to_page[vid])
            else:
                missing_count += 1
        total_pages += len(pages)

    return total_pages / len(query_vids), missing_count


def simulate_cohit_order(query_vids, vid_frequency, vid_to_queries):
    """
    Simulate CoHitTraceOrder layout.

    Greedy approach: pack VIDs accessed by same queries together.
    """
    print("  Building CoHitTraceOrder layout...")

    # Sort VIDs by frequency (descending) for initial ordering
    sorted_vids = sorted(vid_frequency.keys(), key=lambda v: -vid_frequency[v])

    # Page assignment
    vid_to_page = {}
    current_page = 0
    current_offset = 0
    bytes_per_vid = 132  # 128 bytes payload + 4 bytes VID
    vids_per_page = 4096 // bytes_per_vid  # ~31

    # Track which queries each page serves
    page_to_queries = defaultdict(set)

    # Greedy packing with limited lookback
    lookback = 100  # Only check recent pages

    total = len(sorted_vids)
    for idx, vid in enumerate(sorted_vids):
        if idx % 100000 == 0:
            print(f"    Assigning VIDs: {idx}/{total} ({100*idx/total:.1f}%)")

        vid_queries = vid_to_queries[vid]

        # Find best page (one with most query overlap)
        best_page = None
        best_overlap = 0

        search_start = max(0, current_page - lookback)
        for page in range(search_start, current_page + 1):
            overlap = len(vid_queries & page_to_queries[page])
            if overlap > best_overlap:
                best_overlap = overlap
                best_page = page

        # Use best page if good overlap and has space
        if best_overlap > 0:
            vids_on_best = sum(1 for v, p in vid_to_page.items() if p == best_page)
            if vids_on_best < vids_per_page:
                vid_to_page[vid] = best_page
                page_to_queries[best_page].update(vid_queries)
                continue

        # Otherwise, use current page
        vid_to_page[vid] = current_page
        page_to_queries[current_page].update(vid_queries)
        current_offset += bytes_per_vid
        if current_offset >= 4096:
            current_page += 1
            current_offset = 0

    # Simulate queries
    print("  Simulating queries...")
    total_pages = 0
    for query_id, vids in query_vids.items():
        pages = set()
        for vid in vids:
            if vid in vid_to_page:
                pages.add(vid_to_page[vid])
        total_pages += len(pages)

    return total_pages / len(query_vids)


def main():
    print("=" * 60)
    print("M4 Layout Validation (with CoHitTraceOrder)")
    print("=" * 60)
    print()

    trace_file = "results/m4_oracle/pre_dedupe_trace_st8.csv"
    loc_file = "results/m4_storage/sift1m_m4/ssdIndex.m4.loc"
    legacy_pages = 118.7

    # Read location table
    vid_to_page_loc = read_location_table(loc_file)
    print()

    # Analyze trace
    query_vids, vid_frequency, vid_to_queries = analyze_trace_streaming(trace_file)
    print()

    # Simulate VIDOrder
    print("[1/2] Simulating VIDOrder layout...")
    vid_order_pages, missing = simulate_vid_order(query_vids, vid_to_page_loc)
    print()

    # Simulate CoHitTraceOrder
    print("[2/2] Simulating CoHitTraceOrder layout...")
    cohit_pages = simulate_cohit_order(query_vids, vid_frequency, vid_to_queries)
    print()

    # Results
    print("=" * 60)
    print("Results")
    print("=" * 60)
    print()
    print(f"{'Layout':<25} {'Pages/Query':>12} {'vs Legacy':>12}")
    print("-" * 50)

    for name, pages in [('VIDOrder', vid_order_pages), ('CoHitTraceOrder', cohit_pages)]:
        change = (pages - legacy_pages) / legacy_pages * 100
        print(f"{name:<25} {pages:>12.2f} {change:>+11.1f}%")

    print("-" * 50)
    print(f"{'Legacy':<25} {legacy_pages:>12.2f} {'baseline':>12}")

    print()
    print("=" * 60)
    print("Decision Criteria")
    print("=" * 60)
    print()

    best = min(vid_order_pages, cohit_pages)
    threshold_pure = legacy_pages * 3
    threshold_hybrid = legacy_pages * 1.2

    print(f"Pure M4 threshold: pages/query <= {threshold_pure:.1f}")
    if best <= threshold_pure:
        print(f"  ✓ PASS: {best:.2f} <= {threshold_pure:.1f}")
    else:
        print(f"  ✗ FAIL: {best:.2f} > {threshold_pure:.1f}")

    print()
    print(f"Hybrid M4 threshold: pages/query <= {threshold_hybrid:.1f}")
    if best <= threshold_hybrid:
        print(f"  ✓ PASS: {best:.2f} <= {threshold_hybrid:.1f}")
    else:
        print(f"  ✗ FAIL: {best:.2f} > {threshold_hybrid:.1f}")

    print()
    print(f"Best layout: {'VIDOrder' if vid_order_pages < cohit_pages else 'CoHitTraceOrder'}")
    print(f"Best pages/query: {best:.2f}")
    print(f"Increase over legacy: {best / legacy_pages:.1f}x")

    if missing > 0:
        print(f"\nNote: {missing} VID lookups missing from location table")


if __name__ == '__main__':
    main()
