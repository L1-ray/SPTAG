#!/usr/bin/env python3
"""
Compute min_B_for_target_recall for SIFT10M queries.
"""

import csv
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
            }
    return data


def main():
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]

    # Read all stats
    all_stats = {}
    for b in budgets:
        filepath = f'results/adaptive_budget/sift10m_budget_sweep/query_io_stats_b{b}.csv'
        try:
            all_stats[b] = read_query_stats(filepath)
            print(f"Read B={b}: {len(all_stats[b])} queries")
        except Exception as e:
            print(f"Error reading B={b}: {e}")
            return

    # Get baseline recall (B=64)
    baseline_recall = {}
    for qid in all_stats[64]:
        baseline_recall[qid] = all_stats[64][qid]['recall']

    print()
    print("=" * 60)
    print("SIFT10M: Computing min_B_for_baseline_query_recall")
    print("=" * 60)
    print()

    # For each query, find min_B where recall >= baseline_recall
    min_b_distribution = defaultdict(int)
    min_b_pages = defaultdict(list)

    for qid in all_stats[64]:
        target_recall = baseline_recall[qid]

        min_b = None
        for b in budgets:
            if qid in all_stats[b]:
                if all_stats[b][qid]['recall'] >= target_recall - 0.001:
                    min_b = b
                    break

        if min_b is None:
            min_b = 128 + 1

        min_b_distribution[min_b] += 1
        if min_b in all_stats:
            min_b_pages[min_b].append(all_stats[min_b][qid]['pages_read'])

    # Print distribution
    total_queries = len(all_stats[64])
    print(f"{'Budget':<10} {'Count':>8} {'%':>8} {'Cum %':>8}")
    print("-" * 40)

    cum_count = 0
    for b in budgets + [129]:
        count = min_b_distribution[b]
        cum_count += count
        pct = 100 * count / total_queries
        cum_pct = 100 * cum_count / total_queries

        label = str(b) if b != 129 else ">128"
        print(f"{label:<10} {count:>8} {pct:>7.1f}% {cum_pct:>7.1f}%")

    # Oracle analysis
    print()
    print("=" * 60)
    print("SIFT10M Oracle Analysis")
    print("=" * 60)
    print()

    total_oracle_pages = 0
    for b in budgets:
        if b in min_b_pages:
            total_oracle_pages += sum(min_b_pages[b])

    for qid in range(total_queries):
        if qid in all_stats[128] and min_b_distribution[129] > 0:
            pass  # Would need to track individual queries

    oracle_avg_pages = total_oracle_pages / total_queries
    baseline_pages = sum(all_stats[64][qid]['pages_read'] for qid in all_stats[64]) / total_queries

    print(f"Baseline (B=64) pages/query: {baseline_pages:.2f}")
    print(f"Oracle pages/query: {oracle_avg_pages:.2f}")
    print(f"Oracle saving: {100 * (baseline_pages - oracle_avg_pages) / baseline_pages:.1f}%")
    print()

    # Fixed B=48 comparison
    b48_pages = sum(all_stats[48][qid]['pages_read'] for qid in all_stats[48]) / total_queries
    b48_recall = sum(all_stats[48][qid]['recall'] for qid in all_stats[48]) / total_queries

    print(f"Fixed B=48 pages/query: {b48_pages:.2f} ({100 * (baseline_pages - b48_pages) / baseline_pages:.1f}% saving)")
    print(f"Fixed B=48 avg recall: {b48_recall:.4f}")
    print()

    # Decision criteria
    print("=" * 60)
    print("Decision Criteria Check")
    print("=" * 60)
    print()

    easy_40 = sum(min_b_distribution[b] for b in [16, 32, 40])
    pct_easy_40 = 100 * easy_40 / total_queries
    print(f">=30% query min_B <= 40: {pct_easy_40:.1f}% ({'PASS' if pct_easy_40 >= 30 else 'FAIL'})")

    easy_48 = sum(min_b_distribution[b] for b in [16, 32, 40, 48])
    pct_easy_48 = 100 * easy_48 / total_queries
    print(f">=50% query min_B <= 48: {pct_easy_48:.1f}% ({'PASS' if pct_easy_48 >= 50 else 'FAIL'})")

    oracle_saving = 100 * (baseline_pages - oracle_avg_pages) / baseline_pages
    print(f"Oracle pages saving >= 15%: {oracle_saving:.1f}% ({'PASS' if oracle_saving >= 15 else 'FAIL'})")

    print()
    print("=" * 60)
    print("SIFT1M vs SIFT10M Comparison")
    print("=" * 60)
    print()
    print("                    SIFT1M      SIFT10M")
    print(f"min_B<=40:          81.6%       {pct_easy_40:.1f}%")
    print(f"min_B<=48:          88.8%       {pct_easy_48:.1f}%")
    print(f"Oracle saving:      50.8%       {oracle_saving:.1f}%")
    print(f"Baseline recall:    0.9786      {sum(baseline_recall.values())/len(baseline_recall):.4f}")


if __name__ == '__main__':
    main()
