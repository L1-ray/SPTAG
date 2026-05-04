#!/usr/bin/env python3
"""
Find optimal threshold for SIFT10M by analyzing phase2 features.
"""

import pandas as pd
import numpy as np


def main():
    # Load SIFT10M features
    features = pd.read_csv('results/adaptive_budget/phase2_feature_extraction/query_features.csv')

    # Load SIFT10M budget sweep stats
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    for b in budgets:
        try:
            df = pd.read_csv(f'results/adaptive_budget/sift10m_budget_sweep/query_io_stats_b{b}.csv')
            budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
        except:
            pass

    # Compute min_B for SIFT10M baseline (B=64)
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

    # Filter valid queries
    valid = features[features['min_B'].notna() & (features['min_B'] <= 128)].copy()
    print(f"SIFT10M: {len(valid)} valid queries")

    # Test different thresholds
    thresholds = [0.20, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50]

    print("\n" + "="*80)
    print("Threshold Sweep for margin_16 -> B=48 rule")
    print("="*80)
    print(f"{'Threshold':<10} {'Easy %':>10} {'Pages Saving':>12} {'Recall Miss':>12} {'Miss Rate':>10}")
    print("-"*80)

    # Load SIFT10M budget sweep pages data
    pages_data = {}
    for b in budgets:
        df = pd.read_csv(f'results/adaptive_budget/sift10m_budget_sweep/query_io_stats_b{b}.csv')
        pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in baseline_recall])

    for t in thresholds:
        easy_mask = valid['margin_16'] >= t
        easy_count = easy_mask.sum()
        easy_pct = 100 * easy_count / len(valid)

        # Compute pages saving
        total_pages = 0
        missed = 0
        for _, row in valid.iterrows():
            qid = row['query_id']
            if row['margin_16'] >= t:
                budget = 48
            else:
                budget = 64

            pages = pages_data.get(budget, {}).get(qid, 0)
            total_pages += pages

            # Check recall miss
            actual_recall = budget_stats.get(budget, {}).get(qid, 0)
            target_recall = budget_stats.get(64, {}).get(qid, 0)
            if actual_recall < target_recall - 0.001:
                missed += 1

        avg_pages = total_pages / len(valid)
        pages_saving = 100 * (baseline_pages - avg_pages) / baseline_pages
        miss_rate = 100 * missed / len(valid)

        print(f"{t:<10.2f} {easy_pct:>9.1f}% {pages_saving:>11.1f}% {missed:>12} {miss_rate:>9.1f}%")

    # Compare with fixed B=48
    print("\n" + "="*80)
    print("Fixed budget comparison")
    print("="*80)

    for fixed_b in [48, 64]:
        total_pages = sum(pages_data.get(fixed_b, {}).get(qid, 0) for qid in baseline_recall)
        avg_pages = total_pages / len(baseline_recall)
        pages_saving = 100 * (baseline_pages - avg_pages) / baseline_pages

        missed = sum(1 for qid in baseline_recall
                     if budget_stats.get(fixed_b, {}).get(qid, 0) < budget_stats.get(64, {}).get(qid, 0) - 0.001)
        miss_rate = 100 * missed / len(baseline_recall)

        print(f"Fixed B={fixed_b}: pages_saving={pages_saving:.1f}%, miss_rate={miss_rate:.1f}%")


if __name__ == '__main__':
    main()
