#!/usr/bin/env python3
"""
Validate percentile-based and conservative adaptive budget rules.

Rules to test:
1. Percentile-based: top P% easiest queries (by margin_16) -> B=48/32
2. Normalized features: margin_16/d1, margin_16/margin_64 percentile
3. Conservative: only reduce easy query budget, never increase

Criteria: Pages saving >= 8%, Miss rate <= 2% on BOTH SIFT1M and SIFT10M
"""

import pandas as pd
import numpy as np
from collections import defaultdict


def load_features_and_budget_stats(dataset):
    """Load features and budget stats for a dataset."""
    if dataset == 'sift1m':
        trace_path = 'results/adaptive_budget/phase2_feature_extraction/head_distance_trace.csv'
        budget_path = 'results/adaptive_budget/budget_sweep'
    else:
        trace_path = 'results/adaptive_budget/sift10m_phase3_test/head_distance_trace.csv'
        budget_path = 'results/adaptive_budget/sift10m_budget_sweep'

    # Load trace
    trace_df = pd.read_csv(trace_path)

    # Extract features
    features = []
    for qid in trace_df['query_id'].unique():
        query_trace = trace_df[trace_df['query_id'] == qid].sort_values('posting_index')
        dists = query_trace['head_dist'].values

        if len(dists) == 0:
            continue

        d1 = dists[0] if len(dists) > 0 else np.nan
        d16 = dists[15] if len(dists) > 15 else np.nan
        d32 = dists[31] if len(dists) > 31 else np.nan
        d64 = dists[63] if len(dists) > 63 else np.nan

        margin_16 = (d16 - d1) / d1 if d1 > 0 and not np.isnan(d16) else np.nan
        margin_32 = (d32 - d1) / d1 if d1 > 0 and not np.isnan(d32) else np.nan
        margin_64 = (d64 - d1) / d1 if d1 > 0 and not np.isnan(d64) else np.nan

        # Normalized features
        margin_16_norm_d1 = margin_16  # already normalized by d1
        margin_16_norm_m64 = margin_16 / margin_64 if margin_64 > 0 and not np.isnan(margin_64) else np.nan

        features.append({
            'query_id': qid,
            'd1': d1,
            'd16': d16,
            'd32': d32,
            'd64': d64,
            'margin_16': margin_16,
            'margin_32': margin_32,
            'margin_64': margin_64,
            'margin_16_norm_d1': margin_16_norm_d1,
            'margin_16_norm_m64': margin_16_norm_m64,
            'dist_var_16': np.var(dists[:16]) if len(dists) >= 16 else np.nan,
        })

    features_df = pd.DataFrame(features)

    # Load budget stats
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}

    for b in budgets:
        try:
            df = pd.read_csv(f'{budget_path}/query_io_stats_b{b}.csv')
            budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
            pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()
        except:
            pass

    return features_df, budget_stats, pages_data


def evaluate_rule(features_df, budget_stats, pages_data, rule_func, baseline_b=64):
    """Evaluate a budget rule."""
    baseline_recall = budget_stats[baseline_b]
    baseline_pages = np.mean([pages_data[baseline_b].get(qid, 0) for qid in baseline_recall])

    total_pages = 0
    missed = 0
    budget_dist = defaultdict(int)

    for _, row in features_df.iterrows():
        qid = int(row['query_id'])
        budget = rule_func(row)

        budget_dist[budget] += 1

        pages = pages_data.get(budget, {}).get(qid, 0)
        total_pages += pages

        # Check recall miss
        actual_recall = budget_stats.get(budget, {}).get(qid, 0)
        target_recall = budget_stats.get(baseline_b, {}).get(qid, 0)
        if actual_recall < target_recall - 0.001:
            missed += 1

    n_queries = len(features_df)
    avg_pages = total_pages / n_queries
    pages_saving = 100 * (baseline_pages - avg_pages) / baseline_pages
    miss_rate = 100 * missed / n_queries

    return {
        'pages_saving': pages_saving,
        'miss_rate': miss_rate,
        'missed': missed,
        'n_queries': n_queries,
        'budget_dist': dict(budget_dist),
    }


