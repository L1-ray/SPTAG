#!/usr/bin/env python3
"""
More Accurate Offline Oracle Analysis

Uses pre-dedupe trace to estimate how many postings each query actually needs
to find its top-10 neighbors.

Input:
  - query_io_stats_ir64.csv: Ground truth recall
  - pre_dedupe_trace_st8.csv: VID per posting per query
  - Truth file to identify which VIDs are true neighbors
"""

import csv
import struct
import sys
from collections import defaultdict


def read_truth_file(filepath, query_count=10000, top_k=10):
    """Read truth file to get ground truth neighbors."""
    truth = {}
    with open(filepath, 'rb') as f:
        # Read header: num_queries, num_neighbors
        num_queries = struct.unpack('<i', f.read(4))[0]
        num_neighbors = struct.unpack('<i', f.read(4))[0]

        for qid in range(min(num_queries, query_count)):
            neighbors = struct.unpack(f'<{num_neighbors}i', f.read(4 * num_neighbors))
            truth[qid] = set(neighbors[:top_k])

    return truth


def read_trace(filepath):
    """Read pre-dedupe trace to get posting order and VID info."""
    query_data = defaultdict(lambda: {'postings': defaultdict(list)})

    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            qid = int(row['query_id'])
            pid = int(row['posting_id'])
            vid = int(row['vector_id'])
            query_data[qid]['postings'][pid].append(vid)

    return query_data


def read_query_stats(filepath):
    """Read query IO stats."""
    data = {}
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            qid = int(row['query_id'])
            data[qid] = float(row['recall'])
    return data


def main():
    print("=" * 60)
    print("Offline Oracle Analysis (Ground Truth Based)")
    print("=" * 60)
    print()

    # File paths
    truth_file = '/home/ray/data/sift1m/bigann-1M.bin'
    trace_file = 'results/m4_oracle/pre_dedupe_trace_st8.csv'
    stats_file = 'results/adaptive_budget/h0_decoupling/query_io_stats_ir64.csv'

    # Read ground truth
    print("Reading ground truth...")
    try:
        truth = read_truth_file(truth_file)
        print(f"  Loaded truth for {len(truth)} queries")
    except Exception as e:
        print(f"  Error reading truth: {e}")
        print("  Using recall from query stats instead")
        truth = None

    # Read trace
    print("Reading pre-dedupe trace...")
    trace = read_trace(trace_file)
    print(f"  Loaded trace for {len(trace)} queries")

    # Read baseline recall
    print("Reading baseline recall...")
    baseline_recall = read_query_stats(stats_file)
    print(f"  Loaded stats for {len(baseline_recall)} queries")
    print()

    # Analysis without ground truth (using recall from stats)
    print("=" * 60)
    print("Analysis Based on Recall Statistics")
    print("=" * 60)
    print()

    # Count postings per query
    posting_counts = []
    for qid in trace:
        posting_counts.append(len(trace[qid]['postings']))

    print(f"Postings per query in trace:")
    print(f"  Min: {min(posting_counts)}")
    print(f"  Max: {max(posting_counts)}")
    print(f"  Avg: {sum(posting_counts)/len(posting_counts):.1f}")
    print()

    # For queries with perfect recall at B=64,
    # estimate how many postings they actually needed
    perfect_recall_count = sum(1 for qid, r in baseline_recall.items() if r == 1.0)
    print(f"Queries with perfect recall at B=64: {perfect_recall_count} ({100*perfect_recall_count/len(baseline_recall):.1f}%)")
    print()

    # If we had ground truth, we could compute:
    # - For each query, how many postings until all top-10 VIDs are found
    # - This would give true min_B

    # Since we don't have posting order in this trace,
    # we can only make rough estimates
    print("Note: Accurate min_B calculation requires:")
    print("  1. Posting order (posting_index) in trace")
    print("  2. Head distance per posting")
    print()
    print("Current trace lacks posting order, so we use simplified analysis.")

    # Summary based on available data
    print()
    print("=" * 60)
    print("Summary")
    print("=" * 60)
    print()

    print("From H0.1 decoupling experiment:")
    print("  - 83.2% queries have perfect recall at B=64")
    print("  - 12.9% queries improved by B=128")
    print("  - 3.9% queries unchanged (routing miss)")
    print()

    print("This strongly suggests adaptive budget potential:")
    print("  - Easy queries (83.2%): Could potentially use B < 64")
    print("  - Hard queries (12.9%): Could benefit from B > 64")
    print()

    print("Recommendation:")
    print("  1. Implement HeadCandidateNum/PostingBudget decoupling")
    print("  2. Collect head distance distribution trace")
    print("  3. Run true offline oracle with per-posting recall tracking")


if __name__ == '__main__':
    main()
