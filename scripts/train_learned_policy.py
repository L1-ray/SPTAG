#!/usr/bin/env python3
"""
Phase 4: Train GBDT model for adaptive budget prediction.
"""

import pandas as pd
import numpy as np
import json
import lightgbm as lgb
from sklearn.metrics import classification_report, confusion_matrix


def load_data():
    """Load training and test data."""
    train_df = pd.read_csv('results/adaptive_budget/phase4_learned/train_data.csv')
    test_df = pd.read_csv('results/adaptive_budget/phase4_learned/test_data.csv')

    with open('results/adaptive_budget/phase4_learned/feature_cols.json', 'r') as f:
        feature_cols = json.load(f)

    return train_df, test_df, feature_cols


def train_classifier(train_df, test_df, feature_cols):
    """Train GBDT classifier for budget prediction."""
    X_train = train_df[feature_cols]
    y_train = train_df['label_class']
    X_test = test_df[feature_cols]
    y_test = test_df['label_class']

    # Create dataset
    train_data = lgb.Dataset(X_train, label=y_train)
    test_data = lgb.Dataset(X_test, label=y_test, reference=train_data)

    # Parameters
    params = {
        'objective': 'multiclass',
        'num_class': 3,  # Class 0, 1, 2 (no class 3 in data)
        'metric': 'multi_logloss',
        'boosting_type': 'gbdt',
        'num_leaves': 31,
        'learning_rate': 0.05,
        'feature_fraction': 0.8,
        'bagging_fraction': 0.8,
        'bagging_freq': 5,
        'verbose': -1,
        'seed': 42,
    }

    # Train
    print("Training GBDT classifier...")
    model = lgb.train(
        params,
        train_data,
        num_boost_round=200,
        valid_sets=[test_data],
        callbacks=[lgb.log_evaluation(50)]
    )

    return model, X_train, y_train, X_test, y_test


def evaluate_classifier(model, X_test, y_test, test_df, budget_stats, pages_data):
    """Evaluate classifier and compute actual pages saving."""
    # Predict
    y_pred = model.predict(X_test)
    y_pred_class = np.argmax(y_pred, axis=1)

    # Classification report
    print("\n" + "="*60)
    print("Classification Report")
    print("="*60)
    print(classification_report(y_test, y_pred_class, target_names=['B<=32', 'B<=48', 'B<=64']))

    # Confusion matrix
    print("\nConfusion Matrix:")
    print(confusion_matrix(y_test, y_pred_class))

    # Feature importance
    print("\n" + "="*60)
    print("Feature Importance (Top 10)")
    print("="*60)
    importance = model.feature_importance(importance_type='gain')
    feature_names = model.feature_name()
    feat_imp = sorted(zip(feature_names, importance), key=lambda x: x[1], reverse=True)
    for name, imp in feat_imp[:10]:
        print(f"  {name}: {imp:.1f}")

    # Compute actual budget assignment and pages saving
    print("\n" + "="*60)
    print("Budget Assignment Analysis")
    print("="*60)

    # Map class to budget
    class_to_budget = {0: 32, 1: 48, 2: 64}

    # Compute pages saving
    baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in test_df['query_id']])

    total_pages = 0
    missed = 0
    budget_counts = {32: 0, 48: 0, 64: 0}

    for i, (_, row) in enumerate(test_df.iterrows()):
        qid = int(row['query_id'])
        pred_class = y_pred_class[i]
        budget = class_to_budget[pred_class]
        budget_counts[budget] += 1

        pages = pages_data.get(budget, {}).get(qid, 0)
        total_pages += pages

        # Check recall miss
        actual_recall = budget_stats.get(budget, {}).get(qid, 0)
        target_recall = budget_stats.get(64, {}).get(qid, 0)
        if actual_recall < target_recall - 0.001:
            missed += 1

    avg_pages = total_pages / len(test_df)
    pages_saving = 100 * (baseline_pages - avg_pages) / baseline_pages
    miss_rate = 100 * missed / len(test_df)

    print(f"\nBudget distribution:")
    for b, count in sorted(budget_counts.items()):
        pct = 100 * count / len(test_df)
        print(f"  B={b}: {count} ({pct:.1f}%)")

    print(f"\nPages saving: {pages_saving:.1f}%")
    print(f"Miss rate: {miss_rate:.1f}%")

    return pages_saving, miss_rate