def test_percentile_rule(features_df, percentile, easy_budget, hard_budget, feature='margin_16'):
    """Create a percentile-based rule function."""
    threshold = features_df[feature].quantile(1 - percentile/100)

    def rule(row):
        if row[feature] >= threshold:
            return easy_budget
        return hard_budget

    return rule, threshold


def main():
    print("="*80)
    print("Percentile-based and Conservative Adaptive Budget Rules")
    print("="*80)

    # Load both datasets
    print("\nLoading SIFT1M...")
    sift1m_features, sift1m_stats, sift1m_pages = load_features_and_budget_stats('sift1m')
    print(f"  {len(sift1m_features)} queries")

    print("Loading SIFT10M...")
    sift10m_features, sift10m_stats, sift10m_pages = load_features_and_budget_stats('sift10m')
    print(f"  {len(sift10m_features)} queries")

    # Test different percentile rules
    print("\n" + "="*80)
    print("Rule 1: Percentile-based margin_16 (top P% -> B=48, else B=64)")
    print("="*80)
    print(f"{'Percentile':<12} {'SIFT1M Saving':>14} {'SIFT1M Miss':>12} {'SIFT10M Saving':>14} {'SIFT10M Miss':>12} {'Status':>10}")
    print("-"*80)

    results = []
    for pct in [20, 30, 40, 50, 60]:
        # SIFT1M
        rule1, thresh1 = test_percentile_rule(sift1m_features, pct, 48, 64)
        r1 = evaluate_rule(sift1m_features, sift1m_stats, sift1m_pages, rule1)

        # SIFT10M
        rule10, thresh10 = test_percentile_rule(sift10m_features, pct, 48, 64)
        r10 = evaluate_rule(sift10m_features, sift10m_stats, sift10m_pages, rule10)

        # Check criteria
        pass1 = r1['pages_saving'] >= 8 and r1['miss_rate'] <= 2
        pass10 = r10['pages_saving'] >= 8 and r10['miss_rate'] <= 2
        status = "✅ PASS" if pass1 and pass10 else ""

        print(f"top {pct}%      {r1['pages_saving']:>13.1f}% {r1['miss_rate']:>11.1f}% "
              f"{r10['pages_saving']:>13.1f}% {r10['miss_rate']:>11.1f}% {status:>10}")

        results.append({
            'rule': f'percentile_{pct}',
            'sift1m_saving': r1['pages_saving'],
            'sift1m_miss': r1['miss_rate'],
            'sift10m_saving': r10['pages_saving'],
            'sift10m_miss': r10['miss_rate'],
        })

    # Test two-tier percentile rule
    print("\n" + "="*80)
    print("Rule 2: Two-tier percentile (top P1% -> B=32, next P2% -> B=48, else B=64)")
    print("="*80)
    print(f"{'Tiers':<15} {'SIFT1M Saving':>14} {'SIFT1M Miss':>12} {'SIFT10M Saving':>14} {'SIFT10M Miss':>12} {'Status':>10}")
    print("-"*80)

    for p1, p2 in [(20, 30), (20, 40), (30, 40)]:
        def make_two_tier(features, p1, p2):
            t1 = features['margin_16'].quantile(1 - p1/100)
            t2 = features['margin_16'].quantile(1 - (p1+p2)/100)
            return t1, t2

        t1_s1, t2_s1 = make_two_tier(sift1m_features, p1, p2)
        t1_s10, t2_s10 = make_two_tier(sift10m_features, p1, p2)

        def rule_s1(row):
            if row['margin_16'] >= t1_s1:
                return 32
            elif row['margin_16'] >= t2_s1:
                return 48
            return 64

        def rule_s10(row):
            if row['margin_16'] >= t1_s10:
                return 32
            elif row['margin_16'] >= t2_s10:
                return 48
            return 64

        r1 = evaluate_rule(sift1m_features, sift1m_stats, sift1m_pages, rule_s1)
        r10 = evaluate_rule(sift10m_features, sift10m_stats, sift10m_pages, rule_s10)

        pass1 = r1['pages_saving'] >= 8 and r1['miss_rate'] <= 2
        pass10 = r10['pages_saving'] >= 8 and r10['miss_rate'] <= 2
        status = "✅ PASS" if pass1 and pass10 else ""

        print(f"top {p1}%->32, next {p2}%->48 {r1['pages_saving']:>10.1f}% {r1['miss_rate']:>11.1f}% "
              f"{r10['pages_saving']:>13.1f}% {r10['miss_rate']:>11.1f}% {status:>10}")

    # Test normalized features
    print("\n" + "="*80)
    print("Rule 3: Normalized feature (margin_16/margin_64 percentile)")
    print("="*80)
    print(f"{'Percentile':<12} {'SIFT1M Saving':>14} {'SIFT1M Miss':>12} {'SIFT10M Saving':>14} {'SIFT10M Miss':>12} {'Status':>10}")
    print("-"*80)

    # Filter valid
    s1_valid = sift1m_features[sift1m_features['margin_16_norm_m64'].notna()]
    s10_valid = sift10m_features[sift10m_features['margin_16_norm_m64'].notna()]

    for pct in [20, 30, 40, 50]:
        rule1, _ = test_percentile_rule(s1_valid, pct, 48, 64, 'margin_16_norm_m64')
        r1 = evaluate_rule(s1_valid, sift1m_stats, sift1m_pages, rule1)

        rule10, _ = test_percentile_rule(s10_valid, pct, 48, 64, 'margin_16_norm_m64')
        r10 = evaluate_rule(s10_valid, sift10m_stats, sift10m_pages, rule10)

        pass1 = r1['pages_saving'] >= 8 and r1['miss_rate'] <= 2
        pass10 = r10['pages_saving'] >= 8 and r10['miss_rate'] <= 2
        status = "✅ PASS" if pass1 and pass10 else ""

        print(f"top {pct}%      {r1['pages_saving']:>13.1f}% {r1['miss_rate']:>11.1f}% "
              f"{r10['pages_saving']:>13.1f}% {r10['miss_rate']:>11.1f}% {status:>10}")

    # Summary
    print("\n" + "="*80)
    print("Summary")
    print("="*80)

    # Find best rule
    best = None
    for r in results:
        if r['sift1m_saving'] >= 8 and r['sift1m_miss'] <= 2 and r['sift10m_saving'] >= 8 and r['sift10m_miss'] <= 2:
            if best is None or r['sift1m_saving'] + r['sift10m_saving'] > best['sift1m_saving'] + best['sift10m_saving']:
                best = r

    if best:
        print(f"✅ Found rule that passes both datasets: {best['rule']}")
        print(f"   SIFT1M: {best['sift1m_saving']:.1f}% saving, {best['sift1m_miss']:.1f}% miss")
        print(f"   SIFT10M: {best['sift10m_saving']:.1f}% saving, {best['sift10m_miss']:.1f}% miss")
    else:
        print("❌ No percentile rule passes both datasets with >=8% saving and <=2% miss")
        print("\nBest results per dataset:")
        best_s1 = max(results, key=lambda r: r['sift1m_saving'] if r['sift1m_miss'] <= 2 else 0)
        best_s10 = max(results, key=lambda r: r['sift10m_saving'] if r['sift10m_miss'] <= 2 else 0)
        print(f"  SIFT1M best: {best_s1['rule']} -> {best_s1['sift1m_saving']:.1f}% saving, {best_s1['sift1m_miss']:.1f}% miss")
        print(f"  SIFT10M best: {best_s10['rule']} -> {best_s10['sift10m_saving']:.1f}% saving, {best_s10['sift10m_miss']:.1f}% miss")


if __name__ == '__main__':
    main()
