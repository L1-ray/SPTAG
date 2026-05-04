#!/usr/bin/env python3
"""
Phase 2: Develop and evaluate rule-based budget policy.
"""

import pandas as pd
import numpy as np


def load_data():
    """Load features and budget sweep data."""
    features = pd.read_csv('results/adaptive_budget/phase2_feature_extraction/query_features.csv')

    # Load budget sweep stats
    budgets = [16, 32, 40, 48, 64, 80, 96, 128]
    budget_stats = {}
    for b in budgets:
        df = pd.read_csv(f'results/adaptive_budget/budget_sweep/query_io_stats_b{b}.csv')
        budget_stats[b] = df.set_index('query_id')['recall'].to_dict()

    # Load baseline (B=64) stats for pages
    baseline = pd.read_csv('results/adaptive_budget/budget_sweep/query_io_stats_b64.csv')
    baseline = baseline.set_index('query_id')

    return features, budget_stats, baseline, budgets


def compute_min_b(features, budget_stats, budgets):
    """Compute min_B for each query."""
    # Use B=64 as baseline
    baseline_recall = budget_stats[64]

    min_b = {}
    for qid in baseline_recall:
        target = baseline_recall[qid] - 0.001
        for b in budgets:
            if qid in budget_stats.get(b, {}):
                if budget_stats[b][qid] >= target:
                    min_b[qid] = b
                    break
        if qid not in min_b:
            min_b[qid] = 129

    features['min_B'] = features['query_id'].map(min_b)
    return features


def evaluate_rule(features, budget_stats, budgets, rule_func, rule_name):
    """Evaluate a budget rule."""
    decisions = []

    for _, row in features.iterrows():
        qid = row['query_id']
        budget = rule_func(row)
        decisions.append({'query_id': qid, 'budget': budget})

    decisions_df = pd.DataFrame(decisions)

    # Load pages data for all budgets
    pages_data = {}
    for b in budgets:
        try:
            df = pd.read_csv(f'results/adaptive_budget/budget_sweep/query_io_stats_b{b}.csv')
            pages_data[b] = df.set_index('query_id')['pages_read'].to_dict()
        except:
            pages_data[b] = {}

    # Compute recall and pages for each query
    results = []
    for _, row in decisions_df.iterrows():
        qid = row['query_id']
        budget = int(row['budget'])

        # Get actual recall at this budget
        actual_recall = budget_stats.get(budget, {}).get(qid, 0)
        target_recall = budget_stats.get(64, {}).get(qid, 0)

        # Get pages at this budget
        pages = pages_data.get(budget, {}).get(qid, 0)

        results.append({
            'query_id': qid,
            'budget': budget,
            'actual_recall': actual_recall,
            'target_recall': target_recall,
            'pages': pages,
            'meets_target': actual_recall >= target_recall - 0.001
        })

    results_df = pd.DataFrame(results)

    # Summary stats
    total_queries = len(results_df)
    meet_target = results_df['meets_target'].sum()
    avg_pages = results_df['pages'].mean()

    # Baseline (B=64) pages
    baseline_pages_df = pd.read_csv('results/adaptive_budget/budget_sweep/query_io_stats_b64.csv')
    baseline_avg_pages = baseline_pages_df['pages_read'].mean()

    pages_saving = 100 * (baseline_avg_pages - avg_pages) / baseline_avg_pages
    recall_miss_rate = 100 * (total_queries - meet_target) / total_queries

    print(f"\n{rule_name}:")
    print(f"  Avg pages/query: {avg_pages:.1f} (baseline: {baseline_avg_pages:.1f})")
    print(f"  Pages saving: {pages_saving:.1f}%")
    print(f"  Recall miss rate: {recall_miss_rate:.2f}% ({total_queries - meet_target}/{total_queries})")

    # Budget distribution
    budget_dist = results_df['budget'].value_counts().sort_index()
    print(f"  Budget distribution:")
    for b, count in budget_dist.items():
        pct = 100 * count / total_queries
        print(f"    B={b}: {count} ({pct:.1f}%)")

    return results_df, avg_pages, pages_saving, recall_miss_rate


def rule_fixed_budget(row, budget=64):
    """Fixed budget rule."""
    return budget


def rule_margin_16_threshold(row, easy_budget=48, hard_budget=64, threshold=0.30):
    """Rule based on margin_16 threshold."""
    margin = row.get('margin_16', np.nan)
    if pd.isna(margin):
        return hard_budget
    return easy_budget if margin >= threshold else hard_budget


def rule_margin_16_3tier(row, easy_b=32, medium_b=48, hard_b=64,
                         easy_t=0.40, medium_t=0.25):
    """Three-tier rule based on margin_16."""
    margin = row.get('margin_16', np.nan)
    if pd.isna(margin):
        return hard_b
    if margin >= easy_t:
        return easy_b
    elif margin >= medium_t:
        return medium_b
    else:
        return hard_b


def rule_margin_16_multi(row):
    """Multi-tier rule based on margin_16."""
    margin = row.get('margin_16', np.nan)
    if pd.isna(margin):
        return 80

    if margin >= 0.50:
        return 32
    elif margin >= 0.35:
        return 40
    elif margin >= 0.25:
        return 48
    elif margin >= 0.15:
        return 64
    else:
        return 80


def rule_combined_features(row):
    """Rule combining multiple features."""
    margin_16 = row.get('margin_16', np.nan)
    margin_32 = row.get('margin_32', np.nan)

    if pd.isna(margin_16):
        return 80

    # Use both margin_16 and margin_32
    if margin_16 >= 0.40 and (pd.isna(margin_32) or margin_32 >= 0.50):
        return 32
    elif margin_16 >= 0.30:
        return 40
    elif margin_16 >= 0.20:
        return 48
    elif margin_16 >= 0.12:
        return 64
    else:
        return 80


def main():
    print("="*60)
    print("Phase 2: Rule-based Budget Policy Evaluation")
    print("="*60)

    features, budget_stats, baseline, budgets = load_data()
    features = compute_min_b(features, budget_stats, budgets)

    print(f"\nLoaded {len(features)} queries")

    # Evaluate different rules
    rules = [
        ("Fixed B=64", lambda r: 64),
        ("Fixed B=48", lambda r: 48),
        ("margin_16 >= 0.30 -> B=48, else B=64", lambda r: rule_margin_16_threshold(r, 48, 64, 0.30)),
        ("margin_16 >= 0.35 -> B=48, else B=64", lambda r: rule_margin_16_threshold(r, 48, 64, 0.35)),
        ("margin_16 >= 0.30 -> B=40, else B=64", lambda r: rule_margin_16_threshold(r, 40, 64, 0.30)),
        ("3-tier: m16>=0.40->32, >=0.25->48, else 64", lambda r: rule_margin_16_3tier(r)),
        ("Multi-tier (32/40/48/64/80)", rule_margin_16_multi),
        ("Combined features", rule_combined_features),
    ]

    results = []
    for name, rule in rules:
        result = evaluate_rule(features, budget_stats, budgets, rule, name)
        results.append((name, result[1], result[2], result[3]))

    print("\n" + "="*60)
    print("Summary")
    print("="*60)
    print(f"{'Rule':<50} {'Pages':>8} {'Saving':>8} {'Miss%':>8}")
    print("-"*74)
    for name, avg_pages, saving, miss_rate in results:
        print(f"{name:<50} {avg_pages:>8.1f} {saving:>7.1f}% {miss_rate:>7.2f}%")


if __name__ == '__main__':
    main()
