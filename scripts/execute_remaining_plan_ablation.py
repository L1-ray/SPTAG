#!/usr/bin/env python3
"""Execute remaining plan ablations: A5/A6/A7 (offline)."""

import json
from pathlib import Path
from typing import Dict, List

import lightgbm as lgb
import numpy as np
import pandas as pd

REPO = Path('/home/ray/code/SPTAG')
OUT = REPO / 'results' / 'adaptive_budget' / 'budget_granularity_plan_20260505' / 'remaining_ablation'
OUT.mkdir(parents=True, exist_ok=True)

DATASETS = {
    'sift1m': {
        'dir': REPO / 'results' / 'adaptive_budget' / 'sift1m_ir64_retrain',
        'a5_sparse': {32: 0.98, 40: 0.96, 48: 0.93, 56: 0.88},
        'a5_dense': {16: 0.996, 24: 0.990, 32: 0.980, 40: 0.960, 48: 0.930, 56: 0.890},
    },
    'sift10m': {
        'dir': REPO / 'results' / 'adaptive_budget' / 'sift10m_ir64_retrain',
        'a5_sparse': {32: 0.95, 40: 0.92, 48: 0.88, 56: 0.82},
        # conservative dense to control tail risk for SIFT10M
        'a5_dense': {16: 0.999, 24: 0.997, 32: 0.98, 40: 0.96, 48: 0.94, 56: 0.88},
    },
}

BUDGETS_ALL = [16, 24, 32, 40, 48, 56, 64, 80, 96, 128]
BUDGETS_CAND = [16, 24, 32, 40, 48, 56, 64]


def load_ds(d: Path):
    train = pd.read_csv(d / 'train_data.csv')
    test = pd.read_csv(d / 'test_data.csv')
    feat = json.loads((d / 'feature_cols.json').read_text())
    recall = {}
    pages = {}
    for b in BUDGETS_ALL:
        p = d / f'budget_{b}.csv'
        if p.exists():
            df = pd.read_csv(p)
            recall[b] = df.set_index('query_id')['recall'].to_dict()
            pages[b] = df.set_index('query_id')['pages_read'].to_dict()
    return train, test, feat, recall, pages


def train_risk_models(train, feat_cols, recall, budgets):
    x = train[feat_cols]
    models = {}
    safe_rates = {}
    for b in budgets:
        if b not in recall:
            continue
        y = np.array([
            1 if recall[b].get(int(q), 0.0) >= recall[64].get(int(q), 0.0) - 0.001 else 0
            for q in train['query_id']
        ])
        safe_rates[b] = float(y.mean())
        if y.mean() < 0.01 or y.mean() > 0.995:
            continue
        dtrain = lgb.Dataset(x, label=y)
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
        models[b] = lgb.train(params, dtrain, num_boost_round=200)
    return models, safe_rates


def assign_risk_control(models, x, qids, thresholds, fallback=64):
    budgets = sorted(thresholds.keys())
    out = {}
    for i, q in enumerate(qids):
        row = x.iloc[i:i+1]
        bsel = fallback
        for b in budgets:
            if b not in models:
                continue
            p = float(models[b].predict(row)[0])
            if p >= thresholds[b]:
                bsel = b
                break
        out[q] = bsel
    return out


def eval_assign(name, qids, assign, recall, pages):
    base_pages = np.mean([pages[64][q] for q in qids])
    base_recall = np.mean([recall[64][q] for q in qids])
    r = np.array([recall[assign[q]][q] for q in qids])
    p = np.array([pages[assign[q]][q] for q in qids])
    counts = {}
    for q in qids:
        counts[assign[q]] = counts.get(assign[q], 0) + 1
    return {
        'policy': name,
        'pages_saving_pct': 100.0 * (base_pages - p.mean()) / base_pages,
        'recall_delta_vs64': float(r.mean() - base_recall),
        'miss_rate_pct': 100.0 * np.mean(r < np.array([recall[64][q] for q in qids]) - 0.001),
        'low07': int(np.sum(r < 0.7)),
        'low05': int(np.sum(r < 0.5)),
        'b16_pct': 100.0 * counts.get(16, 0) / len(qids),
        'b24_pct': 100.0 * counts.get(24, 0) / len(qids),
        'b32_pct': 100.0 * counts.get(32, 0) / len(qids),
        'b40_pct': 100.0 * counts.get(40, 0) / len(qids),
        'b48_pct': 100.0 * counts.get(48, 0) / len(qids),
        'b56_pct': 100.0 * counts.get(56, 0) / len(qids),
        'b64_pct': 100.0 * counts.get(64, 0) / len(qids),
    }


