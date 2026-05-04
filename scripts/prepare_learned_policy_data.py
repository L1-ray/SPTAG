#!/usr/bin/env python3
"""
Phase 4: Prepare training data for learned policy.
Features: margin, ratio, slope, variance from head distance trace
Label: min_B_for_baseline_query_recall
"""

import pandas as pd
import numpy as np
from sklearn.model_selection import train_test_split
import json


def extract_features(trace_df):
    """Extract comprehensive features from head distance trace."""
    features = []

    for qid in trace_df['query_id'].unique():
        query_trace = trace_df[trace_df['query_id'] == qid].sort_values('posting_index')
        dists = query_trace['head_dist'].values

        if len(dists) == 0:
            continue

        feat = {'query_id': qid}

        # Raw distances
        for i, idx in enumerate([0, 1, 3, 7, 15, 31, 63, 95, 127]):
            key = f'd{idx+1}'
            feat[key] = dists[idx] if len(dists) > idx else np.nan

        d1 = dists[0]
        d1_safe = d1 if d1 > 0.001 else 0.001

        # Margins (normalized by d1)
        for i, idx in enumerate([1, 3, 7, 15, 31, 63]):
            if len(dists) > idx and d1 > 0:
                feat[f'margin_{idx+1}'] = (dists[idx] - d1) / d1
            else:
                feat[f'margin_{idx+1}'] = np.nan

        # Ratios
        for i, idx in enumerate([7, 15, 63]):
            if len(dists) > idx and d1 > 0:
                feat[f'ratio_{idx+1}'] = dists[idx] / d1
            else:
                feat[f'ratio_{idx+1}'] = np.nan

        # Slopes
        if len(dists) >= 8:
            feat['slope_1_8'] = (dists[7] - dists[0]) / 7
        if len(dists) >= 16:
            feat['slope_8_16'] = (dists[15] - dists[7]) / 8
        if len(dists) >= 64:
            feat['slope_16_64'] = (dists[63] - dists[15]) / 48
        if len(dists) >= 96:
            feat['slope_64_96'] = (dists[95] - dists[63]) / 32

        # Variance
        if len(dists) >= 16:
            feat['var_16'] = np.var(dists[:16])
        if len(dists) >= 64:
            feat['var_64'] = np.var(dists[:64])

        # Entropy (using softmax-like normalization)
        if len(dists) >= 16:
            neg_dists = -dists[:16]
            exp_dists = np.exp(neg_dists - np.max(neg_dists))  # stability
            softmax = exp_dists / exp_dists.sum()
            feat['entropy_16'] = -np.sum(softmax * np.log(softmax + 1e-10))

        if len(dists) >= 64:
            neg_dists = -dists[:64]
            exp_dists = np.exp(neg_dists - np.max(neg_dists))
            softmax = exp_dists / exp_dists.sum()
            feat['entropy_64'] = -np.sum(softmax * np.log(softmax + 1e-10))

        # Cross-margin ratios
        if len(dists) > 31 and len(dists) > 15 and feat.get('margin_32', 0) > 0:
            feat['margin_16_32_ratio'] = feat.get('margin_16', np.nan) / feat['margin_32']

        features.append(feat)

    return pd.DataFrame(features)


def compute_labels(features_df, budget_stats):
    """Compute min_B labels for each query."""
    baseline_recall = budget_stats[64]
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]

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

    features_df['min_B'] = features_df['query_id'].map(min_b)
    return features_df


def main():
    print("="*60)
    print("Phase 4: Preparing Training Data for Learned Policy")
    print("="*60)

    # Load head distance trace
    print("\nLoading head distance trace...")
    trace_df = pd.read_csv('results/adaptive_budget/phase2_feature_extraction/head_distance_trace.csv')
    print(f"  {len(trace_df)} trace records, {trace_df['query_id'].nunique()} queries")

    # Extract features
    print("\nExtracting features...")
    features_df = extract_features(trace_df)
    print(f"  {len(features_df)} queries with {len(features_df.columns)-1} features")

    # Load budget stats
    print("\nLoading budget stats...")
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    for b in budgets:
        df = pd.read_csv(f'results/adaptive_budget/budget_sweep/query_io_stats_b{b}.csv')
        budget_stats[b] = df.set_index('query_id')['recall'].to_dict()

    # Compute labels
    features_df = compute_labels(features_df, budget_stats)

    # Filter valid queries
    valid_df = features_df[features_df['min_B'].notna() & (features_df['min_B'] <= 128)].copy()
    print(f"  {len(valid_df)} valid queries with min_B label")

    # Label distribution
    print("\nLabel distribution:")
    for b in budgets:
        count = (valid_df['min_B'] == b).sum()
        pct = 100 * count / len(valid_df)
        print(f"  B={b}: {count} ({pct:.1f}%)")

    # Create classification labels (bins)
    # Class 0: B <= 32, Class 1: B <= 48, Class 2: B <= 64, Class 3: B > 64
    def get_class(min_b):
        if min_b <= 32:
            return 0
        elif min_b <= 48:
            return 1
        elif min_b <= 64:
            return 2
        else:
            return 3

    valid_df['label_class'] = valid_df['min_B'].apply(get_class)

    print("\nClass distribution:")
    for c in range(4):
        count = (valid_df['label_class'] == c).sum()
        pct = 100 * count / len(valid_df)
        print(f"  Class {c}: {count} ({pct:.1f}%)")

    # Feature columns
    feature_cols = [c for c in valid_df.columns if c not in ['query_id', 'min_B', 'label_class']]
    print(f"\nFeature columns ({len(feature_cols)}): {feature_cols}")

    # Handle missing values
    print("\nHandling missing values...")
    for col in feature_cols:
        if valid_df[col].isna().any():
            median_val = valid_df[col].median()
            valid_df[col] = valid_df[col].fillna(median_val)
            print(f"  {col}: filled with median {median_val:.4f}")

    # Split train/test
    train_df, test_df = train_test_split(valid_df, test_size=0.2, random_state=42,
                                          stratify=valid_df['label_class'])

    print(f"\nTrain set: {len(train_df)} queries")
    print(f"Test set: {len(test_df)} queries")

    # Save to files
    train_df.to_csv('results/adaptive_budget/phase4_learned/train_data.csv', index=False)
    test_df.to_csv('results/adaptive_budget/phase4_learned/test_data.csv', index=False)

    # Save feature columns
    with open('results/adaptive_budget/phase4_learned/feature_cols.json', 'w') as f:
        json.dump(feature_cols, f)

    print("\nFiles saved:")
    print("  results/adaptive_budget/phase4_learned/train_data.csv")
    print("  results/adaptive_budget/phase4_learned/test_data.csv")
    print("  results/adaptive_budget/phase4_learned/feature_cols.json")


if __name__ == '__main__':
    import os
    os.makedirs('results/adaptive_budget/phase4_learned', exist_ok=True)
    main()
