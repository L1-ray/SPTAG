#!/usr/bin/env python3
"""
Train ir=128 learned budget model for SIFT1M.
Features: 27 dimensions (including d96, d128, slope_64_96)
"""

import pandas as pd
import numpy as np
import json
import lightgbm as lgb
import os
from pathlib import Path

# Feature columns for ir=128 model (27 features)
FEATURE_COLS_IR128 = [
    "d1", "d2", "d4", "d8", "d16", "d32", "d64", "d96", "d128",
    "margin_2", "margin_4", "margin_8", "margin_16", "margin_32", "margin_64",
    "ratio_8", "ratio_16", "ratio_64",
    "slope_1_8", "slope_8_16", "slope_16_64", "slope_64_96",
    "var_16", "var_64",
    "entropy_16", "entropy_64",
    "margin_16_32_ratio"
]

def extract_features_from_trace(trace_file, max_postings=128):
    """Extract features from head distance trace."""
    df = pd.read_csv(trace_file)

    features_list = []
    for query_id in df['query_id'].unique():
        qdf = df[df['query_id'] == query_id].sort_values('posting_index')
        distances = qdf['head_dist'].values[:max_postings]

        features = extract_features_from_distances(distances)
        features['query_id'] = query_id
        features_list.append(features)

    return pd.DataFrame(features_list)

def extract_features_from_distances(distances):
    """Extract 27 features from distance array."""
    features = {}

    # Pad if needed
    d = np.zeros(128)
    n = min(len(distances), 128)
    d[:n] = distances[:n]

    # Raw distances (9 features)
    indices = [0, 1, 3, 7, 15, 31, 63, 95, 127]
    for i, idx in enumerate(indices):
        features[f'd{idx+1}'] = d[idx] if idx < n else 0.0

    d1 = d[0] if d[0] > 0 else 1e-10

    # Margins (6 features)
    margin_indices = [1, 3, 7, 15, 31, 63]
    for i, idx in enumerate(margin_indices):
        features[f'margin_{idx+1}'] = (d[idx] - d[0]) / d1 if idx < n else 0.0

    # Ratios (3 features)
    ratio_indices = [7, 15, 63]
    for i, idx in enumerate(ratio_indices):
        features[f'ratio_{idx+1}'] = d[idx] / d1 if idx < n else 0.0

    # Slopes (4 features)
    features['slope_1_8'] = (d[7] - d[0]) / 7 if 7 < n else 0.0
    features['slope_8_16'] = (d[15] - d[7]) / 8 if 15 < n else 0.0
    features['slope_16_64'] = (d[63] - d[15]) / 48 if 63 < n else 0.0
    features['slope_64_96'] = (d[95] - d[63]) / 32 if 95 < n else 0.0

    # Variance (2 features)
    features['var_16'] = np.var(d[:16]) if n >= 16 else 0.0
    features['var_64'] = np.var(d[:64]) if n >= 64 else 0.0

    # Entropy (2 features)
    def calc_entropy(arr):
        if len(arr) < 2:
            return 0.0
        max_val = np.max(arr)
        exp_vals = np.exp(-(arr - max_val))
        probs = exp_vals / np.sum(exp_vals)
        return -np.sum(probs * np.log(probs + 1e-10))

    features['entropy_16'] = calc_entropy(d[:16]) if n >= 16 else 0.0
    features['entropy_64'] = calc_entropy(d[:64]) if n >= 64 else 0.0

    # Cross-margin ratio (1 feature)
    margin_16 = features.get('margin_16', 0)
    margin_32 = features.get('margin_32', 0)
    features['margin_16_32_ratio'] = margin_16 / margin_32 if margin_32 > 0 else 0.0

    return features

def load_budget_stats():
    """Load recall stats for each budget."""
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}

    for b in budgets:
        csv_path = f'results/adaptive_budget/budget_sweep/query_io_stats_b{b}.csv'
        if os.path.exists(csv_path):
            df = pd.read_csv(csv_path)
            budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
            pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    return budget_stats, pages_data