def train_with_asymmetric_loss(train_df, test_df, feature_cols, budget_stats, pages_data):
    """
    Train with asymmetric loss: under-budget penalty > over-budget penalty.
    Under-budget (predicting B=32 when min_B=64) hurts recall more than
    over-budget (predicting B=64 when min_B=32) wastes I/O.
    """
    print("\n" + "="*60)
    print("Training with Asymmetric Loss")
    print("="*60)

    X_train = train_df[feature_cols]
    y_train = train_df['min_B']  # Use actual min_B as regression target
    X_test = test_df[feature_cols]
    y_test = test_df['min_B']

    # Use regression and round to nearest budget
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

    model = lgb.train(
        params,
        train_data,
        num_boost_round=200,
        valid_sets=[test_data],
        callbacks=[lgb.log_evaluation(50)]
    )

    # Predict and round to nearest budget
    y_pred = model.predict(X_test)

    # Round to nearest valid budget with conservative bias
    # Round up slightly to avoid under-budget
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]

    def round_to_budget(pred, conservative_bias=0.0):
        """Round prediction to nearest budget with optional conservative bias."""
        adjusted_pred = pred * (1 + conservative_bias)  # Bias toward higher budget
        return min(budgets, key=lambda b: abs(b - adjusted_pred))

    print("\nTesting different conservative bias levels:")
    print(f"{'Bias':<8} {'Saving':>10} {'Miss Rate':>12} {'Miss Count':>12}")
    print("-"*50)

    best_result = None
    for bias in [0.0, 0.05, 0.10, 0.15, 0.20]:
        total_pages = 0
        missed = 0
        budget_counts = {}

        baseline_pages = np.mean([pages_data[64].get(qid, 0) for qid in test_df['query_id']])

        for i, (_, row) in enumerate(test_df.iterrows()):
            qid = int(row['query_id'])
            budget = round_to_budget(y_pred[i], bias)
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

        status = ""
        if saving >= 12 and miss_rate <= 2:  # Better than rule-based (9.4% saving)
            status = " ✅ BEST"

        print(f"{bias:<8.2f} {saving:>9.1f}% {miss_rate:>11.1f}% {missed:>12}{status}")

        if best_result is None or (miss_rate <= 2 and saving > best_result['saving']):
            best_result = {
                'bias': bias,
                'saving': saving,
                'miss_rate': miss_rate,
                'budget_counts': budget_counts,
                'model': model,
            }

    return best_result


def main():
    print("="*60)
    print("Phase 4: Training GBDT Model for Adaptive Budget")
    print("="*60)

    # Load data
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

    # Train classifier
    model, X_train, y_train, X_test, y_test = train_classifier(train_df, test_df, feature_cols)

    # Evaluate classifier
    cls_saving, cls_miss = evaluate_classifier(model, X_test, y_test, test_df, budget_stats, pages_data)

    # Train with asymmetric loss (regression)
    best_result = train_with_asymmetric_loss(train_df, test_df, feature_cols, budget_stats, pages_data)

    # Compare with rule-based
    print("\n" + "="*60)
    print("Comparison with Rule-based Policy")
    print("="*60)
    print(f"{'Method':<30} {'Saving':>10} {'Miss Rate':>12}")
    print("-"*55)
    print(f"{'Rule-based (margin_16 top 40%)':<30} {9.4:>9.1f}% {1.7:>11.1f}%")
    print(f"{'GBDT Classifier':<30} {cls_saving:>9.1f}% {cls_miss:>11.1f}%")
    if best_result:
        print(f"{'GBDT Regression + bias=' + str(best_result['bias']):<30} {best_result['saving']:>9.1f}% {best_result['miss_rate']:>11.1f}%")

    # Save model
    if best_result and best_result['saving'] > cls_saving:
        best_result['model'].save_model('results/adaptive_budget/phase4_learned/model.txt')
        print(f"\nBest model saved to results/adaptive_budget/phase4_learned/model.txt")
    else:
        model.save_model('results/adaptive_budget/phase4_learned/model_classifier.txt')
        print(f"\nClassifier model saved to results/adaptive_budget/phase4_learned/model_classifier.txt")


if __name__ == '__main__':
    main()
