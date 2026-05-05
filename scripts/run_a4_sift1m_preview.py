#!/usr/bin/env python3
"""A4 controlled offline preview on SIFT1M only (small-flow test split)."""

import json
from pathlib import Path
from typing import Dict, List

import lightgbm as lgb
import numpy as np
import pandas as pd

BASE = Path('/home/ray/code/SPTAG')
DS_DIR = BASE / 'results' / 'adaptive_budget' / 'sift1m_ir64_retrain'
OUT_DIR = BASE / 'results' / 'adaptive_budget' / 'budget_granularity_plan_20260505' / 'a4_preview_sift1m'
OUT_DIR.mkdir(parents=True, exist_ok=True)

MODEL_BUDGETS = [16, 24, 32, 40, 48, 56]
ALL_BUDGETS = [16, 24, 32, 40, 48, 56, 64, 80, 96, 128]

THRESHOLDS = {
    'A3_ref': {32: 0.98, 40: 0.96, 48: 0.93, 56: 0.88},
    'A4_cons_v1': {16: 0.998, 24: 0.995, 32: 0.985, 40: 0.970, 48: 0.945, 56: 0.900},
    'A4_cons_v2': {16: 0.997, 24: 0.993, 32: 0.982, 40: 0.965, 48: 0.938, 56: 0.895},
    'A4_cons_v3': {16: 0.996, 24: 0.990, 32: 0.980, 40: 0.960, 48: 0.930, 56: 0.890},
}


def export_lgbm_txt_to_compact_json(model_txt_path: Path, output_json_path: Path) -> None:
    text = model_txt_path.read_text()
    lines = text.split('\n')
    feature_names = []
    for line in lines:
        if line.startswith('feature_names='):
            feature_names = line.split('=')[1].split()
            break

    trees = []
    for tree_text in text.split('Tree=')[1:]:
        t = {
            'num_leaves': 0,
            'split_feature': [],
            'threshold': [],
            'left_child': [],
            'right_child': [],
            'leaf_value': [],
        }
        for line in tree_text.strip().split('\n'):
            if line.startswith('num_leaves='):
                t['num_leaves'] = int(line.split('=')[1])
            elif line.startswith('split_feature='):
                t['split_feature'] = [int(x) for x in line.split('=')[1].split()]
            elif line.startswith('threshold='):
                t['threshold'] = [float(x) for x in line.split('=')[1].split()]
            elif line.startswith('left_child='):
                t['left_child'] = [int(x) for x in line.split('=')[1].split()]
            elif line.startswith('right_child='):
                t['right_child'] = [int(x) for x in line.split('=')[1].split()]
            elif line.startswith('leaf_value='):
                t['leaf_value'] = [float(x) for x in line.split('=')[1].split()]
        trees.append(t)

    payload = {
        'feature_names': feature_names,
        'num_features': len(feature_names),
        'num_trees': len(trees),
        'trees': trees,
    }
    output_json_path.write_text(json.dumps(payload, indent=2))


def load_data():
    train_df = pd.read_csv(DS_DIR / 'train_data.csv')
    test_df = pd.read_csv(DS_DIR / 'test_data.csv')
    feature_cols = json.loads((DS_DIR / 'feature_cols.json').read_text())

    recall = {}
    pages = {}
    for b in ALL_BUDGETS:
        p = DS_DIR / f'budget_{b}.csv'
        if p.exists():
            df = pd.read_csv(p)
            recall[b] = df.set_index('query_id')['recall'].to_dict()
            pages[b] = df.set_index('query_id')['pages_read'].to_dict()
    return train_df, test_df, feature_cols, recall, pages


def train_models(train_df, feature_cols, recall):
    x = train_df[feature_cols]
    models = {}
    rows = []
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
        rows.append({'budget': b, 'safe_rate_train': safe_rate})
        if safe_rate < 0.02 or safe_rate > 0.995:
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
        m = lgb.train(params, dtrain, num_boost_round=200)
        models[b] = m

        txt = DS_DIR / f'risk_model_b{b}.txt'
        js = DS_DIR / f'risk_model_b{b}.json'
        m.save_model(str(txt))
        export_lgbm_txt_to_compact_json(txt, js)

    return models, pd.DataFrame(rows)


def oracle_min_b(qids: List[int], recall: Dict[int, Dict[int, float]]) -> Dict[int, int]:
    available = [b for b in ALL_BUDGETS if b in recall]
    available = sorted(available)
    out = {}
    for q in qids:
        target = recall[64][q] - 0.001
        picked = 64
        for b in available:
            if recall[b][q] >= target:
                picked = b
                break
        out[q] = picked
    return out


def assign_for_policy(models, x, qids, th):
    budgets = sorted(th.keys())
    assigned = {}
    safety = {}
    for i, q in enumerate(qids):
        row = x.iloc[i:i+1]
        chosen = 64
        chosen_prob = None
        for b in budgets:
            if b not in models:
                continue
            p = float(models[b].predict(row)[0])
            if p >= th[b]:
                chosen = b
                chosen_prob = p
                break
        assigned[q] = chosen
        safety[q] = chosen_prob
    return assigned, safety


