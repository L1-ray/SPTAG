#!/usr/bin/env python3
"""
用 ir=64 数据训练 Learned Policy
Step 1: 从 head_distance_trace.csv 提取特征
Step 2: 训练 Risk Model
"""

import pandas as pd
import numpy as np
import lightgbm as lgb
import json
import os
from sklearn.model_selection import train_test_split

RESULT_DIR = '/home/ray/code/SPTAG/results/adaptive_budget/sift1m_ir64_retrain'

def extract_features_from_trace():
    """从 head_distance_trace.csv 提取特征"""
    print("Loading head distance trace...")
    trace_df = pd.read_csv(f'{RESULT_DIR}/head_distance_trace.csv')

    print(f"Total rows: {len(trace_df)}")
    print(f"Unique queries: {trace_df['query_id'].nunique()}")

    # 提取特征
    features_list = []

    for query_id, group in trace_df.groupby('query_id'):
        # 按距离排序（head search 已经排序）
        distances = group['head_dist'].values

        # ir=64 可用的特征
        d1 = distances[0] if len(distances) > 0 else 0
        d2 = distances[1] if len(distances) > 1 else d1
        d4 = distances[3] if len(distances) > 3 else d1
        d8 = distances[7] if len(distances) > 7 else d1
        d16 = distances[15] if len(distances) > 15 else d1
        d32 = distances[31] if len(distances) > 31 else d1
        d64 = distances[63] if len(distances) > 63 else d1

        # Margin (相对 d1 的增长率)
        d1_safe = d1 if d1 > 0.001 else 0.001
        margin_2 = (d2 - d1) / d1_safe
        margin_4 = (d4 - d1) / d1_safe
        margin_8 = (d8 - d1) / d1_safe
        margin_16 = (d16 - d1) / d1_safe
        margin_32 = (d32 - d1) / d1_safe
        margin_64 = (d64 - d1) / d1_safe

        # Ratio
        ratio_8 = d8 / d1_safe
        ratio_16 = d16 / d1_safe
        ratio_64 = d64 / d1_safe

        # Slope
        slope_1_8 = (d8 - d1) / 7 if len(distances) > 7 else 0
        slope_8_16 = (d16 - d8) / 8 if len(distances) > 15 else 0
        slope_16_64 = (d64 - d16) / 48 if len(distances) > 63 else 0

        # Variance
        var_16 = np.var(distances[:16]) if len(distances) >= 16 else 0
        var_64 = np.var(distances[:64]) if len(distances) >= 64 else 0

        # Entropy (softmax over negative distances)
        def entropy(dist_arr):
            if len(dist_arr) < 2:
                return 0
            max_val = np.max(dist_arr)
            exp_vals = np.exp(-(dist_arr - max_val))
            probs = exp_vals / np.sum(exp_vals)
            return -np.sum(probs * np.log(probs + 1e-10))

        entropy_16 = entropy(distances[:16])
        entropy_64 = entropy(distances[:64])

        # Cross-margin ratio
        margin_16_32_ratio = margin_16 / margin_32 if margin_32 != 0 else 0

        features_list.append({
            'query_id': query_id,
            'd1': d1, 'd2': d2, 'd4': d4, 'd8': d8, 'd16': d16, 'd32': d32, 'd64': d64,
            'margin_2': margin_2, 'margin_4': margin_4, 'margin_8': margin_8,
            'margin_16': margin_16, 'margin_32': margin_32, 'margin_64': margin_64,
            'ratio_8': ratio_8, 'ratio_16': ratio_16, 'ratio_64': ratio_64,
            'slope_1_8': slope_1_8, 'slope_8_16': slope_8_16, 'slope_16_64': slope_16_64,
            'var_16': var_16, 'var_64': var_64,
            'entropy_16': entropy_16, 'entropy_64': entropy_64,
            'margin_16_32_ratio': margin_16_32_ratio
        })

    features_df = pd.DataFrame(features_list)
    print(f"Extracted features for {len(features_df)} queries")

    return features_df

def load_budget_sweep_data():
    """加载 budget sweep 数据"""
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    pages_data = {}

    for b in budgets:
        csv_path = f'{RESULT_DIR}/budget_{b}.csv'
        if os.path.exists(csv_path):
            df = pd.read_csv(csv_path)
            budget_stats[b] = df.set_index('query_id')['recall'].to_dict()
            pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()

    return budget_stats, pages_data

