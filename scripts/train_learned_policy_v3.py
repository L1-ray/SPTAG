#!/usr/bin/env python3
"""
Phase 4: Train GBDT for adaptive budget.
Approach 1: Regression + conservative rounding
Approach 2: Risk-control (per-budget safety prediction)
"""

import pandas as pd
import numpy as np
import json
import lightgbm as lgb


def load_data():
    train_df = pd.read_csv('results/adaptive_budget/phase4_learned/train_data.csv')
    test_df = pd.read_csv('results/adaptive_budget/phase4_learned/test_data.csv')
    with open('results/adaptive_budget/phase4_learned/feature_cols.json', 'r') as f:
        feature_cols = json.load(f)

    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}
    for b in budgets:
        df = pd.read_csv(f'results/adaptive_budget/budget_sweep/query_io_stats_b{b}.csv')
        budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
        pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    return train_df, test_df, feature_cols, budget_stats, pages_data


def evaluate_budget_assignment(test_df, assignments, budget_stats, pages_data):
    """Evaluate a budget assignment (dict: query_id -> budget)."""
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
    return saving, miss_rate, budget_counts


def approach1_regression(train_df, test_df, feature_cols, budget_stats, pages_data):
    """Approach 1: Regression model + conservative rounding."""
    print("\n" + "="*60)
    print("Approach 1: Regression + Conservative Rounding")
    print("="*60)

    X_train = train_df[feature_cols]
    y_train = train_df['min_B'].values.astype(float)
    X_test = test_df[feature_cols]

    train_data = lgb.Dataset(X_train, label=y_train)
    valid_data = lgb.Dataset(X_test, label=test_df['min_B'].values.astype(float), reference=train_data)

    params = {
        'objective': 'quantile',
        'alpha': 0.7,  # Predict 70th percentile (conservative, biased toward higher budget)
        'metric': 'mae',
        'boosting_type': 'gbdt',
        'num_leaves': 31,
        'learning_rate': 0.05,
        'feature_fraction': 0.8,
        'verbose': -1,
        'seed': 42,
    }

    model = lgb.train(params, train_data, num_boost_round=300, valid_sets=[valid_data],
                       callbacks=[lgb.log_evaluation(100)])

    y_pred = model.predict(X_test)

    valid_budgets = [16, 32, 40, 48, 64, 80, 96, 128]

    def round_budget(pred, bias=0.0):
        adjusted = pred * (1 + bias)
        return min(valid_budgets, key=lambda b: abs(b - adjusted))

    print(f"\n{'Bias':<8} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*60)

    for bias in [0.0, 0.05, 0.10, 0.15, 0.20, 0.30]:
        assignments = {}
        for i, (_, row) in enumerate(test_df.iterrows()):
            qid = int(row['query_id'])
            assignments[qid] = round_budget(y_pred[i], bias)

        saving, miss_rate, budget_counts = evaluate_budget_assignment(
            test_df, assignments, budget_stats, pages_data)

        n = len(test_df)
        b32 = 100 * budget_counts.get(32, 0) / n
        b48 = 100 * budget_counts.get(48, 0) / n
        b64 = 100 * budget_counts.get(64, 0) / n

        status = ""
        if saving >= 12 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 8 and miss_rate <= 2:
            status = " ⚠️"

        print(f"{bias:<8.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {b32:>7.1f}% {b48:>7.1f}% {b64:>7.1f}%{status}")

    return model


def approach2_risk_control(train_df, test_df, feature_cols, budget_stats, pages_data):
    """Approach 2: Risk-control - per-budget safety prediction."""
    print("\n" + "="*60)
    print("Approach 2: Risk-control (per-budget safety prediction)")
    print("="*60)

    X_train = train_df[feature_cols]
    X_test = test_df[feature_cols]

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
            print(f"  Skipping")
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

    # Feature importance for B=32 risk model
    if 32 in risk_models:
        print("\nRisk model B=32 feature importance (top 10):")
        importance = risk_models[32].feature_importance(importance_type='gain')
        names = risk_models[32].feature_name()
        for name, imp in sorted(zip(names, importance), key=lambda x: -x[1])[:10]:
            print(f"  {name}: {imp:.1f}")

    # Use risk models for budget assignment
    print(f"\n{'Risk Thr':<12} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B40%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*75)

    for threshold in [0.80, 0.85, 0.90, 0.95, 0.97, 0.99]:
        assignments = {}
        for i, (_, row) in enumerate(test_df.iterrows()):
            qid = int(row['query_id'])
            x = X_test.iloc[i:i+1]

            budget = 64
            for b in [32, 40, 48]:
                if b in risk_models:
                    prob_safe = risk_models[b].predict(x)[0]
                    if prob_safe >= threshold:
                        budget = b
                        break

            assignments[qid] = budget

        saving, miss_rate, budget_counts = evaluate_budget_assignment(
            test_df, assignments, budget_stats, pages_data)

        n = len(test_df)
        b32 = 100 * budget_counts.get(32, 0) / n
        b40 = 100 * budget_counts.get(40, 0) / n
        b48 = 100 * budget_counts.get(48, 0) / n
        b64 = 100 * budget_counts.get(64, 0) / n

        status = ""
        if saving >= 12 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 8 and miss_rate <= 2:
            status = " ⚠️"

        print(f"{threshold:<12.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {b32:>7.1f}% {b40:>7.1f}% {b48:>7.1f}% {b64:>7.1f}%{status}")

    return risk_models


def main():
    print("="*60)
    print("Phase 4: Training Learned Policy (SIFT1M)")
    print("="*60)

    train_df, test_df, feature_cols, budget_stats, pages_data = load_data()
    print(f"Train: {len(train_df)}, Test: {len(test_df)}")
    print(f"Features: {len(feature_cols)}")

    # Baseline: rule-based
    print("\n" + "="*60)
    print("Baseline: Rule-based (margin_16 top 40% -> B=48)")
    print("="*60)

    valid = pd.concat([train_df, test_df])  # all data
    thresh = valid['margin_16'].quantile(0.60)  # top 40%

    rule_assignments = {}
    for _, row in test_df.iterrows():
        qid = int(row['query_id'])
        rule_assignments[qid] = 48 if row['margin_16'] >= thresh else 64

    rule_saving, rule_miss, _ = evaluate_budget_assignment(test_df, rule_assignments, budget_stats, pages_data)
    print(f"Rule-based: saving={rule_saving:.1f}%, miss_rate={rule_miss:.1f}%")

    # Approach 1: Regression
    reg_model = approach1_regression(train_df, test_df, feature_cols, budget_stats, pages_data)

    # Approach 2: Risk-control
    risk_models = approach2_risk_control(train_df, test_df, feature_cols, budget_stats, pages_data)

    # Save models
    reg_model.save_model('results/adaptive_budget/phase4_learned/regression_model.txt')
    for b, model in risk_models.items():
        model.save_model(f'results/adaptive_budget/phase4_learned/risk_model_b{b}.txt')

    print("\nModels saved to results/adaptive_budget/phase4_learned/")


if __name__ == '__main__':
    main()