def compute_min_b(features_df, budget_stats):
    """Compute minimum budget for each query."""
    min_b_list = []

    for _, row in features_df.iterrows():
        qid = int(row['query_id'])
        target_recall = budget_stats.get(64, {}).get(qid, 1.0) - 0.001  # Allow small drop

        min_b = 64
        for b in [16, 32, 40, 48]:
            if b in budget_stats:
                if budget_stats[b].get(qid, 0) >= target_recall:
                    min_b = b
                    break

        min_b_list.append(min_b)

    features_df['min_B'] = min_b_list
    return features_df

def train_risk_models(train_df, test_df, budget_stats, output_dir):
    """Train risk-control models for budgets 32, 40, 48."""
    os.makedirs(output_dir, exist_ok=True)

    X_train = train_df[FEATURE_COLS_IR128]
    X_test = test_df[FEATURE_COLS_IR128]

    risk_models = {}

    for b in [32, 40, 48]:
        print(f"\nTraining risk model for B={b}...")

        train_labels = []
        for _, row in train_df.iterrows():
            qid = int(row['query_id'])
            safe = budget_stats.get(b, {}).get(qid, 0) >= budget_stats.get(64, {}).get(qid, 0) - 0.001
            train_labels.append(1 if safe else 0)

        train_labels = np.array(train_labels)
        safe_rate = train_labels.mean()
        print(f"  Safe rate at B={b}: {safe_rate:.3f}")

        if safe_rate < 0.05 or safe_rate > 0.99:
            print(f"  Skipping (too few samples)")
            continue

        train_data = lgb.Dataset(X_train, label=train_labels)
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

        # Save as text format
        model.save_model(f'{output_dir}/risk_model_b{b}.txt')

    # Export to JSON
    export_models_to_json(risk_models, output_dir)

    return risk_models

def export_models_to_json(models, output_dir):
    """Export LightGBM models to JSON format for C++ inference."""
    for b, model in models.items():
        model_text = model.model_to_string()

        # Parse the text format to extract tree structure
        # This is a simplified export - for production use lightgbm's native JSON export
        import re

        trees = []
        lines = model_text.split('\n')
        current_tree = None
        tree_data = None

        for line in lines:
            if line.startswith('Tree='):
                if tree_data:
                    trees.append(tree_data)
                tree_data = {'num_leaves': 0, 'num_cat': 0, 'split_feature': [],
                            'threshold': [], 'left_child': [], 'right_child': [],
                            'leaf_value': [], 'decision_type': []}
            elif line.startswith('num_leaves='):
                if tree_data:
                    tree_data['num_leaves'] = int(line.split('=')[1])
            elif line.startswith('num_cat='):
                if tree_data:
                    tree_data['num_cat'] = int(line.split('=')[1])
            elif line.startswith('split_feature='):
                if tree_data:
                    vals = line.split('=')[1].strip()
                    if vals:
                        tree_data['split_feature'] = [int(x) for x in vals.split()]
            elif line.startswith('threshold='):
                if tree_data:
                    vals = line.split('=')[1].strip()
                    if vals:
                        tree_data['threshold'] = [float(x) for x in vals.split()]
            elif line.startswith('left_child='):
                if tree_data:
                    vals = line.split('=')[1].strip()
                    if vals:
                        tree_data['left_child'] = [int(x) for x in vals.split()]
            elif line.startswith('right_child='):
                if tree_data:
                    vals = line.split('=')[1].strip()
                    if vals:
                        tree_data['right_child'] = [int(x) for x in vals.split()]
            elif line.startswith('leaf_value='):
                if tree_data:
                    vals = line.split('=')[1].strip()
                    if vals:
                        tree_data['leaf_value'] = [float(x) for x in vals.split()]
            elif line.startswith('decision_type='):
                if tree_data:
                    vals = line.split('=')[1].strip()
                    if vals:
                        tree_data['decision_type'] = [int(x) for x in vals.split()]

        if tree_data:
            trees.append(tree_data)

        # Create JSON structure
        json_model = {
            'feature_names': FEATURE_COLS_IR128,
            'num_features': len(FEATURE_COLS_IR128),
            'num_trees': len(trees),
            'trees': trees
        }

        json_path = f'{output_dir}/risk_model_b{b}.json'
        with open(json_path, 'w') as f:
            json.dump(json_model, f, indent=2)
        print(f"  Saved {json_path}")

