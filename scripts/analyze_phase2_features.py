#!/usr/bin/env python3
"""
Phase 2: Feature extraction analysis for adaptive posting budget.
Analyze the correlation between head distance features and min_B.
"""

import pandas as pd
import numpy as np
from collections import defaultdict
import sys


def read_head_distance_trace(filepath):
    """Read head distance trace CSV."""
    print(f"Reading head distance trace from {filepath}...")
    df = pd.read_csv(filepath)
    print(f"  Loaded {len(df)} records, {df['query_id'].nunique()} queries")
    return df


def read_query_io_stats(filepath):
    """Read query IO stats CSV."""
    print(f"Reading query IO stats from {filepath}...")
    df = pd.read_csv(filepath)
    print(f"  Loaded {len(df)} records")
    return df


def read_budget_sweep_stats(budgets, base_path):
    """Read budget sweep stats for multiple budgets."""
    all_stats = {}
    for b in budgets:
        filepath = f"{base_path}/query_io_stats_b{b}.csv"
        try:
            df = pd.read_csv(filepath)
            all_stats[b] = df.set_index('query_id')['recall'].to_dict()
            print(f"  B={b}: {len(df)} queries")
        except Exception as e:
            print(f"  Error reading B={b}: {e}")
    return all_stats


def compute_min_b(budgets, budget_stats, baseline_recall):
    """Compute min_B for each query."""
    min_b = {}
    for qid in baseline_recall:
        target = baseline_recall[qid] - 0.001  # Allow small tolerance
        for b in budgets:
            if qid in budget_stats.get(b, {}):
                if budget_stats[b][qid] >= target:
                    min_b[qid] = b
                    break
        if qid not in min_b:
            min_b[qid] = 129  # > max budget
    return min_b


def extract_head_distance_features(trace_df):
    """Extract features from head distance trace."""
    features = []

    for qid in trace_df['query_id'].unique():
        query_trace = trace_df[trace_df['query_id'] == qid].sort_values('posting_index')
        dists = query_trace['head_dist'].values

        if len(dists) == 0:
            continue

        feat = {
            'query_id': qid,
            'num_postings': len(dists),
            'd1': dists[0] if len(dists) > 0 else np.nan,
            'd2': dists[1] if len(dists) > 1 else np.nan,
            'd4': dists[3] if len(dists) > 3 else np.nan,
            'd8': dists[7] if len(dists) > 7 else np.nan,
            'd16': dists[15] if len(dists) > 15 else np.nan,
            'd32': dists[31] if len(dists) > 31 else np.nan,
            'd64': dists[63] if len(dists) > 63 else np.nan,
            'd96': dists[95] if len(dists) > 95 else np.nan,
            'd128': dists[127] if len(dists) > 127 else np.nan,
        }

        # Margins (relative to d1)
        d1 = feat['d1']
        if not np.isnan(d1) and d1 > 0:
            feat['margin_2'] = (feat['d2'] - d1) / d1 if not np.isnan(feat['d2']) else np.nan
            feat['margin_4'] = (feat['d4'] - d1) / d1 if not np.isnan(feat['d4']) else np.nan
            feat['margin_8'] = (feat['d8'] - d1) / d1 if not np.isnan(feat['d8']) else np.nan
            feat['margin_16'] = (feat['d16'] - d1) / d1 if not np.isnan(feat['d16']) else np.nan
            feat['margin_32'] = (feat['d32'] - d1) / d1 if not np.isnan(feat['d32']) else np.nan
            feat['margin_64'] = (feat['d64'] - d1) / d1 if not np.isnan(feat['d64']) else np.nan

        # Ratios
        if not np.isnan(feat['d8']) and not np.isnan(d1) and d1 > 0:
            feat['ratio_8'] = feat['d8'] / d1
        if not np.isnan(feat['d16']) and not np.isnan(d1) and d1 > 0:
            feat['ratio_16'] = feat['d16'] / d1
        if not np.isnan(feat['d64']) and not np.isnan(d1) and d1 > 0:
            feat['ratio_64'] = feat['d64'] / d1

        # Distance variance in top 16/64
        if len(dists) >= 16:
            feat['dist_var_top16'] = np.var(dists[:16])
        if len(dists) >= 64:
            feat['dist_var_top64'] = np.var(dists[:64])

        # Slope
        if len(dists) >= 8:
            feat['slope_1_8'] = (dists[7] - dists[0]) / 7 if dists[0] != dists[7] else 0
        if len(dists) >= 64:
            feat['slope_8_64'] = (dists[63] - dists[7]) / 56 if dists[7] != dists[63] else 0

        features.append(feat)

    return pd.DataFrame(features)


