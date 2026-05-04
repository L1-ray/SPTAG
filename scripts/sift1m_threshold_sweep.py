#!/usr/bin/env python3
"""
SIFT1M Threshold Sweep - Find optimal threshold for SIFT1M dataset.
"""

import pandas as pd
import numpy as np
import lightgbm as lgb
import json
import os

def load_sift1m_data():
    """Load SIFT1M features and budget stats."""
    print("Loading SIFT1M data...")

    # Load head distance trace
    trace_df = pd.read_csv('results/adaptive_budget/phase2_feature_extraction/head_distance_trace.csv')
    print(f"  Head distance trace: {len(trace_df)} records")

    # Extract features
    features = []
    for qid in trace_df['query_id'].unique():
        query_trace = trace_df[trace_df['query_id'] == qid].sort_values('posting_index')
        dists = query_trace['head_dist'].values

        if len(dists) == 0:
            continue

        feat = {'query_id': qid}

        # Raw distances
        d1 = dists[0]
        for i, idx in enumerate([0, 1, 3, 7, 15, 31, 63, 95, 127]):
            key = f'd{idx+1}'
            feat[key] = dists[idx] if len(dists) > idx else np.nan

        # Margins (normalized)
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

        # Entropy
        if len(dists) >= 16:
            neg_dists = -dists[:16]
            exp_dists = np.exp(neg_dists - np.max(neg_dists))
            softmax = exp_dists / exp_dists.sum()
            feat['entropy_16'] = -np.sum(softmax * np.log(softmax + 1e-10))

        if len(dists) >= 64:
            neg_dists = -dists[:64]
            exp_dists = np.exp(neg_dists - np.max(neg_dists))
            softmax = exp_dists / exp_dists.sum()
            feat['entropy_64'] = -np.sum(softmax * np.log(softmax + 1e-10))

        # margin_16_32_ratio
        margin_16 = feat.get('margin_16', 0)
        margin_32 = feat.get('margin_32', 0)
        if margin_32 > 0:
            feat['margin_16_32_ratio'] = margin_16 / margin_32
        else:
            feat['margin_16_32_ratio'] = 0.0

        features.append(feat)

    features_df = pd.DataFrame(features)
    print(f"  Extracted features for {len(features_df)} queries")

    # Load budget stats
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}
    for b in budgets:
        df = pd.read_csv(f'results/adaptive_budget/budget_sweep/query_io_stats_b{b}.csv')
        budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
        pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    return features_df, budget_stats, pages_data

def train_risk_models(features_df, budget_stats, feature_cols):
    """Train risk-control models."""
    print("\nTraining risk-control models...")

    X = features_df[feature_cols].copy()
    for col in feature_cols:
        if X[col].isna().any():
            X[col] = X[col].fillna(X[col].median())

    risk_models = {}
    for b in [32, 40, 48]:
        print(f"\n  Training risk model for B={b}...")

        labels = []
        for _, row in features_df.iterrows():
            qid = int(row['query_id'])
            safe = budget_stats.get(b, {}).get(qid, 0) >= budget_stats.get(64, {}).get(qid, 0) - 0.001
            labels.append(1 if safe else 0)

        labels = np.array(labels)
        safe_rate = labels.mean()
        print(f"    Safe rate at B={b}: {safe_rate:.3f}")

        if safe_rate < 0.05 or safe_rate > 0.99:
            print(f"    Skipping")
            continue

        train_data = lgb.Dataset(X, label=labels)
        params = {
            'objective': 'binary',
            'metric': 'binary_logloss',
            'boosting_type': 'gbdt',
            'num_leaves': 31,
            'learning_rate': 0.05,
            'feature_fraction': 0.8,
            'verbose': -1,
            'seed': 42,
        }

        model = lgb.train(params, train_data, num_boost_round=200)
        risk_models[b] = model

    return risk_models

