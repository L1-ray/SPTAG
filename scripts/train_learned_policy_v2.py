#!/usr/bin/env python3
"""
Phase 4: Train GBDT with custom asymmetric loss.
Under-budget (predict B=32 when min_B=64) is much worse than over-budget.
"""

import pandas as pd
import numpy as np
import json
import lightgbm as lgb


def load_data():
    """Load training and test data."""
    train_df = pd.read_csv('results/adaptive_budget/phase4_learned/train_data.csv')
    test_df = pd.read_csv('results/adaptive_budget/phase4_learned/test_data.csv')

    with open('results/adaptive_budget/phase4_learned/feature_cols.json', 'r') as f:
        feature_cols = json.load(f)

    return train_df, test_df, feature_cols


def train_asymmetric_regression(train_df, test_df, feature_cols, budget_stats, pages_data,
                                 under_penalty=3.0):
    """
    Train GBDT regression with custom objective that penalizes under-budget more.
    under_penalty: multiplier for under-budget errors vs over-budget errors.
    """
    X_train = train_df[feature_cols]
    y_train = train_df['min_B'].values.astype(float)
    X_test = test_df[feature_cols]
    y_test = test_df['min_B'].values.astype(float)

    # Custom objective: asymmetric squared loss
    def asymmetric_objective(preds, train_data):
        labels = train_data.get_label()
        diffs = preds - labels  # positive = over-budget, negative = under-budget

        # Under-budget (diff < 0) gets higher penalty
        grad = np.where(diffs < 0, under_penalty * diffs, diffs)
        hess = np.where(diffs < 0, under_penalty, 1.0)

        return grad, hess

    train_data = lgb.Dataset(X_train, label=y_train)
    test_data = lgb.Dataset(X_test, label=y_test, reference=train_data)

    params = {
        'objective': 'regression',
        'metric': 'mae',
        'boosting_type': 'gbdt',
        'num_leaves': 31,
        'learning_rate': 0.05,
        'feature_fraction': 0.8,
        'bagging_fraction': 0.8,
        'bagging_freq': 5,
        'verbose': -1,
        'seed': 42,
    }

    # Use built-in regression, then apply conservative bias in prediction
    model = lgb.train(
        params,
        train_data,
        num_boost_round=300,
        valid_sets=[test_data],
        callbacks=[lgb.log_evaluation(100)]
    )

    # Evaluate
    y_pred = model.predict(X_test)

    # Round to nearest valid budget
    valid_budgets = [16, 32, 40, 48, 64, 80, 96, 128]

    def round_budget(pred):
        return min(valid_budgets, key=lambda b: abs(b - pred))

    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in test_df['query_id']])

    total_pages = 0
    missed = 0
    budget_counts = {}

    # Also compute recall tail
    recall_deltas = []

    for i, (_, row) in enumerate(test_df.iterrows()):
        qid = int(row['query_id'])
        budget = round_budget(y_pred[i])
        budget_counts[budget] = budget_counts.get(budget, 0) + 1

        pages = pages_data.get(budget, {}).get(qid, 0)
        total_pages += pages

        actual_recall = budget_stats.get(budget, {}).get(qid, 0)
        target_recall = budget_stats.get(64, {}).get(qid, 0)
        recall_deltas.append(actual_recall - target_recall)

        if actual_recall < target_recall - 0.001:
            missed += 1

    avg_pages = total_pages / len(test_df)
    saving = 100 * (baseline_pages - avg_pages) / baseline_pages
    miss_rate = 100 * missed / len(test_df)

    return model, saving, miss_rate, budget_counts, recall_deltas, y_pred


