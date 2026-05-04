#!/usr/bin/env python3
"""
Train and test learned policy on SIFT10M.
"""

import pandas as pd
import numpy as np
import json
import os

# Check if we can use existing data
def check_data():
    """Check if SIFT10M training data exists."""
    trace_path = 'results/adaptive_budget/sift10m_phase3_test/head_distance_trace.csv'
    budget_dir = 'results/adaptive_budget/sift10m_budget_sweep'

    if os.path.exists(trace_path):
        print(f"Found head distance trace: {trace_path}")
    else:
        print(f"Missing: {trace_path}")
        return False

    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    for b in budgets:
        path = f'{budget_dir}/query_io_stats_b{b}.csv'
        if os.path.exists(path):
            print(f"Found budget stats: {path}")
        else:
            print(f"Missing: {path}")
            return False

    return True

def load_sift10m_data():
    """Load SIFT10M features and budget stats."""
    print("\nLoading SIFT10M data...")

    # Load head distance trace
    trace_df = pd.read_csv('results/adaptive_budget/sift10m_phase3_test/head_distance_trace.csv')
    print(f"  Head distance trace: {len(trace_df)} records")

    # Extract features
    features = []
    for qid in trace_df['query_id'].unique():
        query_trace = trace_df[trace_df['query_id'] == qid].sort_values('posting_index')
        dists = query_trace['head_dist'].values

        if len(dists) == 0:
            continue

        feat = {'query_id': qid}

        # Raw distances (normalized to d1 ratio)
        d1 = dists[0]
        for i, idx in enumerate([0, 1, 3, 7, 15, 31, 63, 95, 127]):
            key = f'd{idx+1}'
            feat[key] = dists[idx] if len(dists) > idx else np.nan

        # Margins
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
        df = pd.read_csv(f'results/adaptive_budget/sift10m_budget_sweep/query_io_stats_b{b}.csv')
        budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
        pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    return features_df, budget_stats, pages_data

def train_risk_models(features_df, budget_stats, feature_cols, save_path='results/adaptive_budget/sift10m_learned'):
    """Train risk-control models for SIFT10M."""
    import lightgbm as lgb
    import os

    print("\nTraining risk-control models...")

    os.makedirs(save_path, exist_ok=True)

    # Handle missing features
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

        # Save model
        model_path = f'{save_path}/risk_model_b{b}.txt'
        model.save_model(model_path)
        print(f"    Saved to {model_path}")

    # Save feature columns
    with open(f'{save_path}/feature_cols.json', 'w') as f:
        json.dump(feature_cols, f)
    print(f"  Saved feature columns to {save_path}/feature_cols.json")

    return risk_models

def evaluate_learned_policy(features_df, budget_stats, pages_data, risk_models, threshold=0.80):
    """Evaluate learned policy on SIFT10M."""
    print(f"\nEvaluating learned policy (threshold={threshold})...")

    feature_cols = [
        'd1', 'd2', 'd4', 'd8', 'd16', 'd32', 'd64', 'd96', 'd128',
        'margin_2', 'margin_4', 'margin_8', 'margin_16', 'margin_32', 'margin_64',
        'ratio_8', 'ratio_16', 'ratio_64',
        'slope_1_8', 'slope_8_16', 'slope_16_64', 'slope_64_96',
        'var_16', 'var_64', 'entropy_16', 'entropy_64',
        'margin_16_32_ratio'
    ]

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

        assignments[qid] = budget
        budget_counts[budget] = budget_counts.get(budget, 0) + 1

        total_pages += pages_data.get(budget, {}).get(qid, 0)
        if budget_stats.get(budget, {}).get(qid, 0) < budget_stats.get(64, {}).get(qid, 0) - 0.001:
            missed += 1

    saving = 100 * (baseline_pages - total_pages / len(features_df)) / baseline_pages
    miss_rate = 100 * missed / len(features_df)

    print(f"  Pages saving: {saving:.1f}%")
    print(f"  Miss rate: {miss_rate:.1f}%")
    print(f"  Budget distribution: {budget_counts}")

    return saving, miss_rate, budget_counts

def get_feature_cols():
    """Get feature columns for SIFT10M."""
    return [
        'd1', 'd2', 'd4', 'd8', 'd16', 'd32', 'd64', 'd96', 'd128',
        'margin_2', 'margin_4', 'margin_8', 'margin_16', 'margin_32', 'margin_64',
        'ratio_8', 'ratio_16', 'ratio_64',
        'slope_1_8', 'slope_8_16', 'slope_16_64', 'slope_64_96',
        'var_16', 'var_64', 'entropy_16', 'entropy_64',
        'margin_16_32_ratio'
    ]

def main():
    print("="*60)
    print("SIFT10M Learned Policy Training and Evaluation")
    print("="*60)

    # Check data
    if not check_data():
        print("\nMissing data, cannot proceed")
        return

    # Use local feature columns (not from SIFT1M model)
    feature_cols = get_feature_cols()
    print(f"\nFeature columns: {len(feature_cols)}")

    # Load SIFT10M data
    features_df, budget_stats, pages_data = load_sift10m_data()

    # Train models
    risk_models = train_risk_models(features_df, budget_stats, feature_cols)

    # Evaluate with different thresholds
    print("\n" + "="*60)
    print("Threshold Sweep")
    print("="*60)
    print(f"{'Threshold':<12} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*65)

    for threshold in [0.70, 0.75, 0.80, 0.85, 0.90, 0.95]:
        saving, miss_rate, budget_counts = evaluate_learned_policy(
            features_df, budget_stats, pages_data, risk_models, threshold)

        n = len(features_df)
        b32 = 100 * budget_counts.get(32, 0) / n
        b48 = 100 * budget_counts.get(48, 0) / n
        b64 = 100 * budget_counts.get(64, 0) / n

        status = ""
        if saving >= 8 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 5 and miss_rate <= 5:
            status = " ⚠️"

        print(f"{threshold:<12.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {b32:>7.1f}% {b48:>7.1f}% {b64:>7.1f}%{status}")

if __name__ == '__main__':
    # Need to define assignments globally
    assignments = {}
    main()
