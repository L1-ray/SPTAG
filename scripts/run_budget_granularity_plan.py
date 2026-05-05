#!/usr/bin/env python3
"""
Execute SPANN learned policy budget granularity optimization offline plan.
- Train risk_model_b56 for SIFT1M/SIFT10M (ir=64)
- Export C++ JSON models
- Evaluate A0/A1/A2/A3 on held-out test split
- Output summary CSV/Markdown
"""

import json
import os
from pathlib import Path
from typing import Dict, List, Tuple

import lightgbm as lgb
import numpy as np
import pandas as pd

REPO = Path('/home/ray/code/SPTAG')
OUT_DIR = REPO / 'results' / 'adaptive_budget' / 'budget_granularity_plan_20260505'
OUT_DIR.mkdir(parents=True, exist_ok=True)

DATASETS = {
    'sift1m': {
        'dir': REPO / 'results' / 'adaptive_budget' / 'sift1m_ir64_retrain',
        'global_threshold': 0.95,
        'per_threshold': {32: 0.98, 40: 0.96, 48: 0.93, 56: 0.88},
    },
    'sift10m': {
        'dir': REPO / 'results' / 'adaptive_budget' / 'sift10m_ir64_retrain',
        'global_threshold': 0.85,
        'per_threshold': {32: 0.95, 40: 0.92, 48: 0.88, 56: 0.82},
    },
}

MODEL_BUDGETS = [32, 40, 48, 56]
SWEEP_BUDGETS = [16, 24, 32, 40, 48, 56, 64, 80, 96, 128]


def export_lgbm_txt_to_compact_json(model_txt_path: Path, output_json_path: Path) -> None:
    """Export LightGBM txt model to compact JSON consumed by C++ predictor."""
    text = model_txt_path.read_text()
    lines = text.split('\n')
    feature_names = []
    for line in lines:
        if line.startswith('feature_names='):
            feature_names = line.split('=')[1].split()
            break

    trees = []
    for tree_text in text.split('Tree=')[1:]:
        node = {
            'num_leaves': 0,
            'split_feature': [],
            'threshold': [],
            'left_child': [],
            'right_child': [],
            'leaf_value': [],
        }
        for line in tree_text.strip().split('\n'):
            if line.startswith('num_leaves='):
                node['num_leaves'] = int(line.split('=')[1])
            elif line.startswith('split_feature='):
                node['split_feature'] = [int(x) for x in line.split('=')[1].split()]
            elif line.startswith('threshold='):
                node['threshold'] = [float(x) for x in line.split('=')[1].split()]
            elif line.startswith('left_child='):
                node['left_child'] = [int(x) for x in line.split('=')[1].split()]
            elif line.startswith('right_child='):
                node['right_child'] = [int(x) for x in line.split('=')[1].split()]
            elif line.startswith('leaf_value='):
                node['leaf_value'] = [float(x) for x in line.split('=')[1].split()]
        trees.append(node)

    payload = {
        'feature_names': feature_names,
        'num_features': len(feature_names),
        'num_trees': len(trees),
        'trees': trees,
    }
    output_json_path.write_text(json.dumps(payload, indent=2))


def ensure_test_split(ds_dir: Path) -> Tuple[pd.DataFrame, pd.DataFrame, List[str]]:
    train_path = ds_dir / 'train_data.csv'
    test_path = ds_dir / 'test_data.csv'
    feat_cols_path = ds_dir / 'feature_cols.json'

    if not train_path.exists() or not test_path.exists():
        raise FileNotFoundError(f'missing split files under {ds_dir}')

    train_df = pd.read_csv(train_path)
    test_df = pd.read_csv(test_path)

    if feat_cols_path.exists():
        feature_cols = json.loads(feat_cols_path.read_text())
    else:
        feature_cols = [c for c in train_df.columns if c != 'query_id']

    return train_df, test_df, feature_cols


def load_budget_tables(ds_dir: Path) -> Tuple[Dict[int, Dict[int, float]], Dict[int, Dict[int, float]], List[int]]:
    recall = {}
    pages = {}
    available = []
    for b in SWEEP_BUDGETS:
        p = ds_dir / f'budget_{b}.csv'
        if p.exists():
            df = pd.read_csv(p)
            recall[b] = df.set_index('query_id')['recall'].to_dict()
            pages[b] = df.set_index('query_id')['pages_read'].to_dict()
            available.append(b)
    if 64 not in available:
        raise RuntimeError(f'budget_64.csv missing in {ds_dir}')
    return recall, pages, sorted(available)