def evaluate_risk_models(models, test_df, budget_stats, pages_data, threshold):
    """Evaluate risk models with given threshold."""
    X_test = test_df[FEATURE_COLS_IR128]

    assignments = {}
    for i, (_, row) in enumerate(test_df.iterrows()):
        qid = int(row['query_id'])
        x = X_test.iloc[i:i+1]

        budget = 64
        for b in [32, 40, 48]:
            if b in models:
                prob = models[b].predict(x)[0]
                if prob >= threshold:
                    budget = b
                    break

        assignments[qid] = budget

    # Calculate metrics
    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in test_df['query_id']])

    total_pages = 0
    missed = 0
    budget_counts = {}

    for _, row in test_df.iterrows():
        qid = int(row['query_id'])
        budget = assignments[qid]
        budget_counts[budget] = budget_counts.get(budget, 0) + 1

        total_pages += pages_data.get(budget, {}).get(qid, 0)
        if budget_stats.get(budget, {}).get(qid, 0) < budget_stats.get(64, {}).get(qid, 0) - 0.001:
            missed += 1

    saving = 100 * (baseline_pages - total_pages / len(test_df)) / baseline_pages
    miss_rate = 100 * missed / len(test_df)

    n = len(test_df)
    budget_dist = {b: 100 * budget_counts.get(b, 0) / n for b in [32, 40, 48, 64]}

    return saving, miss_rate, budget_dist

def main():
    print("="*60)
    print("Training ir=128 Learned Budget Model for SIFT1M")
    print("="*60)

    output_dir = 'results/adaptive_budget/sift1m_ir128_retrain'
    os.makedirs(output_dir, exist_ok=True)

    # Load budget stats
    budget_stats, pages_data = load_budget_stats()
    print(f"Loaded budget stats for: {list(budget_stats.keys())}")

    # Check for head distance trace with ir=128 data
    trace_file = 'results/adaptive_budget/phase2_feature_extraction/head_distance_trace.csv'

    print(f"\nExtracting features from {trace_file}...")
    features_df = extract_features_from_trace(trace_file, max_postings=128)
    print(f"Extracted features for {len(features_df)} queries")

    # Compute min_B
    features_df = compute_min_b(features_df, budget_stats)

    # Split train/test
    np.random.seed(42)
    query_ids = features_df['query_id'].values
    np.random.shuffle(query_ids)
    split = int(0.8 * len(query_ids))

    train_df = features_df[features_df['query_id'].isin(query_ids[:split])]
    test_df = features_df[features_df['query_id'].isin(query_ids[split:])]

    print(f"Train: {len(train_df)}, Test: {len(test_df)}")

    # Save feature columns
    with open(f'{output_dir}/feature_cols.json', 'w') as f:
        json.dump(FEATURE_COLS_IR128, f, indent=2)

    # Save train/test data
    train_df.to_csv(f'{output_dir}/train_data.csv', index=False)
    test_df.to_csv(f'{output_dir}/test_data.csv', index=False)

    # Train risk models
    models = train_risk_models(train_df, test_df, budget_stats, output_dir)

    # Evaluate with different thresholds
    print(f"\n{'Threshold':<12} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B40%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*75)

    for threshold in [0.80, 0.85, 0.90, 0.95, 0.97, 0.99]:
        saving, miss_rate, budget_dist = evaluate_risk_models(
            models, test_df, budget_stats, pages_data, threshold)

        status = ""
        if saving >= 12 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 8 and miss_rate <= 2:
            status = " ⚠️"

        print(f"{threshold:<12.2f} {saving:>9.1f}% {miss_rate:>11.1f}% "
              f"{budget_dist.get(32, 0):>7.1f}% {budget_dist.get(40, 0):>7.1f}% "
              f"{budget_dist.get(48, 0):>7.1f}% {budget_dist.get(64, 0):>7.1f}%{status}")

    print(f"\nModels saved to {output_dir}/")
    print(f"Feature columns: {len(FEATURE_COLS_IR128)} (ir=128)")

if __name__ == '__main__':
    main()
