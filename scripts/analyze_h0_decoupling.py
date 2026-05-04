#!/usr/bin/env python3
"""
Analyze query-level recall difference between ir=64 and ir=128.
No pandas dependency.
"""

import csv
import sys
from collections import defaultdict


def main():
    # Read query stats
    data_64 = {}
    with open('results/adaptive_budget/h0_decoupling/query_io_stats_ir64.csv', 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            qid = int(row['query_id'])
            data_64[qid] = {
                'recall': float(row['recall']),
                'postings_touched': int(row['postings_touched']),
                'pages_read': int(row['pages_read'])
            }

    data_128 = {}
    with open('results/adaptive_budget/h0_decoupling/query_io_stats_ir128.csv', 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            qid = int(row['query_id'])
            data_128[qid] = {
                'recall': float(row['recall']),
                'postings_touched': int(row['postings_touched']),
                'pages_read': int(row['pages_read'])
            }

    # Merge and analyze
    total = len(data_64)

    # Category counts
    already_perfect = 0
    fixed_by_128 = 0
    improved_by_128 = 0
    still_unresolved = 0

    # Recall distribution at ir=64
    recall_bins_64 = defaultdict(int)

    # Low recall queries
    low_recall_at_128 = []

    # Pages stats
    pages_64 = []
    pages_128 = []

    for qid in data_64:
        r64 = data_64[qid]['recall']
        r128 = data_128[qid]['recall']
        delta = r128 - r64

        pages_64.append(data_64[qid]['pages_read'])
        pages_128.append(data_128[qid]['pages_read'])

        # Categorize
        if r64 == 1.0:
            already_perfect += 1
        elif r128 == 1.0:
            fixed_by_128 += 1
        elif delta > 0:
            improved_by_128 += 1
        else:
            still_unresolved += 1

        # Recall bins
        if r64 < 0.5:
            recall_bins_64['<0.5'] += 1
        elif r64 < 0.7:
            recall_bins_64['0.5-0.7'] += 1
        elif r64 < 0.8:
            recall_bins_64['0.7-0.8'] += 1
        elif r64 < 0.9:
            recall_bins_64['0.8-0.9'] += 1
        elif r64 < 0.95:
            recall_bins_64['0.9-0.95'] += 1
        elif r64 < 1.0:
            recall_bins_64['0.95-1.0'] += 1
        else:
            recall_bins_64['1.0'] += 1

        # Track low recall at ir=128
        if r128 < 0.9:
            low_recall_at_128.append((qid, r64, r128))

    # Print results
    print("=" * 60)
    print("Query-level Recall Analysis: ir=64 vs ir=128")
    print("=" * 60)
    print()

    print(f"Total queries: {total}")
    print()

    print("Query Category Distribution:")
    print(f"  already_perfect_64: {already_perfect} ({100*already_perfect/total:.1f}%)")
    print(f"  fixed_by_128:       {fixed_by_128} ({100*fixed_by_128/total:.1f}%)")
    print(f"  improved_by_128:    {improved_by_128} ({100*improved_by_128/total:.1f}%)")
    print(f"  still_unresolved:   {still_unresolved} ({100*still_unresolved/total:.1f}%)")
    print()

    print("Recall Distribution at ir=64:")
    for bin_name in ['<0.5', '0.5-0.7', '0.7-0.8', '0.8-0.9', '0.9-0.95', '0.95-1.0', '1.0']:
        count = recall_bins_64[bin_name]
        pct = 100 * count / total
        print(f"  {bin_name:>10}: {count:5d} ({pct:5.1f}%)")
    print()

    # Improved queries
    improved_count = fixed_by_128 + improved_by_128
    print(f"Queries improved by ir=128: {improved_count} ({100*improved_count/total:.1f}%)")
    print()

    # Low recall at ir=128
    print(f"Queries with recall < 0.9 at ir=128: {len(low_recall_at_128)} ({100*len(low_recall_at_128)/total:.1f}%)")
    if len(low_recall_at_128) > 0 and len(low_recall_at_128) <= 10:
        print("  Details:")
        for qid, r64, r128 in sorted(low_recall_at_128, key=lambda x: x[2]):
            print(f"    query {qid}: ir64={r64:.2f}, ir128={r128:.2f}")
    print()

    # Pages stats
    avg_pages_64 = sum(pages_64) / len(pages_64)
    avg_pages_128 = sum(pages_128) / len(pages_128)
    print("Pages/query Comparison:")
    print(f"  ir=64:  {avg_pages_64:.2f} avg")
    print(f"  ir=128: {avg_pages_128:.2f} avg")
    print()

    # Adaptive budget potential
    print("=" * 60)
    print("Adaptive Budget Potential Analysis")
    print("=" * 60)
    print()

    print(f"Queries with perfect recall at ir=64: {already_perfect} ({100*already_perfect/total:.1f}%)")
    print("  These might be candidates for B < 64")
    print()

    benefit_from_more = fixed_by_128 + improved_by_128
    print(f"Queries that benefit from B > 64: {benefit_from_more} ({100*benefit_from_more/total:.1f}%)")
    print()

    print("=" * 60)
    print("Conclusion")
    print("=" * 60)
    print()
    print(f"1. {100*already_perfect/total:.1f}% queries have perfect recall at B=64")
    print(f"2. {100*improved_count/total:.1f}% queries improved by B=128")
    print(f"3. {100*len(low_recall_at_128)/total:.1f}% queries still have low recall even at B=128")
    print()
    print("This suggests adaptive budget has potential:")
    print("- Easy queries (perfect at B=64) could potentially use B < 64")
    print("- Hard queries could use B > 64 to improve recall")
    print()
    print("Next step: Run offline oracle to find min_B for each query")


if __name__ == '__main__':
    main()
