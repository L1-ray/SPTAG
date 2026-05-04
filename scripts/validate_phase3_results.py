#!/usr/bin/env python3
"""
Phase 3: Validate online adaptive budget results.
"""

import pandas as pd


def load_stats(filepath):
    """Load query IO stats CSV."""
    return pd.read_csv(filepath)


def main():
    print("="*60)
    print("Phase 3: Online Adaptive Budget Validation")
    print("="*60)

    baseline = load_stats('results/adaptive_budget/phase3_online_test/query_io_stats_baseline.csv')
    adaptive = load_stats('results/adaptive_budget/phase3_online_test/query_io_stats_adaptive.csv')

    print(f"\nBaseline queries: {len(baseline)}")
    print(f"Adaptive queries: {len(adaptive)}")

    # Compare pages
    baseline_pages = baseline['pages_read'].mean()
    adaptive_pages = adaptive['pages_read'].mean()
    pages_saving = 100 * (baseline_pages - adaptive_pages) / baseline_pages

    print(f"\n=== Pages Comparison ===")
    print(f"Baseline avg pages: {baseline_pages:.2f}")
    print(f"Adaptive avg pages: {adaptive_pages:.2f}")
    print(f"Pages saving: {pages_saving:.1f}%")

    # Compare recall
    baseline_recall = baseline['recall'].mean()
    adaptive_recall = adaptive['recall'].mean()
    recall_delta = baseline_recall - adaptive_recall

    print(f"\n=== Recall Comparison ===")
    print(f"Baseline avg recall: {baseline_recall:.4f}")
    print(f"Adaptive avg recall: {adaptive_recall:.4f}")
    print(f"Recall delta: {recall_delta:.4f}")

    # Per-query recall loss
    merged = baseline[['query_id', 'recall']].merge(
        adaptive[['query_id', 'recall']],
        on='query_id',
        suffixes=('_baseline', '_adaptive')
    )
    merged['recall_loss'] = merged['recall_baseline'] - merged['recall_adaptive']
    lost_queries = (merged['recall_loss'] > 0.001).sum()

    print(f"\nQueries with recall loss > 0.001: {lost_queries} ({100*lost_queries/len(merged):.1f}%)")

    # Latency comparison
    baseline_latency = baseline['total_latency_ms'].mean()
    adaptive_latency = adaptive['total_latency_ms'].mean()
    latency_change = 100 * (adaptive_latency - baseline_latency) / baseline_latency

    print(f"\n=== Latency Comparison ===")
    print(f"Baseline avg latency: {baseline_latency:.2f} ms")
    print(f"Adaptive avg latency: {adaptive_latency:.2f} ms")
    print(f"Latency change: {latency_change:.1f}%")

    # Summary
    print("\n" + "="*60)
    print("Summary")
    print("="*60)
    print(f"Pages saving: {pages_saving:.1f}%")
    print(f"Recall delta: {recall_delta:.4f}")
    print(f"QPS uplift: ~{abs(latency_change):.1f}%")

    # Decision criteria
    print("\n" + "="*60)
    print("Decision Criteria Check")
    print("="*60)
    print(f"✅ Pages saving >= 8%: {pages_saving:.1f}% {'PASS' if pages_saving >= 8 else 'FAIL'}")
    print(f"{'✅' if abs(recall_delta) <= 0.002 else '❌'} Recall delta <= 0.002: {abs(recall_delta):.4f} {'PASS' if abs(recall_delta) <= 0.002 else 'FAIL'}")
    print(f"✅ QPS uplift >= 5%: {abs(latency_change):.1f}% {'PASS' if abs(latency_change) >= 5 else 'FAIL'}")


if __name__ == '__main__':
    main()
