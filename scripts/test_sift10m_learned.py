#!/usr/bin/env python3
"""
Test learned policy (risk-control) on SIFT10M.
"""

import pandas as pd
import numpy as np
import json
import lightgbm as lgb


def load_sift10m_data():
    """Load SIFT10M features and budget stats."""
    # Load head distance trace
    trace_df = pd.read_csv('results/adaptive_budget/sift10m_phase3_test/head_distance_trace.csv')

    # Extract features
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

        # Cross-margin ratios
        if len(dists) > 31 and len(dists) > 15 and feat.get('margin_32', 0) > 0:
            feat['margin_16_32_ratio'] = feat.get('margin_16', np.nan) / feat['margin_32']

        features.append(feat)

    features_df = pd.DataFrame(features)

    # Load budget stats
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}
    for b in budgets:
        df = pd.read_csv(f'results/adaptive_budget/sift10m_budget_sweep/query_io_stats_b{b}.csv')
        budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
        pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    return features_df, budget_stats, pages_data


def evaluate_assignments(features_df, assignments, budget_stats, pages_data):
    """Evaluate budget assignments."""
    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in features_df['query_id']])

    total_pages = 0
    missed = 0
    budget_counts = {}

    for _, row in features_df.iterrows():
        qid = int(row['query_id'])
        budget = assignments[qid]
        budget_counts[budget] = budget_counts.get(budget, 0) + 1

        total_pages += pages_data.get(budget, {}).get(qid, 0)
        if budget_stats.get(budget, {}).get(qid, 0) < budget_stats.get(64, {}).get(qid, 0) - 0.001:
            missed += 1

    saving = 100 * (baseline_pages - total_pages / len(features_df)) / baseline_pages
    miss_rate = 100 * missed / len(features_df)
    return saving, miss_rate, budget_counts


