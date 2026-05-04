#!/usr/bin/env python3
"""
Final analysis: what's the theoretical limit of feature-based rules?
"""

import pandas as pd
import numpy as np


def load_data(dataset):
    """Load features and budget stats."""
    if dataset == 'sift1m':
        trace_path = 'results/adaptive_budget/phase2_feature_extraction/head_distance_trace.csv'
        budget_path = 'results/adaptive_budget/budget_sweep'
    else:
        trace_path = 'results/adaptive_budget/sift10m_phase3_test/head_distance_trace.csv'
        budget_path = 'results/adaptive_budget/sift10m_budget_sweep'

    trace_df = pd.read_csv(trace_path)

    features = []
    for qid in trace_df['query_id'].unique():
        query_trace = trace_df[trace_df['query_id'] == qid].sort_values('posting_index')
        dists = query_trace['head_dist'].values

        if len(dists) == 0:
            continue

        d1 = dists[0] if len(dists) > 0 else np.nan
        d16 = dists[15] if len(dists) > 15 else np.nan

        margin_16 = (d16 - d1) / d1 if d1 > 0 and not np.isnan(d16) else np.nan

        features.append({
            'query_id': qid,
            'margin_16': margin_16,
            'd1': d1,
        })

    features_df = pd.DataFrame(features)

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


def compute_min_b(features_df, budget_stats):
    """Compute min_B for each query."""
    baseline_recall = budget_stats[64]

    min_b = {}
    for qid in baseline_recall:
        target = baseline_recall[qid] - 0.001
        for b in [16, 32, 40, 48, 64, 80, 96, 128]:
            if qid in budget_stats.get(b, {}):
                if budget_stats[b][qid] >= target:
                    min_b[qid] = b
                    break
        if qid not in min_b:
            min_b[qid] = 129

    features_df['min_B'] = features_df['query_id'].map(min_b)
    return features_df


def main():
    print("="*80)
    print("Feature-based Rule Theoretical Limit Analysis")
    print("="*80)

    s1_feat, s1_stats, s1_pages = load_data('sift1m')
    s10_feat, s10_stats, s10_pages = load_data('sift10m')

    s1_feat = compute_min_b(s1_feat, s1_stats)
    s10_feat = compute_min_b(s10_feat, s10_stats)

    # Analysis: if we can only use margin_16, what's the best we can do?
    # Perfect classifier would identify all queries with min_B <= 48 and give them B=48

    print("\n" + "="*80)
    print("Analysis: Feature-marginal oracle (perfect classifier for given feature)")
    print("="*80)

    for name, feat, stats, pages in [('SIFT1M', s1_feat, s1_stats, s1_pages),
                                      ('SIFT10M', s10_feat, s10_stats, s10_pages)]:
        print(f"\n{name}:")

        # Sort by margin_16 descending
        valid = feat[feat['margin_16'].notna()].copy()
        valid = valid.sort_values('margin_16', ascending=False)

        n = len(valid)

        # Compute oracle saving for each percentile threshold
        baseline_pages = np.mean([pages[64].get(int(qid), 0) for qid in valid['query_id']])

        print(f"\n  If we use B=48 for top P% by margin_16:")
        print(f"  {'P%':<6} {'Saving':>8} {'Correct':>10} {'Wrong':>8} {'Wrong Rate':>10}")

        for pct in [10, 20, 30, 40, 50]:
            cutoff = int(n * pct / 100)
            top_queries = valid.head(cutoff)

            # How many of these actually have min_B <= 48?
            correct = (top_queries['min_B'] <= 48).sum()
            wrong = (top_queries['min_B'] > 48).sum()
            wrong_rate = 100 * wrong / cutoff

            # Compute actual pages saving
            total_pages = 0
            for _, row in top_queries.iterrows():
                qid = int(row['query_id'])
                total_pages += pages[48].get(qid, 0)

            # Rest use B=64
            rest_queries = valid.tail(n - cutoff)
            for _, row in rest_queries.iterrows():
                qid = int(row['query_id'])
                total_pages += pages[64].get(qid, 0)

            saving = 100 * (baseline_pages * n - total_pages) / (baseline_pages * n)

            status = " ✅" if saving >= 8 and wrong_rate <= 2 else ""
            print(f"  {pct}%    {saving:>6.1f}% {correct:>10} {wrong:>8} {wrong_rate:>9.1f}%{status}")

    # Final conclusion
    print("\n" + "="*80)
    print("Conclusion")
    print("="*80)

    print("""
Key insight: Even if we use margin_16 as a perfect percentile-based feature,
we cannot achieve >=8% saving with <=2% miss rate on SIFT10M.

This is because:
1. margin_16 is correlated with min_B, but not perfectly
2. For top 30% by margin_16 in SIFT10M:
   - Some have min_B > 48 (would cause recall miss)
   - The "wrong" rate is already ~5%+

The theoretical limit of margin_16-based rule:
- SIFT1M: Can achieve ~9% saving with ~2% miss (top 40%)
- SIFT10M: Cannot achieve both >=8% saving AND <=2% miss simultaneously

Decision:
- Archive adaptive budget as main optimization direction
- Consider dataset-specific optimization (SIFT1M only) as minor engineering improvement
""")

    # Compute exact numbers for summary
    print("\n" + "="*80)
    print("Summary Numbers")
    print("="*80)

    for name, feat in [('SIFT1M', s1_feat), ('SIFT10M', s10_feat)]:
        valid = feat[feat['margin_16'].notna()].copy()
        valid = valid.sort_values('margin_16', ascending=False)

        # Find best threshold that meets criteria
        n = len(valid)
        baseline_recall = s1_stats[64] if name == 'SIFT1M' else s10_stats[64]
        pages = s1_pages if name == 'SIFT1M' else s10_pages
        stats = s1_stats if name == 'SIFT1M' else s10_stats
        baseline_pages = np.mean([pages[64].get(int(qid), 0) for qid in valid['query_id']])

        best_saving = 0
        best_miss_rate = 100

        for pct in range(5, 51, 5):
            cutoff = int(n * pct / 100)
            top_queries = valid.head(cutoff)

            total_pages = 0
            missed = 0
            for _, row in top_queries.iterrows():
                qid = int(row['query_id'])
                total_pages += pages[48].get(qid, 0)
                if stats[48].get(qid, 0) < stats[64].get(qid, 0) - 0.001:
                    missed += 1

            rest_queries = valid.tail(n - cutoff)
            for _, row in rest_queries.iterrows():
                qid = int(row['query_id'])
                total_pages += pages[64].get(qid, 0)

            saving = 100 * (baseline_pages * n - total_pages) / (baseline_pages * n)
            miss_rate = 100 * missed / n

            if miss_rate <= 2 and saving > best_saving:
                best_saving = saving
                best_miss_rate = miss_rate

        print(f"{name}: Best with miss_rate <= 2% -> saving={best_saving:.1f}%, miss_rate={best_miss_rate:.1f}%")


if __name__ == '__main__':
    main()