def train_risk_models(train_df, test_df, feature_cols, budget_stats, pages_data):
    """训练 Risk Model"""
    print("\n" + "="*60)
    print("Training Risk Models")
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
        print(f"  Safe rate at B={b}: {safe_rate:.3f} ({safe_rate*100:.1f}%)")

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

        # 特征重要性
        importance = model.feature_importance(importance_type='gain')
        names = model.feature_name()
        print(f"  Top 5 features:")
        for name, imp in sorted(zip(names, importance), key=lambda x: -x[1])[:5]:
            print(f"    {name}: {imp:.1f}")

    return risk_models

def evaluate_model(test_df, feature_cols, risk_models, budget_stats, pages_data, threshold):
    """评估模型"""
    X_test = test_df[feature_cols]

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

    # 计算指标
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

def main():
    print("="*60)
    print("SIFT1M Learned Policy Training (ir=64)")
    print("="*60)

    # 1. 提取特征
    print("\n[1] Extracting features from trace...")
    features_df = extract_features_from_trace()

    # 2. 加载 budget sweep 数据
    print("\n[2] Loading budget sweep data...")
    budget_stats, pages_data = load_budget_sweep_data()
    print(f"  Budgets loaded: {list(budget_stats.keys())}")

    # 3. 分割训练/测试集
    print("\n[3] Splitting train/test...")
    train_df, test_df = train_test_split(features_df, test_size=0.2, random_state=42)
    print(f"  Train: {len(train_df)}, Test: {len(test_df)}")

    # 保存数据
    train_df.to_csv(f'{RESULT_DIR}/train_data.csv', index=False)
    test_df.to_csv(f'{RESULT_DIR}/test_data.csv', index=False)
    print(f"  Saved train_data.csv and test_data.csv")

    # 4. 特征列
    feature_cols = [c for c in features_df.columns if c != 'query_id']
    print(f"  Features ({len(feature_cols)}): {feature_cols}")

    with open(f'{RESULT_DIR}/feature_cols.json', 'w') as f:
        json.dump(feature_cols, f)

    # 5. 训练模型
    print("\n[4] Training models...")
    risk_models = train_risk_models(train_df, test_df, feature_cols, budget_stats, pages_data)

    # 6. 评估不同 threshold
    print("\n[5] Evaluating thresholds...")
    print(f"\n{'Threshold':<12} {'Saving':>10} {'Miss Rate':>12} {'B32%':>8} {'B40%':>8} {'B48%':>8} {'B64%':>8} {'Status'}")
    print("-" * 85)

    results = []
    for threshold in [0.80, 0.85, 0.90, 0.95, 0.97]:
        saving, miss_rate, budget_counts = evaluate_model(
            test_df, feature_cols, risk_models, budget_stats, pages_data, threshold)

        n = len(test_df)
        b32 = 100 * budget_counts.get(32, 0) / n
        b40 = 100 * budget_counts.get(40, 0) / n
        b48 = 100 * budget_counts.get(48, 0) / n
        b64 = 100 * budget_counts.get(64, 0) / n

        status = "✅" if saving >= 12 and miss_rate <= 2 else "⚠️" if miss_rate <= 2 else "❌"
        print(f"{threshold:<12} {saving:>9.1f}% {miss_rate:>11.2f}% {b32:>7.1f}% {b40:>7.1f}% {b48:>7.1f}% {b64:>7.1f}% {status}")

        results.append((threshold, saving, miss_rate))

    # 7. 保存模型
    print("\n[6] Saving models...")
    for b, model in risk_models.items():
        model_path = f'{RESULT_DIR}/risk_model_b{b}.txt'
        model.save_model(model_path)
        print(f"  Saved: {model_path}")

        # JSON 格式
        json_path = f'{RESULT_DIR}/risk_model_b{b}.json'
        json_str = model.dump_model()
        with open(json_path, 'w') as f:
            f.write(json_str)
        print(f"  Saved: {json_path}")

    # 8. 推荐配置
    print("\n[7] Recommended configuration:")
    valid = [(t, s, m) for t, s, m in results if s >= 12 and m <= 2]
    if valid:
        best = min(valid, key=lambda x: x[2])
        print(f"  LearnedBudgetThreshold={best[0]}")
        print(f"  Expected: saving={best[1]:.1f}%, miss_rate={best[2]:.2f}%")
    else:
        # 找 miss_rate <= 2 中 saving 最大的
        valid2 = [(t, s, m) for t, s, m in results if m <= 2]
        if valid2:
            best = max(valid2, key=lambda x: x[1])
            print(f"  LearnedBudgetThreshold={best[0]} (best available)")
            print(f"  Expected: saving={best[1]:.1f}%, miss_rate={best[2]:.2f}%")
        else:
            print("  Warning: No threshold meets miss_rate <= 2%")

    print("\n" + "="*60)
    print("Training completed!")
    print(f"Models saved to: {RESULT_DIR}")
    print("="*60)

if __name__ == '__main__':
    main()