def eval_policy(name, assigned, recall, pages, qids, oracle, a3_ref_row=None):
    base_pages = np.mean([pages[64][q] for q in qids])
    base_recall = np.mean([recall[64][q] for q in qids])

    cur_pages = np.mean([pages[assigned[q]][q] for q in qids])
    cur_recall = np.mean([recall[assigned[q]][q] for q in qids])

    counts = {b: 0 for b in [16, 24, 32, 40, 48, 56, 64]}
    miss = 0
    low07 = 0
    low05 = 0
    low_from_16_24 = 0
    miss_from_16_24 = 0

    for q in qids:
        b = assigned[q]
        counts[b] = counts.get(b, 0) + 1
        r = recall[b][q]
        if r < recall[64][q] - 0.001:
            miss += 1
            if b <= 24:
                miss_from_16_24 += 1
        if r < 0.7:
            low07 += 1
            if b <= 24:
                low_from_16_24 += 1
        if r < 0.5:
            low05 += 1

    n = len(qids)
    row = {
        'policy': name,
        'queries': n,
        'pages_saving_pct_vs64': 100.0 * (base_pages - cur_pages) / base_pages,
        'recall10_delta_vs64': cur_recall - base_recall,
        'miss_rate_pct': 100.0 * miss / n,
        'low_recall_lt_0_7': low07,
        'low_recall_lt_0_5': low05,
        'b16_pct': 100.0 * counts[16] / n,
        'b24_pct': 100.0 * counts[24] / n,
        'b32_pct': 100.0 * counts[32] / n,
        'b40_pct': 100.0 * counts[40] / n,
        'b48_pct': 100.0 * counts[48] / n,
        'b56_pct': 100.0 * counts[56] / n,
        'b64_pct': 100.0 * counts[64] / n,
        'miss_from_b16_24': miss_from_16_24,
        'low07_from_b16_24': low_from_16_24,
    }

    if a3_ref_row is not None:
        row['delta_low07_vs_A3'] = int(row['low_recall_lt_0_7'] - a3_ref_row['low_recall_lt_0_7'])
        row['delta_low05_vs_A3'] = int(row['low_recall_lt_0_5'] - a3_ref_row['low_recall_lt_0_5'])

    # A4 pre-check criteria from plan
    easy_prop = 100.0 * sum(1 for q in qids if oracle[q] <= 24) / n
    row['oracle_easy_le24_pct'] = easy_prop
    row['b16_b24_pct'] = row['b16_pct'] + row['b24_pct']
    row['pass_recall_delta_le_0p0015'] = abs(row['recall10_delta_vs64']) <= 0.0015
    row['pass_low_tail_not_worse_vs_A3'] = (row.get('delta_low07_vs_A3', 0) <= 0 and row.get('delta_low05_vs_A3', 0) <= 0)
    row['pass_low_budget_not_over_oracle_easy'] = row['b16_b24_pct'] <= easy_prop

    return row


def main():
    train_df, test_df, feature_cols, recall, pages = load_data()
    models, safe_df = train_models(train_df, feature_cols, recall)

    qids = [int(x) for x in test_df['query_id'].tolist()]
    x = test_df[feature_cols]
    oracle = oracle_min_b(qids, recall)

    rows = []

    # A3 reference first
    a3_assigned, _ = assign_for_policy(models, x, qids, THRESHOLDS['A3_ref'])
    a3_row = eval_policy('A3_ref', a3_assigned, recall, pages, qids, oracle)
    rows.append(a3_row)

    for name in ['A4_cons_v1', 'A4_cons_v2', 'A4_cons_v3']:
        assigned, _ = assign_for_policy(models, x, qids, THRESHOLDS[name])
        rows.append(eval_policy(name, assigned, recall, pages, qids, oracle, a3_ref_row=a3_row))

    out = pd.DataFrame(rows)
    out.to_csv(OUT_DIR / 'a4_preview_summary.csv', index=False)
    safe_df.to_csv(OUT_DIR / 'a4_model_safe_rates.csv', index=False)

    meta = {
        'dataset': 'sift1m',
        'flow': 'offline_test_split_only',
        'test_queries': len(qids),
        'thresholds': THRESHOLDS,
        'trained_model_budgets': sorted(models.keys()),
    }
    (OUT_DIR / 'a4_preview_meta.json').write_text(json.dumps(meta, indent=2))

    with (OUT_DIR / 'a4_preview_report.md').open('w') as f:
        f.write('# A4 Controlled Offline Preview (SIFT1M)\n\n')
        f.write('Date: 2026-05-05\n\n')
        f.write('## Model Safe Rates (Train)\n\n```csv\n')
        f.write(safe_df.to_csv(index=False))
        f.write('```\n\n')
        f.write('## Policy Results\n\n```csv\n')
        f.write(out.to_csv(index=False))
        f.write('```\n')

    print(out[['policy', 'pages_saving_pct_vs64', 'recall10_delta_vs64', 'miss_rate_pct', 'low_recall_lt_0_7', 'b16_pct', 'b24_pct', 'b64_pct', 'pass_recall_delta_le_0p0015', 'pass_low_tail_not_worse_vs_A3', 'pass_low_budget_not_over_oracle_easy']])


if __name__ == '__main__':
    main()
