#!/usr/bin/env python3
"""
Conservative fallback rules: only reduce budget for very confident easy queries.
Check recall tail distribution.
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


def evaluate_conservative(features_df, budget_stats, pages_data, rule_func):
    """Evaluate with detailed recall tail analysis."""
    baseline_b = 64
    baseline_recall = budget_stats[baseline_b]
    baseline_pages = np.mean([pages_data[baseline_b].get(qid, 0) for qid in baseline_recall])

    recalls = []
    baseline_recalls = []
    pages_list = []
    missed_count = 0

    for _, row in features_df.iterrows():
        qid = int(row['query_id'])
        budget = rule_func(row, features_df)

        pages = pages_data.get(budget, {}).get(qid, 0)
        pages_list.append(pages)

        actual_recall = budget_stats.get(budget, {}).get(qid, 0)
        target_recall = budget_stats.get(baseline_b, {}).get(qid, 0)

        recalls.append(actual_recall)
        baseline_recalls.append(target_recall)

        if actual_recall < target_recall - 0.001:
            missed_count += 1

    n = len(features_df)
    pages_saving = 100 * (baseline_pages - np.mean(pages_list)) / baseline_pages

    recall_deltas = [r - b for r, b in zip(recalls, baseline_recalls)]

    return {
        'pages_saving': pages_saving,
        'miss_count': missed_count,
        'miss_rate': 100 * missed_count / n,
        'recall_delta_mean': np.mean(recall_deltas),
        'recall_delta_min': np.min(recall_deltas),
        'recall_delta_p5': np.percentile(recall_deltas, 5),
        'recall_delta_p10': np.percentile(recall_deltas, 10),
        'recall_at_budget': budget_stats,
    }


def main():
    print("="*80)
    print("Conservative Fallback Rules with Recall Tail Analysis")
    print("="*80)

    s1_feat, s1_stats, s1_pages = load_data('sift1m')
    s10_feat, s10_stats, s10_pages = load_data('sift10m')

    print(f"SIFT1M: {len(s1_feat)} queries")
    print(f"SIFT10M: {len(s10_feat)} queries")

    # Conservative Rule: Only reduce for top 30% margin_16 AND d1 <= median
    # This requires the query to be both "easy" (high margin) and "close" (low absolute distance)

    print("\n" + "="*80)
    print("Rule: Two-condition conservative (margin_16 percentile AND d1 percentile)")
    print("="*80)
    print(f"{'Conditions':<40} {'S1 Save':>8} {'S1 Miss':>8} {'S1 P5':>8} {'S10 Save':>8} {'S10 Miss':>8} {'S10 P5':>8}")
    print("-"*80)

    for margin_pct in [20, 30, 40]:
        for d1_pct in [30, 50, 70]:
            margin_thresh_s1 = s1_feat['margin_16'].quantile(1 - margin_pct/100)
            d1_thresh_s1 = s1_feat['d1'].quantile(d1_pct/100)  # lower is better

            margin_thresh_s10 = s10_feat['margin_16'].quantile(1 - margin_pct/100)
            d1_thresh_s10 = s10_feat['d1'].quantile(d1_pct/100)

            def rule_s1(row, df):
                if row['margin_16'] >= margin_thresh_s1 and row['d1'] <= d1_thresh_s1:
                    return 48
                return 64

            def rule_s10(row, df):
                if row['margin_16'] >= margin_thresh_s10 and row['d1'] <= d1_thresh_s10:
                    return 48
                return 64

            r1 = evaluate_conservative(s1_feat, s1_stats, s1_pages, rule_s1)
            r10 = evaluate_conservative(s10_feat, s10_stats, s10_pages, rule_s10)

            status = ""
            if r1['pages_saving'] >= 8 and r1['miss_rate'] <= 2 and r10['pages_saving'] >= 8 and r10['miss_rate'] <= 2:
                status = " ✅"

            print(f"margin>={margin_pct}% & d1<={d1_pct}%           {r1['pages_saving']:>6.1f}% {r1['miss_rate']:>6.1f}% {r1['recall_delta_p5']:>7.3f} "
                  f"{r10['pages_saving']:>6.1f}% {r10['miss_rate']:>6.1f}% {r10['recall_delta_p5']:>7.3f}{status}")

    # Alternative: Budget based on min_B prediction confidence
    print("\n" + "="*80)
    print("Analysis: What if we knew min_B from oracle?")
    print("="*80)

    # For each dataset, what's the theoretical best with conservative policy?
    # Conservative = only reduce if oracle says min_B < 64

    for name, feat, stats, pages in [('SIFT1M', s1_feat, s1_stats, s1_pages),
                                      ('SIFT10M', s10_feat, s10_stats, s10_pages)]:
        baseline_recall = stats[64]

        # Compute min_B
        min_b = {}
        for qid in baseline_recall:
            target = baseline_recall[qid] - 0.001
            for b in [16, 32, 40, 48, 64, 80, 96, 128]:
                if qid in stats.get(b, {}):
                    if stats[b][qid] >= target:
                        min_b[qid] = b
                        break
            if qid not in min_b:
                min_b[qid] = 129

        feat['min_B'] = feat['query_id'].map(min_b)

        # Conservative oracle: if min_B < 64, use min_B; else use 64
        total_pages = 0
        baseline_total = 0
        n = len(feat)

        for _, row in feat.iterrows():
            qid = int(row['query_id'])
            oracle_b = row['min_B']

            if pd.isna(oracle_b) or oracle_b > 64:
                use_b = 64
            else:
                use_b = min(int(oracle_b), 64)  # conservative: at most 64

            total_pages += pages.get(use_b, {}).get(qid, 0)
            baseline_total += pages.get(64, {}).get(qid, 0)

        saving = 100 * (baseline_total - total_pages) / baseline_total

        # Count queries with min_B <= 48 (can safely reduce)
        safe_reduce = (feat['min_B'] <= 48).sum()
        safe_reduce_pct = 100 * safe_reduce / n

        print(f"\n{name} Conservative Oracle Analysis:")
        print(f"  Queries with min_B <= 48: {safe_reduce} ({safe_reduce_pct:.1f}%)")
        print(f"  Conservative oracle pages saving: {saving:.1f}%")

        # Break down by min_B
        print(f"  min_B distribution:")
        for b in [16, 32, 40, 48, 64]:
            count = (feat['min_B'] == b).sum()
            pct = 100 * count / n
            print(f"    B={b}: {count} ({pct:.1f}%)")

    # Final recommendation
    print("\n" + "="*80)
    print("Final Recommendation")
    print("="*80)

    # Check if any combination works
    print("\nConservative oracle analysis shows the theoretical ceiling.")
    print("If even conservative oracle can't achieve >=8% saving with <=2% miss,")
    print("then no feature-based rule can work.")

    # Compute ceiling
    print("\nCompute theoretical ceiling for SIFT10M:")
    s10_feat['min_B'] = s10_feat['query_id'].map(
        {qid: next((b for b in [16, 32, 40, 48, 64, 80, 96, 128]
                    if s10_stats.get(b, {}).get(qid, 0) >= target - 0.001), 129)
         for qid, target in s10_stats[64].items()}
    )

    safe_reduce = (s10_feat['min_B'] <= 48).sum()
    print(f"  SIFT10M queries with min_B <= 48: {safe_reduce} ({100*safe_reduce/len(s10_feat):.1f}%)")

    # What would be the saving if we use B=48 for these queries?
    # Need to compute actual pages savings for these specific queries


if __name__ == '__main__':
    main()
