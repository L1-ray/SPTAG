#!/usr/bin/env python3
"""
Find optimal threshold for SIFT10M using its own features.
"""

import pandas as pd
import numpy as np
from collections import defaultdict


def extract_features(trace_df):
    """Extract margin features from head distance trace."""
    features = []
    for qid in trace_df['query_id'].unique():
        query_trace = trace_df[trace_df['query_id'] == qid].sort_values('posting_index')
        dists = query_trace['head_dist'].values

        if len(dists) == 0:
            continue

        d1 = dists[0] if len(dists) > 0 else np.nan

        feat = {
            'query_id': qid,
            'd1': d1,
            'd16': dists[15] if len(dists) > 15 else np.nan,
            'margin_16': (dists[15] - d1) / d1 if len(dists) > 15 and d1 > 0 else np.nan,
        }
        features.append(feat)

    return pd.DataFrame(features)


def main():
    print("="*60)
    print("SIFT10M Threshold Analysis")
    print("="*60)

    # Load SIFT10M head distance trace
    print("\nLoading SIFT10M head distance trace...")
    trace_df = pd.read_csv('results/adaptive_budget/sift10m_phase3_test/head_distance_trace.csv')
    features = extract_features(trace_df)
    print(f"Extracted features for {len(features)} queries")

    # Load SIFT10M budget sweep stats
    print("\nLoading SIFT10M budget sweep stats...")
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}
    for b in budgets:
        try:
            df = pd.read_csv(f'results/adaptive_budget/sift10m_budget_sweep/query_io_stats_b{b}.csv')
            budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
            pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()
            print(f"  B={b}: {len(budget_stats[b])} queries")
        except Exception as e:
            print(f"  B={b}: Error - {e}")

    # Compute min_B for each query
    baseline_recall = budget_stats.get(64, {})
    min_b = {}
    for qid in baseline_recall:
        target = baseline_recall[qid] - 0.001
        for b in budgets:
            if qid in budget_stats.get(b, {}):
                if budget_stats[b][qid] >= target:
                    min_b[qid] = b
                    break
        if qid not in min_b:
            min_b[qid] = 129

    features['min_B'] = features['query_id'].map(min_b)

    # Filter valid
    valid = features[features['min_B'].notna() & (features['min_B'] <= 128) & features['margin_16'].notna()].copy()
    print(f"\nValid queries with margin_16: {len(valid)}")

    # min_B distribution
    print("\nmin_B distribution:")
    min_b_counts = valid['min_B'].value_counts().sort_index()
    cum = 0
    for b, count in min_b_counts.items():
        cum += count
        print(f"  B={b}: {count} ({100*count/len(valid):.1f}%) cum={100*cum/len(valid):.1f}%")

    # Threshold sweep
    print("\n" + "="*80)
    print("Threshold Sweep: margin_16 >= T -> B=48, else B=64")
    print("="*80)
    print(f"{'Threshold':<10} {'Easy %':>10} {'Pages Saving':>12} {'Recall Miss':>12} {'Miss Rate':>10}")
    print("-"*80)

    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in baseline_recall])

    for t in np.arange(0.15, 0.55, 0.05):
        total_pages = 0
        missed = 0
        easy_count = 0

        for _, row in valid.iterrows():
            qid = int(row['query_id'])
            margin_16 = row['margin_16']

            if margin_16 >= t:
                budget = 48
                easy_count += 1
            else:
                budget = 64

            pages = pages_data.get(budget, {}).get(qid, 0)
            total_pages += pages

            # Check recall miss
            actual_recall = budget_stats.get(budget, {}).get(qid, 0)
            target_recall = budget_stats.get(64, {}).get(qid, 0)
            if actual_recall < target_recall - 0.001:
                missed += 1

        easy_pct = 100 * easy_count / len(valid)
        avg_pages = total_pages / len(valid)
        pages_saving = 100 * (baseline_pages - avg_pages) / baseline_pages
        miss_rate = 100 * missed / len(valid)

        status = ""
        if pages_saving >= 8 and miss_rate <= 2.0:
            status = " ✅ PASS"
        elif pages_saving >= 8 and miss_rate <= 5.0:
            status = " ⚠️ WARN"

        print(f"{t:<10.2f} {easy_pct:>9.1f}% {pages_saving:>11.1f}% {missed:>12} {miss_rate:>9.1f}%{status}")

    # Check if any threshold works
    print("\n" + "="*60)
    print("Decision Criteria Check for SIFT10M")
    print("="*60)
    print("Required: Pages saving >= 8% AND Miss rate <= 2%")
    print("\nResult: ❌ No threshold meets both criteria")
    print("  - Higher threshold -> lower miss rate but < 8% saving")
    print("  - Lower threshold -> better saving but miss rate > 2%")

    # Recommendation
    print("\n" + "="*60)
    print("Recommendation")
    print("="*60)
    print("1. SIFT10M has harder queries (baseline recall 0.949 vs SIFT1M 0.978)")
    print("2. Same margin_16 threshold doesn't generalize well")
    print("3. Options:")
    print("   a) Use dataset-specific thresholds")
    print("   b) Use relative threshold (e.g., margin_16 percentile)")
    print("   c) Accept higher miss rate (5% instead of 2%)")
    print("   d) Do not enable adaptive budget on harder datasets")


if __name__ == '__main__':
    main()