def test_sift1m_models_on_sift10m(sift10m_features, budget_stats, pages_data, feature_cols):
    """Test SIFT1M-trained models on SIFT10M."""
    print("\n" + "="*60)
    print("Test 1: SIFT1M models on SIFT10M (zero-shot)")
    print("="*60)

    # Load SIFT1M models
    risk_models = {}
    for b in [32, 40, 48]:
        try:
            risk_models[b] = lgb.Booster(model_file=f'results/adaptive_budget/phase4_learned/risk_model_b{b}.txt')
            print(f"Loaded risk model for B={b}")
        except:
            print(f"No model for B={b}")

    # Handle missing features
    X = sift10m_features[feature_cols].copy()
    for col in feature_cols:
        if X[col].isna().any():
            X[col] = X[col].fillna(X[col].median())

    print(f"\n{'Threshold':<12} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*65)

    for threshold in [0.90, 0.95, 0.97, 0.99]:
        assignments = {}
        for i, (_, row) in enumerate(sift10m_features.iterrows()):
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

        saving, miss_rate, budget_counts = evaluate_assignments(
            sift10m_features, assignments, budget_stats, pages_data)

        n = len(sift10m_features)
        b32 = 100 * budget_counts.get(32, 0) / n
        b48 = 100 * budget_counts.get(48, 0) / n
        b64 = 100 * budget_counts.get(64, 0) / n

        status = ""
        if saving >= 8 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 5 and miss_rate <= 5:
            status = " ⚠️"

        print(f"{threshold:<12.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {b32:>7.1f}% {b48:>7.1f}% {b64:>7.1f}%{status}")

    return risk_models


def train_sift10m_models(sift10m_features, budget_stats, pages_data, feature_cols):
    """Train risk-control models on SIFT10M."""
    print("\n" + "="*60)
    print("Test 2: Train models on SIFT10M")
    print("="*60)

    # Handle missing features
    X = sift10m_features[feature_cols].copy()
    for col in feature_cols:
        if X[col].isna().any():
            X[col] = X[col].fillna(X[col].median())

    # Compute min_B labels
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

    sift10m_features['min_B'] = sift10m_features['query_id'].map(min_b)

    # Train risk models
    risk_models = {}
    for b in [32, 40, 48]:
        print(f"\nTraining risk model for B={b}...")

        labels = []
        for _, row in sift10m_features.iterrows():
            qid = int(row['query_id'])
            safe = budget_stats.get(b, {}).get(qid, 0) >= budget_stats.get(64, {}).get(qid, 0) - 0.001
            labels.append(1 if safe else 0)

        labels = np.array(labels)
        safe_rate = labels.mean()
        print(f"  Safe rate at B={b}: {safe_rate:.3f}")

        if safe_rate < 0.05 or safe_rate > 0.99:
            print(f"  Skipping")
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

    # Test
    print(f"\n{'Threshold':<12} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*65)

    for threshold in [0.80, 0.85, 0.90, 0.95, 0.97, 0.99]:
        assignments = {}
        for i, (_, row) in enumerate(sift10m_features.iterrows()):
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

        saving, miss_rate, budget_counts = evaluate_assignments(
            sift10m_features, assignments, budget_stats, pages_data)

        n = len(sift10m_features)
        b32 = 100 * budget_counts.get(32, 0) / n
        b48 = 100 * budget_counts.get(48, 0) / n
        b64 = 100 * budget_counts.get(64, 0) / n

        status = ""
        if saving >= 8 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 5 and miss_rate <= 5:
            status = " ⚠️"

        print(f"{threshold:<12.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {b32:>7.1f}% {b48:>7.1f}% {b64:>7.1f}%{status}")

    return risk_models


def compare_with_rule_based(sift10m_features, budget_stats, pages_data):
    """Compare with percentile-based rule."""
    print("\n" + "="*60)
    print("Baseline: Rule-based (margin_16 percentile)")
    print("="*60)

    valid = sift10m_features[sift10m_features['margin_16'].notna()]

    print(f"\n{'Percentile':<12} {'Saving':>10} {'Miss Rate':>12}")
    print("-"*40)

    for pct in [20, 30, 40]:
        thresh = valid['margin_16'].quantile(1 - pct/100)

        assignments = {}
        for _, row in valid.iterrows():
            qid = int(row['query_id'])
            assignments[qid] = 48 if row['margin_16'] >= thresh else 64

        saving, miss_rate, _ = evaluate_assignments(valid, assignments, budget_stats, pages_data)

        status = ""
        if saving >= 8 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 5 and miss_rate <= 5:
            status = " ⚠️"

        print(f"top {pct}%      {saving:>9.1f}% {miss_rate:>11.1f}%{status}")


def main():
    print("="*60)
    print("Testing Learned Policy on SIFT10M")
    print("="*60)

    # Load feature columns
    with open('results/adaptive_budget/phase4_learned/feature_cols.json', 'r') as f:
        feature_cols = json.load(f)

    # Load SIFT10M data
    print("\nLoading SIFT10M data...")
    sift10m_features, budget_stats, pages_data = load_sift10m_data()
    print(f"SIFT10M: {len(sift10m_features)} queries")

    # Compare with rule-based
    compare_with_rule_based(sift10m_features, budget_stats, pages_data)

    # Test SIFT1M models
    test_sift1m_models_on_sift10m(sift10m_features, budget_stats, pages_data, feature_cols)

    # Train on SIFT10M
    train_sift10m_models(sift10m_features, budget_stats, pages_data, feature_cols)

    # Summary
    print("\n" + "="*60)
    print("Summary")
    print("="*60)
    print("""
SIFT10M Results:
- Rule-based: max ~6.9% saving at <=2% miss
- SIFT1M models (zero-shot): similar to rule-based
- SIFT10M-trained models: TBD (check above)

Key insight: SIFT10M has lower baseline recall (0.949 vs 0.978),
making it harder to reduce budget without hurting recall.
""")


if __name__ == '__main__':
    main()