def fit_multiclass_and_regression(train, test, feat_cols, recall):
    # labels: oracle min_B class under recall>=baseline-0.001
    label_budget = []
    for q in train['query_id']:
        q = int(q)
        target = recall[64][q] - 0.001
        chosen = 64
        for b in BUDGETS_CAND:
            if b in recall and recall[b][q] >= target:
                chosen = b
                break
        label_budget.append(chosen)

    class_budgets = BUDGETS_CAND
    b2c = {b: i for i, b in enumerate(class_budgets)}
    c2b = {i: b for i, b in enumerate(class_budgets)}
    y_cls = np.array([b2c[b] for b in label_budget])
    x_train = train[feat_cols]
    x_test = test[feat_cols]

    dtrain = lgb.Dataset(x_train, label=y_cls)
    params_cls = {
        'objective': 'multiclass',
        'num_class': len(class_budgets),
        'metric': 'multi_logloss',
        'learning_rate': 0.05,
        'num_leaves': 31,
        'feature_fraction': 0.8,
        'verbose': -1,
        'seed': 42,
    }
    mcls = lgb.train(params_cls, dtrain, num_boost_round=300)
    pred_prob = mcls.predict(x_test)
    pred_cls = pred_prob.argmax(axis=1)
    pred_budget_cls = [c2b[int(c)] for c in pred_cls]

    # regression on oracle min_B
    y_reg = np.array(label_budget, dtype=float)
    dtrain_reg = lgb.Dataset(x_train, label=y_reg)
    params_reg = {
        'objective': 'regression_l2',
        'metric': 'l2',
        'learning_rate': 0.05,
        'num_leaves': 31,
        'feature_fraction': 0.8,
        'verbose': -1,
        'seed': 42,
    }
    mreg = lgb.train(params_reg, dtrain_reg, num_boost_round=300)
    pred = mreg.predict(x_test)

    def round_to_budget(v):
        return min(class_budgets, key=lambda b: abs(b - float(v)))

    pred_budget_reg = [round_to_budget(v) for v in pred]
    return pred_budget_cls, pred_budget_reg


def oracle_studies(qids, recall, pages):
    # posting-budget oracle: minimal B satisfying recall target
    assign_post = {}
    assign_page = {}
    available = [b for b in BUDGETS_CAND if b in recall]
    for q in qids:
        target = recall[64][q] - 0.001
        cand = [b for b in available if recall[b][q] >= target]
        if not cand:
            assign_post[q] = 64
            assign_page[q] = 64
            continue
        assign_post[q] = min(cand)
        # page-aware oracle: among satisfying budgets, choose minimal pages
        assign_page[q] = min(cand, key=lambda b: pages[b][q])
    return assign_post, assign_page


def run_one_dataset(name, cfg):
    d = cfg['dir']
    train, test, feat_cols, recall, pages = load_ds(d)
    qids = [int(q) for q in test['query_id']]
    x = test[feat_cols]

    models, safe_rates = train_risk_models(train, feat_cols, recall, [16, 24, 32, 40, 48, 56])

    # A5 sparse vs dense
    a5_sparse_assign = assign_risk_control(models, x, qids, cfg['a5_sparse'], fallback=64)
    a5_dense_assign = assign_risk_control(models, x, qids, cfg['a5_dense'], fallback=64)

    rows = []
    rows.append(eval_assign('A5_sparse', qids, a5_sparse_assign, recall, pages))
    rows.append(eval_assign('A5_dense', qids, a5_dense_assign, recall, pages))

    # A6 multiclass/regression
    cls_b, reg_b = fit_multiclass_and_regression(train, test, feat_cols, recall)
    cls_assign = {q: int(b) for q, b in zip(qids, cls_b)}
    reg_assign = {q: int(b) for q, b in zip(qids, reg_b)}
    rows.append(eval_assign('A6_multiclass', qids, cls_assign, recall, pages))
    rows.append(eval_assign('A6_regression', qids, reg_assign, recall, pages))

    # A7 oracle studies
    opost, opage = oracle_studies(qids, recall, pages)
    rows.append(eval_assign('A7_oracle_posting', qids, opost, recall, pages))
    rows.append(eval_assign('A7_oracle_page', qids, opage, recall, pages))

    out = pd.DataFrame(rows)
    out.insert(0, 'dataset', name)

    safe_df = pd.DataFrame([{'budget': b, 'safe_rate_train': r} for b, r in sorted(safe_rates.items())])
    safe_df.to_csv(OUT / f'{name}_safe_rates.csv', index=False)
    out.to_csv(OUT / f'{name}_remaining_ablation.csv', index=False)

    meta = {
        'dataset': name,
        'threshold_sparse': cfg['a5_sparse'],
        'threshold_dense': cfg['a5_dense'],
        'trained_budgets': sorted(models.keys()),
        'test_queries': len(qids),
    }
    (OUT / f'{name}_meta.json').write_text(json.dumps(meta, indent=2))

    return out


def main():
    all_df = []
    for name, cfg in DATASETS.items():
        all_df.append(run_one_dataset(name, cfg))

    final = pd.concat(all_df, ignore_index=True)
    final.to_csv(OUT / 'remaining_ablation_summary.csv', index=False)

    with (OUT / 'remaining_ablation_report.md').open('w') as f:
        f.write('# Remaining Plan Ablations (A5/A6/A7)\n\n')
        f.write('Date: 2026-05-05\n\n')
        f.write('```csv\n')
        f.write(final.to_csv(index=False))
        f.write('```\n')

    print(final[['dataset', 'policy', 'pages_saving_pct', 'recall_delta_vs64', 'miss_rate_pct', 'low07', 'low05']])


if __name__ == '__main__':
    main()
