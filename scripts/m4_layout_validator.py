#!/usr/bin/env python3
"""
Lightweight M4 Layout Validator

Validates the offline query simulation results using streaming approach
to avoid loading the entire trace into memory.
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


def analyze_trace_streaming(trace_file, vid_to_page):
    """Analyze trace using streaming to avoid memory issues."""
    query_vids = defaultdict(set)
    vid_frequency = defaultdict(int)

    print(f"Reading trace (streaming): {trace_file}")

    with open(trace_file, 'r') as f:
        header = f.readline()  # Skip header
        line_count = 0

        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 6:
                continue

            query_id = int(parts[0])
            vid = int(parts[2])

            query_vids[query_id].add(vid)
            vid_frequency[vid] += 1
            line_count += 1

            if line_count % 5000000 == 0:
                print(f"  Processed {line_count/1e6:.1f}M lines...")

    print(f"  Total lines: {line_count}")
    print(f"  Unique queries: {len(query_vids)}")
    print(f"  Unique VIDs: {len(vid_frequency)}")

    return query_vids, vid_frequency


def simulate_vid_order(query_vids, vid_to_page):
    """Simulate VIDOrder layout using actual location table."""
    total_pages = 0
    queries_with_missing = 0

    for query_id, vids in query_vids.items():
        pages = set()
        for vid in vids:
            if vid in vid_to_page:
                pages.add(vid_to_page[vid])
            else:
                queries_with_missing += 1
        total_pages += len(pages)

    avg_pages = total_pages / len(query_vids)
    return avg_pages, queries_with_missing


def main():
    print("=" * 60)
    print("M4 Layout Validation (Streaming)")
    print("=" * 60)
    print()

    trace_file = "results/m4_oracle/pre_dedupe_trace_st8.csv"
    loc_file = "results/m4_storage/sift1m_m4/ssdIndex.m4.loc"
    legacy_pages = 118.7

    # Read location table
    vid_to_page = read_location_table(loc_file)
    print()

    # Analyze trace (streaming)
    query_vids, vid_frequency = analyze_trace_streaming(trace_file, vid_to_page)
    print()

    # Simulate VIDOrder layout
    print("Simulating VIDOrder layout...")
    avg_pages, missing = simulate_vid_order(query_vids, vid_to_page)

    print()
    print("=" * 60)
    print("Results")
    print("=" * 60)
    print()
    print(f"{'Layout':<25} {'Pages/Query':>12} {'vs Legacy':>12}")
    print("-" * 50)

    change = (avg_pages - legacy_pages) / legacy_pages * 100
    print(f"{'VIDOrder':<25} {avg_pages:>12.2f} {change:>+11.1f}%")
    print("-" * 50)
    print(f"{'Legacy':<25} {legacy_pages:>12.2f} {'baseline':>12}")

    print()
    print("=" * 60)
    print("Decision Criteria")
    print("=" * 60)
    print()

    threshold_pure = legacy_pages * 3
    threshold_hybrid = legacy_pages * 1.2

    print(f"Pure M4 threshold: pages/query <= {threshold_pure:.1f}")
    if avg_pages <= threshold_pure:
        print(f"  ✓ PASS: {avg_pages:.2f} <= {threshold_pure:.1f}")
    else:
        print(f"  ✗ FAIL: {avg_pages:.2f} > {threshold_pure:.1f}")

    print()
    print(f"Hybrid M4 threshold: pages/query <= {threshold_hybrid:.1f}")
    if avg_pages <= threshold_hybrid:
        print(f"  ✓ PASS: {avg_pages:.2f} <= {threshold_hybrid:.1f}")
    else:
        print(f"  ✗ FAIL: {avg_pages:.2f} > {threshold_hybrid:.1f}")

    print()
    print(f"VIDOrder pages/query increase: {avg_pages / legacy_pages:.1f}x")

    if missing > 0:
        print(f"\nWarning: {missing} queries had VIDs not in location table")


if __name__ == '__main__':
    main()
