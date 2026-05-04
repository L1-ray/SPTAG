#!/usr/bin/env python3
"""
Offline Oracle Analysis for Adaptive Posting Budget

Simulates different posting budgets B ∈ {16, 32, 48, 64, 96, 128} using
existing ir=128 trace data to find min_B_for_target_recall for each query.

Input:
  - query_io_stats_ir64.csv: Baseline recall at B=64
  - query_io_stats_ir128.csv: Recall at B=128
  - pre_dedupe_trace_st8.csv: VID per posting per query

Note: This is a simplified oracle because we don't have per-posting recall info.
      We can only estimate min_B based on recall jumps between B=64 and B=128.
"""

import csv
import sys
from collections import defaultdict


def read_query_stats(filepath):
    """Read query IO stats CSV."""
    data = {}
    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            qid = int(row['query_id'])
            data[qid] = {
                'recall': float(row['recall']),
                'postings_touched': int(row['postings_touched']),
                'pages_read': int(row['pages_read']),
                'posting_elements_raw': int(row['posting_elements_raw']),
                'distance_evaluated_count': int(row['distance_evaluated_count'])
            }
    return data


def read_trace(filepath):
    """Read pre-dedupe trace to get posting->VID mapping."""
    query_postings = defaultdict(lambda: defaultdict(list))

    with open(filepath, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            qid = int(row['query_id'])
            pid = int(row['posting_id'])
            vid = int(row['vector_id'])
            query_postings[qid][pid].append(vid)

    return query_postings


def estimate_min_b(stats_64, stats_128):
    """
    Estimate min_B for each query based on recall at B=64 and B=128.

    This is a simplified estimation:
    - If recall_64 == 1.0: min_B <= 64 (could be lower)
    - If recall_128 == 1.0 and recall_64 < 1.0: min_B between 64 and 128
    - If recall_128 < 1.0: min_B > 128 (routing miss, cannot fix with budget)
    """
    results = {}

    for qid in stats_64:
        r64 = stats_64[qid]['recall']
        r128 = stats_128[qid]['recall']

        if r64 == 1.0:
            # Perfect recall at B=64, might need less
            # Estimate: assume linear improvement, min_B could be lower
            min_b = 32  # Conservative estimate
        elif r128 == 1.0:
            # Fixed by B=128, estimate interpolation point
            # Linear interpolation: recall improves from r64 to 1.0
            # between B=64 and B=128
            if r64 < 1.0:
                # Estimate B where recall would reach 1.0
                improvement_rate = (1.0 - r64) / 64  # per posting
                min_b = int(64 + (1.0 - r64) / improvement_rate) if improvement_rate > 0 else 128
                min_b = min(max(min_b, 65), 128)
            else:
                min_b = 64
        elif r128 > r64:
            # Improved but not perfect
            # min_B > 128
            min_b = 128 + 1  # Cannot achieve target with B=128
        else:
            # No improvement, routing miss
            min_b = -1  # Cannot fix with budget

        results[qid] = {
            'recall_64': r64,
            'recall_128': r128,
            'min_b_estimate': min_b,
            'pages_64': stats_64[qid]['pages_read'],
            'pages_128': stats_128[qid]['pages_read']
        }

    return results


def main():
    print("=" * 60)
    print("Offline Oracle Analysis (Simplified)")
    print("=" * 60)
    print()

    # Read data
    print("Reading query stats...")
    stats_64 = read_query_stats('results/adaptive_budget/h0_decoupling/query_io_stats_ir64.csv')
    stats_128 = read_query_stats('results/adaptive_budget/h0_decoupling/query_io_stats_ir128.csv')

    print(f"  Queries with ir=64: {len(stats_64)}")
    print(f"  Queries with ir=128: {len(stats_128)}")
    print()

    # Estimate min_B
    print("Estimating min_B for each query...")
    results = estimate_min_b(stats_64, stats_128)

    # Analyze min_B distribution
    min_b_dist = defaultdict(int)
    for qid, data in results.items():
        b = data['min_b_estimate']
        if b == -1:
            min_b_dist['routing_miss'] += 1
        elif b <= 32:
            min_b_dist['<=32'] += 1
        elif b <= 48:
            min_b_dist['33-48'] += 1
        elif b <= 64:
            min_b_dist['49-64'] += 1
        elif b <= 96:
            min_b_dist['65-96'] += 1
        elif b <= 128:
            min_b_dist['97-128'] += 1
        else:
            min_b_dist['>128'] += 1

    print()
    print("min_B Distribution (Estimate):")
    total = len(results)
    for bucket in ['<=32', '33-48', '49-64', '65-96', '97-128', '>128', 'routing_miss']:
        count = min_b_dist[bucket]
        pct = 100 * count / total
        print(f"  {bucket:>12}: {count:5d} ({pct:5.1f}%)")

    # Calculate oracle pages/query
    print()
    print("=" * 60)
    print("Oracle Analysis")
    print("=" * 60)
    print()

    # Fixed budget comparison
    print("Fixed Budget Baselines:")
    avg_pages_64 = sum(d['pages_64'] for d in results.values()) / total
    avg_pages_128 = sum(d['pages_128'] for d in results.values()) / total
    print(f"  B=64:  pages/query = {avg_pages_64:.2f}")
    print(f"  B=128: pages/query = {avg_pages_128:.2f}")
    print()

    # Oracle upper bound (if we could perfectly predict min_B)
    # This is a rough estimate since we don't have actual per-budget recall
    print("Oracle Upper Bound (Theoretical):")
    print("  If all queries used their min_B:")
    print("  - This would require knowing ground truth ahead of time")
    print("  - Realistic goal: approach this with feature-based prediction")
    print()

    # Summary
    print("=" * 60)
    print("Summary")
    print("=" * 60)
    print()

    easy_queries = min_b_dist['<=32'] + min_b_dist['33-48']
    normal_queries = min_b_dist['49-64']
    hard_queries = min_b_dist['65-96'] + min_b_dist['97-128']
    routing_miss = min_b_dist['routing_miss']

    print(f"Easy queries (min_B <= 48):  {easy_queries:5d} ({100*easy_queries/total:.1f}%)")
    print(f"Normal queries (min_B 49-64): {normal_queries:5d} ({100*normal_queries/total:.1f}%)")
    print(f"Hard queries (min_B > 64):   {hard_queries:5d} ({100*hard_queries/total:.1f}%)")
    print(f"Routing miss (min_B > 128):  {routing_miss:5d} ({100*routing_miss/total:.1f}%)")
    print()

    print("Potential Adaptive Budget Benefit:")
    print(f"  Easy queries could save pages by using B < 64")
    print(f"  Hard queries could improve recall by using B > 64")
    print()

    # Decision criteria check
    print("=" * 60)
    print("Decision Criteria Check (from plan)")
    print("=" * 60)
    print()

    # From plan: >=30% query min_B <= 32 or 40
    easy_32 = min_b_dist['<=32']
    pct_easy_32 = 100 * easy_32 / total
    print(f"1. >=30% query min_B <= 32 or 40:")
    print(f"   min_B <= 32: {pct_easy_32:.1f}% ({'PASS' if pct_easy_32 >= 30 else 'FAIL'})")

    # From plan: >=50% query min_B <= 48
    easy_48 = min_b_dist['<=32'] + min_b_dist['33-48']
    pct_easy_48 = 100 * easy_48 / total
    print(f"2. >=50% query min_B <= 48:")
    print(f"   min_B <= 48: {pct_easy_48:.1f}% ({'PASS' if pct_easy_48 >= 50 else 'FAIL'})")

    print()
    print("Note: This is a simplified estimate. For accurate min_B,")
    print("we need per-budget recall data (requires code modification).")


if __name__ == '__main__':
    main()