def analyze_feature_correlation(features_df, min_b_dict):
    """Analyze correlation between features and min_B."""
    # Merge features with min_B
    features_df['min_B'] = features_df['query_id'].map(min_b_dict)

    # Filter valid queries
    valid_df = features_df[features_df['min_B'].notna() & (features_df['min_B'] <= 128)]

    print(f"\n{'='*60}")
    print("Feature Correlation Analysis")
    print(f"{'='*60}")
    print(f"Valid queries: {len(valid_df)}")

    # Compute correlations
    feature_cols = ['margin_2', 'margin_4', 'margin_8', 'margin_16', 'margin_32', 'margin_64',
                    'ratio_8', 'ratio_16', 'ratio_64', 'dist_var_top16', 'dist_var_top64',
                    'slope_1_8', 'slope_8_64']

    correlations = {}
    for col in feature_cols:
        if col in valid_df.columns:
            valid_mask = valid_df[col].notna()
            if valid_mask.sum() > 100:
                corr = valid_df.loc[valid_mask, col].corr(valid_df.loc[valid_mask, 'min_B'])
                correlations[col] = corr

    print("\nCorrelation with min_B:")
    for col, corr in sorted(correlations.items(), key=lambda x: abs(x[1]), reverse=True):
        print(f"  {col:20s}: {corr:+.4f}")

    # Bin analysis: min_B distribution by feature quartiles
    print("\n" + "="*60)
    print("min_B Distribution by margin_16 Quartile")
    print("="*60)

    if 'margin_16' in valid_df.columns:
        valid_margin = valid_df[valid_df['margin_16'].notna()].copy()
        valid_margin['margin_16_q'] = pd.qcut(valid_margin['margin_16'], 4, labels=['Q1', 'Q2', 'Q3', 'Q4'])

        for q in ['Q1', 'Q2', 'Q3', 'Q4']:
            q_df = valid_margin[valid_margin['margin_16_q'] == q]
            print(f"\n{q} (n={len(q_df)}):")
            print(f"  margin_16 range: [{q_df['margin_16'].min():.4f}, {q_df['margin_16'].max():.4f}]")
            print(f"  min_B <= 40: {100 * (q_df['min_B'] <= 40).sum() / len(q_df):.1f}%")
            print(f"  min_B <= 48: {100 * (q_df['min_B'] <= 48).sum() / len(q_df):.1f}%")
            print(f"  min_B <= 64: {100 * (q_df['min_B'] <= 64).sum() / len(q_df):.1f}%")
            print(f"  mean min_B: {q_df['min_B'].mean():.1f}")

    return valid_df


def propose_rule(valid_df):
    """Propose rule-based budget policy."""
    print("\n" + "="*60)
    print("Rule-based Policy Proposal")
    print("="*60)

    # Rule: margin_16 >= threshold -> easy query
    thresholds = [0.05, 0.10, 0.15, 0.20]

    for t in thresholds:
        easy_mask = valid_df['margin_16'] >= t
        easy_df = valid_df[easy_mask]
        hard_df = valid_df[~easy_mask]

        if len(easy_df) < 100:
            continue

        print(f"\nRule: margin_16 >= {t:.2f}")
        print(f"  Easy queries: {len(easy_df)} ({100*len(easy_df)/len(valid_df):.1f}%)")
        print(f"    min_B <= 40: {100 * (easy_df['min_B'] <= 40).sum() / len(easy_df):.1f}%")
        print(f"    min_B <= 48: {100 * (easy_df['min_B'] <= 48).sum() / len(easy_df):.1f}%")
        print(f"    mean min_B: {easy_df['min_B'].mean():.1f}")
        print(f"  Hard queries: {len(hard_df)} ({100*len(hard_df)/len(valid_df):.1f}%)")
        print(f"    min_B <= 64: {100 * (hard_df['min_B'] <= 64).sum() / len(hard_df):.1f}%")
        print(f"    mean min_B: {hard_df['min_B'].mean():.1f}")


def main():
    print("="*60)
    print("Phase 2: Head Distance Feature Extraction Analysis")
    print("="*60)

    # Read head distance trace
    trace_df = read_head_distance_trace('results/adaptive_budget/phase2_feature_extraction/head_distance_trace.csv')

    # Read query IO stats for InternalResultNum=128
    io_stats = read_query_io_stats('results/adaptive_budget/phase2_feature_extraction/query_io_stats.csv')
    baseline_recall = io_stats.set_index('query_id')['recall'].to_dict()

    # Extract features
    features_df = extract_head_distance_features(trace_df)
    print(f"\nExtracted features for {len(features_df)} queries")

    # Read budget sweep stats
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = read_budget_sweep_stats(budgets, 'results/adaptive_budget/budget_sweep')

    # Compute min_B
    min_b = compute_min_b(budgets, budget_stats, baseline_recall)
    print(f"\nComputed min_B for {len(min_b)} queries")

    # Distribution
    min_b_dist = defaultdict(int)
    for b in min_b.values():
        min_b_dist[b] += 1
    print("\nmin_B distribution:")
    for b in sorted(min_b_dist.keys()):
        pct = 100 * min_b_dist[b] / len(min_b)
        cum_pct = 100 * sum(min_b_dist[bb] for bb in sorted(min_b_dist.keys()) if bb <= b) / len(min_b)
        label = str(b) if b != 129 else ">128"
        print(f"  {label:>4s}: {min_b_dist[b]:5d} ({pct:5.1f}%) cum={cum_pct:5.1f}%")

    # Analyze correlation
    valid_df = analyze_feature_correlation(features_df, min_b)

    # Propose rule
    propose_rule(valid_df)

    # Save features
    output_path = 'results/adaptive_budget/phase2_feature_extraction/query_features.csv'
    features_df.to_csv(output_path, index=False)
    print(f"\nFeatures saved to {output_path}")


if __name__ == '__main__':
    main()