def train_models(train_df: pd.DataFrame, feature_cols: List[str], recall: Dict[int, Dict[int, float]], ds_dir: Path) -> Dict[int, lgb.Booster]:
    x_train = train_df[feature_cols]
    models: Dict[int, lgb.Booster] = {}

    for b in MODEL_BUDGETS:
        if b not in recall:
            continue
        y = []
        for _, row in train_df.iterrows():
            qid = int(row['query_id'])
            safe = recall[b].get(qid, 0.0) >= recall[64].get(qid, 0.0) - 0.001
            y.append(1 if safe else 0)

        y = np.array(y)
        safe_rate = float(y.mean())
        if safe_rate < 0.05 or safe_rate > 0.99:
            continue

        dtrain = lgb.Dataset(x_train, label=y)
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
        model = lgb.train(params, dtrain, num_boost_round=200)
        models[b] = model

        txt_path = ds_dir / f'risk_model_b{b}.txt'
        model.save_model(str(txt_path))
        json_path = ds_dir / f'risk_model_b{b}.json'
        export_lgbm_txt_to_compact_json(txt_path, json_path)

    return models


def compute_oracle_min_b(qids: List[int], recall: Dict[int, Dict[int, float]], available_budgets: List[int]) -> Dict[int, int]:
    min_b = {}
    for qid in qids:
        target = recall[64].get(qid, 0.0) - 0.001
        choice = available_budgets[-1]
        for b in available_budgets:
            if recall[b].get(qid, -1.0) >= target:
                choice = b
                break
        min_b[qid] = choice
    return min_b


def assign_budget(
    x: pd.DataFrame,
    qids: List[int],
    models: Dict[int, lgb.Booster],
    eval_budgets: List[int],
    global_threshold: float,
    per_threshold: Dict[int, float] = None,
) -> Dict[int, int]:
    assigned = {}
    for i, qid in enumerate(qids):
        row = x.iloc[i:i+1]
        budget = 64
        for b in eval_budgets:
            if b not in models:
                continue
            p = float(models[b].predict(row)[0])
            t = per_threshold.get(b, global_threshold) if per_threshold else global_threshold
            if p >= t:
                budget = b
                break
        assigned[qid] = budget
    return assigned


def evaluate_policy(
    qids: List[int],
    assigned: Dict[int, int],
    recall: Dict[int, Dict[int, float]],
    pages: Dict[int, Dict[int, float]],
    oracle_min_b: Dict[int, int],
    oracle_saving: float,
    label: str,
) -> Dict[str, float]:
    base_pages = np.mean([pages[64][qid] for qid in qids])
    base_recall = np.mean([recall[64][qid] for qid in qids])

    policy_pages = np.mean([pages[assigned[qid]][qid] for qid in qids])
    policy_recall = np.mean([recall[assigned[qid]][qid] for qid in qids])

    saving = 100.0 * (base_pages - policy_pages) / base_pages
    recall_delta = policy_recall - base_recall

    miss = 0
    low07 = 0
    low05 = 0
    hard64_under = 0

    counts = {16: 0, 24: 0, 32: 0, 40: 0, 48: 0, 56: 0, 64: 0}
    for qid in qids:
        b = assigned[qid]
        counts[b] = counts.get(b, 0) + 1
        r = recall[b][qid]
        if r < recall[64][qid] - 0.001:
            miss += 1
        if r < 0.7:
            low07 += 1
        if r < 0.5:
            low05 += 1
        if oracle_min_b[qid] == 64 and b <= 48:
            hard64_under += 1

    n = len(qids)
    oracle_util = (saving / oracle_saving * 100.0) if oracle_saving > 0 else 0.0

    out = {
        'policy': label,
        'queries': n,
        'pages_saving_pct': saving,
        'recall10_delta_vs_b64': recall_delta,
        'miss_rate_pct': 100.0 * miss / n,
        'low_recall_lt_0_7': low07,
        'low_recall_lt_0_5': low05,
        'hard_oracle64_underbudget': hard64_under,
        'oracle_util_pct': oracle_util,
        'b16_pct': 100.0 * counts.get(16, 0) / n,
        'b24_pct': 100.0 * counts.get(24, 0) / n,
        'b32_pct': 100.0 * counts.get(32, 0) / n,
        'b40_pct': 100.0 * counts.get(40, 0) / n,
        'b48_pct': 100.0 * counts.get(48, 0) / n,
        'b56_pct': 100.0 * counts.get(56, 0) / n,
        'b64_pct': 100.0 * counts.get(64, 0) / n,
    }
    return out