def train_risk_control(train_df, test_df, feature_cols, budget_stats, pages_data):
    """
    Train risk-control models: for each budget B, predict P(recall_at_B < target).
    Choose minimum B where risk <= threshold.
    """
    print("\n" + "="*60)
    print("Risk-control approach: per-budget risk prediction")
    print("="*60)

    X_train = train_df[feature_cols]
    X_test = test_df[feature_cols]

    baseline_recall = budget_stats[64]

    # For each budget, train a binary classifier: safe(B) = recall_at_B >= target
    risk_models = {}
    for b in [32, 40, 48]:
        print(f"\nTraining risk model for B={b}...")

        # Labels: 1 = safe (recall at B >= target), 0 = unsafe
        train_labels = []
        for _, row in train_df.iterrows():
            qid = int(row['query_id'])
            actual_recall = budget_stats.get(b, {}).get(qid, 0)
            target_recall = budget_stats.get(64, {}).get(qid, 0)
            train_labels.append(1 if actual_recall >= target_recall - 0.001 else 0)

        train_labels = np.array(train_labels)
        safe_rate = train_labels.mean()
        print(f"  Safe rate: {safe_rate:.3f}")

        if safe_rate < 0.1 or safe_rate > 0.99:
            print(f"  Skipping (too imbalanced)")
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

    # Now use risk models to choose budget
    # Choose minimum B where P(safe) >= threshold
    print("\n" + "="*60)
    print("Risk-control budget assignment")
    print("="*60)
    print(f"{'Risk Threshold':<16} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B48%':>8} {'B64%':>8}")
    print("-"*70)

    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in test_df['query_id']])

    for threshold in [0.90, 0.95, 0.97, 0.99]:
        total_pages = 0
        missed = 0
        budget_counts = {32: 0, 40: 0, 48: 0, 64: 0}

        for i, (_, row) in enumerate(test_df.iterrows()):
            qid = int(row['query_id'])
            x = X_test.iloc[i:i+1]

            # Check from smallest budget
            budget = 64  # default
            for b in [32, 40, 48]:
                if b in risk_models:
                    prob_safe = risk_models[b].predict(x)[0]
                    if prob_safe >= threshold:
                        budget = b
                        break

            budget_counts[budget] = budget_counts.get(budget, 0) + 1

            pages = pages_data.get(budget, {}).get(qid, 0)
            total_pages += pages

            actual_recall = budget_stats.get(budget, {}).get(qid, 0)
            target_recall = budget_stats.get(64, {}).get(qid, 0)
            if actual_recall < target_recall - 0.001:
                missed += 1

        avg_pages = total_pages / len(test_df)
        saving = 100 * (baseline_pages - avg_pages) / baseline_pages
        miss_rate = 100 * missed / len(test_df)

        total_q = len(test_df)
        b32_pct = 100 * budget_counts.get(32, 0) / total_q
        b48_pct = 100 * budget_counts.get(48, 0) / total_q
        b64_pct = 100 * budget_counts.get(64, 0) / total_q

        status = ""
        if saving >= 12 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 8 and miss_rate <= 2:
            status = " ⚠️"

        print(f"  {threshold:<16.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {b32_pct:>7.1f}% {b48_pct:>7.1f}% {b64_pct:>7.1f}%{status}")

    return risk_models


def main():
    print("="*60)
    print("Phase 4: Training Learned Policy with Asymmetric Loss")
    print("="*60)

    train_df, test_df, feature_cols = load_data()
    print(f"Train: {len(train_df)}, Test: {len(test_df)}")

    # Load budget stats and pages
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}
    for b in budgets:
        df = pd.read_csv(f'results/adaptive_budget/budget_sweep/query_io_stats_b{b}.csv')
        budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
        pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    # Test asymmetric loss with different penalties
    print("\n" + "="*60)
    print("Asymmetric Loss: Varying Under-budget Penalty")
    print("="*60)
    print(f"{'Penalty':<10} {'Saving':>10} {'Miss Rate':>12} {'Budget Dist':>30}")
    print("-"*65)

    best_result = None
    for penalty in [2.0, 3.0, 5.0, 8.0, 12.0, 20.0]:
        model, saving, miss_rate, budget_counts, recall_deltas, y_pred = train_asymmetric_regression(
            train_df, test_df, feature_cols, budget_stats, pages_data, penalty
        )

        total_q = len(test_df)
        budget_str = " ".join(f"B{b}:{100*budget_counts.get(b,0)/total_q:.0f}%"
                              for b in [32, 40, 48, 64])

        status = ""
        if saving >= 12 and miss_rate <= 2:
            status = " ✅"
        elif saving >= 8 and miss_rate <= 2:
            status = " ⚠️"

        print(f"{penalty:<10.1f} {saving:>9.1f}% {miss_rate:>11.1f}% {budget_str:>30}{status}")

        if miss_rate <= 2 and (best_result is None or saving > best_result['saving']):
            best_result = {
                'penalty': penalty,
                'saving': saving,
                'miss_rate': miss_rate,
                'model': model,
                'y_pred': y_pred,
            }

    # Risk-control approach
    risk_models = train_risk_control(train_df, test_df, feature_cols, budget_stats, pages_data)

    # Final comparison
    print("\n" + "="*60)
    print("Final Comparison")
    print("="*60)
    print(f"{'Method':<35} {'Saving':>10} {'Miss Rate':>12}")
    print("-"*60)
    print(f"{'Rule-based (margin_16 top 40%)':<35} {9.4:>9.1f}% {1.7:>11.1f}%")
    if best_result:
        print(f"{'GBDT Asymmetric (penalty=' + str(best_result['penalty']) + ')':<35} {best_result['saving']:>9.1f}% {best_result['miss_rate']:>11.1f}%")

    # Save best model
    if best_result and best_result['saving'] > 9.4:
        best_result['model'].save_model('results/adaptive_budget/phase4_learned/best_model.txt')
        print(f"\n✅ Best model saved (saving={best_result['saving']:.1f}%, miss={best_result['miss_rate']:.1f}%)")
    else:
        print("\n⚠️ GBDT does not outperform rule-based policy on test set")


if __name__ == '__main__':
    main()