def evaluate_threshold(features_df, budget_stats, pages_data, risk_models, threshold, feature_cols):
    """Evaluate learned policy with given threshold."""
    X = features_df[feature_cols].copy()
    for col in feature_cols:
        if X[col].isna().any():
            X[col] = X[col].fillna(X[col].median())

    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in features_df['query_id']])

    total_pages = 0
    missed = 0
    budget_counts = {}

    for i, (_, row) in enumerate(features_df.iterrows()):
        qid = int(row['query_id'])
        x = X.iloc[i:i+1]

        budget = 64
        for b in [32, 40, 48]:
            if b in risk_models:
                prob_safe = risk_models[b].predict(x)[0]
                if prob_safe >= threshold:
                    budget = b
                    break

        budget_counts[budget] = budget_counts.get(budget, 0) + 1

        total_pages += pages_data.get(budget, {}).get(qid, 0)
        if budget_stats.get(budget, {}).get(qid, 0) < budget_stats.get(64, {}).get(qid, 0) - 0.001:
            missed += 1

    saving = 100 * (baseline_pages - total_pages / len(features_df)) / baseline_pages
    miss_rate = 100 * missed / len(features_df)

    return saving, miss_rate, budget_counts

def main():
    print("="*70)
    print("SIFT1M Threshold Sweep")
    print("="*70)

    feature_cols = [
        'd1', 'd2', 'd4', 'd8', 'd16', 'd32', 'd64', 'd96', 'd128',
        'margin_2', 'margin_4', 'margin_8', 'margin_16', 'margin_32', 'margin_64',
        'ratio_8', 'ratio_16', 'ratio_64',
        'slope_1_8', 'slope_8_16', 'slope_16_64', 'slope_64_96',
        'var_16', 'var_64', 'entropy_16', 'entropy_64',
        'margin_16_32_ratio'
    ]

    # Load data
    features_df, budget_stats, pages_data = load_sift1m_data()

    # Train models
    risk_models = train_risk_models(features_df, budget_stats, feature_cols)

    # Threshold sweep
    print("\n" + "="*70)
    print("Threshold Sweep Results")
    print("="*70)
    print(f"{'Threshold':<12} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B40%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*75)

    results = []
    for threshold in [0.70, 0.75, 0.80, 0.85, 0.90, 0.95, 0.97, 0.99]:
        saving, miss_rate, budget_counts = evaluate_threshold(
            features_df, budget_stats, pages_data, risk_models, threshold, feature_cols)

        n = len(features_df)
        b32 = 100 * budget_counts.get(32, 0) / n
        b40 = 100 * budget_counts.get(40, 0) / n
        b48 = 100 * budget_counts.get(48, 0) / n
        b64 = 100 * budget_counts.get(64, 0) / n

        status = ""
        if saving >= 8 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 5 and miss_rate <= 5:
            status = " ⚠️"

        print(f"{threshold:<12.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {b32:>7.1f}% {b40:>7.1f}% {b48:>7.1f}% {b64:>7.1f}%{status}")

        results.append({
            'threshold': threshold,
            'saving': saving,
            'miss_rate': miss_rate,
            'b32': b32,
            'b40': b40,
            'b48': b48,
            'b64': b64
        })

    # Find optimal threshold
    print("\n" + "="*70)
    print("Recommendation")
    print("="*70)

    # Filter thresholds that meet criteria
    valid = [r for r in results if r['miss_rate'] <= 2.0]
    if valid:
        # Find the one with max saving
        best = max(valid, key=lambda x: x['saving'])
        print(f"\nRecommended threshold: {best['threshold']:.2f}")
        print(f"  Expected pages saving: {best['saving']:.1f}%")
        print(f"  Expected miss rate: {best['miss_rate']:.1f}%")
        print(f"  Budget distribution: B32={best['b32']:.1f}%, B40={best['b40']:.1f}%, B48={best['b48']:.1f}%, B64={best['b64']:.1f}%")
    else:
        print("\nNo threshold meets miss_rate <= 2% criteria")

if __name__ == '__main__':
    main()