def run_one_dataset(name: str, cfg: Dict) -> pd.DataFrame:
    ds_dir = cfg['dir']
    train_df, test_df, feature_cols = ensure_test_split(ds_dir)
    recall, pages, available_budgets = load_budget_tables(ds_dir)

    models = train_models(train_df, feature_cols, recall, ds_dir)

    qids = [int(x) for x in test_df['query_id'].tolist()]
    x_test = test_df[feature_cols]

    oracle_min_b = compute_oracle_min_b(qids, recall, available_budgets)
    base_pages = np.mean([pages[64][qid] for qid in qids])
    oracle_pages = np.mean([pages[oracle_min_b[qid]][qid] for qid in qids])
    oracle_saving = 100.0 * (base_pages - oracle_pages) / base_pages

    rows = []

    # A0: current baseline learned policy
    a0_assign = assign_budget(
        x_test,
        qids,
        models,
        eval_budgets=[32, 40, 48],
        global_threshold=cfg['global_threshold'],
    )
    rows.append(evaluate_policy(qids, a0_assign, recall, pages, oracle_min_b, oracle_saving, 'A0_current'))

    # A1: add B=56 only (single threshold)
    a1_assign = assign_budget(
        x_test,
        qids,
        models,
        eval_budgets=[32, 40, 48, 56],
        global_threshold=cfg['global_threshold'],
    )
    rows.append(evaluate_policy(qids, a1_assign, recall, pages, oracle_min_b, oracle_saving, 'A1_add56_globalT'))

    # A2: per-budget thresholds with current budgets
    a2_per = {k: v for k, v in cfg['per_threshold'].items() if k in [32, 40, 48]}
    a2_assign = assign_budget(
        x_test,
        qids,
        models,
        eval_budgets=[32, 40, 48],
        global_threshold=cfg['global_threshold'],
        per_threshold=a2_per,
    )
    rows.append(evaluate_policy(qids, a2_assign, recall, pages, oracle_min_b, oracle_saving, 'A2_perT'))

    # A3: add B=56 + per-budget thresholds
    a3_assign = assign_budget(
        x_test,
        qids,
        models,
        eval_budgets=[32, 40, 48, 56],
        global_threshold=cfg['global_threshold'],
        per_threshold=cfg['per_threshold'],
    )
    rows.append(evaluate_policy(qids, a3_assign, recall, pages, oracle_min_b, oracle_saving, 'A3_add56_perT'))

    df = pd.DataFrame(rows)
    df.insert(0, 'dataset', name)

    meta = {
        'dataset': name,
        'test_queries': len(qids),
        'available_budgets': available_budgets,
        'oracle_saving_pct': oracle_saving,
        'global_threshold': cfg['global_threshold'],
        'per_threshold': cfg['per_threshold'],
        'model_budgets_trained': sorted(models.keys()),
    }
    (OUT_DIR / f'{name}_meta.json').write_text(json.dumps(meta, indent=2))

    return df


def main():
    all_rows = []
    for name, cfg in DATASETS.items():
        df = run_one_dataset(name, cfg)
        all_rows.append(df)

    summary = pd.concat(all_rows, ignore_index=True)
    csv_path = OUT_DIR / 'offline_ablation_summary.csv'
    summary.to_csv(csv_path, index=False)

    # Markdown report
    md_path = OUT_DIR / 'offline_ablation_summary.md'
    with md_path.open('w') as f:
        f.write('# Offline Ablation Summary (A0/A1/A2/A3)\n\n')
        f.write(f'Output time: {pd.Timestamp.now()}\n\n')
        f.write('## Core Metrics\n\n')
        cols = [
            'dataset', 'policy', 'pages_saving_pct', 'recall10_delta_vs_b64', 'miss_rate_pct',
            'low_recall_lt_0_7', 'low_recall_lt_0_5', 'hard_oracle64_underbudget', 'oracle_util_pct',
            'b32_pct', 'b40_pct', 'b48_pct', 'b56_pct', 'b64_pct',
        ]
        f.write('```csv\n')
        f.write(summary[cols].to_csv(index=False))
        f.write('```\n')
        f.write('\n\n')

        f.write('## Plan Criteria Check\n\n')
        for ds in summary['dataset'].unique():
            part = summary[summary['dataset'] == ds].set_index('policy')
            a0 = part.loc['A0_current']
            a3 = part.loc['A3_add56_perT']
            save_gain = a3['pages_saving_pct'] - a0['pages_saving_pct']
            qps_proxy = save_gain  # offline proxy only
            f.write(f'### {ds}\n\n')
            f.write(f'- A0 pages saving: {a0["pages_saving_pct"]:.4f}%\n')
            f.write(f'- A3 pages saving: {a3["pages_saving_pct"]:.4f}%\n')
            f.write(f'- Saving gain (A3-A0): {save_gain:.4f} pp\n')
            f.write(f'- A3 recall delta vs B64: {a3["recall10_delta_vs_b64"]:.6f}\n')
            f.write(f'- A3 miss rate: {a3["miss_rate_pct"]:.4f}%\n')
            f.write(f'- A3 oracle utilization: {a3["oracle_util_pct"]:.2f}%\n')
            f.write(f'- Offline QPS proxy (save gain): {qps_proxy:.4f} pp\n\n')

    print(f'Wrote: {csv_path}')
    print(f'Wrote: {md_path}')


if __name__ == '__main__':
    main()
