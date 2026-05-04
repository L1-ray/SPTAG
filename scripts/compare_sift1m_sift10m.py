#!/usr/bin/env python3
"""
Compare adaptive budget results on SIFT1M and SIFT10M.
"""

import pandas as pd


def analyze_results(baseline_path, adaptive_path, dataset_name):
    """Analyze and compare results."""
    baseline = pd.read_csv(baseline_path)
    adaptive = pd.read_csv(adaptive_path)

    # Merge on query_id
    merged = baseline[['query_id', 'recall', 'pages_read', 'total_latency_ms']].merge(
        adaptive[['query_id', 'recall', 'pages_read', 'total_latency_ms']],
        on='query_id',
        suffixes=('_baseline', '_adaptive')
    )

    # Compute deltas
    merged['recall_delta'] = merged['recall_adaptive'] - merged['recall_baseline']
    merged['pages_delta'] = merged['pages_read_adaptive'] - merged['pages_read_baseline']

    # Summary stats
    baseline_pages = baseline['pages_read'].mean()
    adaptive_pages = adaptive['pages_read'].mean()
    pages_saving = 100 * (baseline_pages - adaptive_pages) / baseline_pages

    baseline_recall = baseline['recall'].mean()
    adaptive_recall = adaptive['recall'].mean()
    recall_delta = adaptive_recall - baseline_recall

    # Count queries with recall loss
    lost_queries = (merged['recall_delta'] < -0.001).sum()
    lost_queries_002 = (merged['recall_delta'] < -0.002).sum()

    print(f"\n{'='*60}")
    print(f"{dataset_name} Results")
    print(f"{'='*60}")
    print(f"Queries: {len(merged)}")
    print(f"\nBaseline avg pages: {baseline_pages:.2f}")
    print(f"Adaptive avg pages: {adaptive_pages:.2f}")
    print(f"Pages saving: {pages_saving:.1f}%")
    print(f"\nBaseline avg recall: {baseline_recall:.4f}")
    print(f"Adaptive avg recall: {adaptive_recall:.4f}")
    print(f"Recall delta: {recall_delta:.4f}")
    print(f"\nQueries with recall loss > 0.001: {lost_queries} ({100*lost_queries/len(merged):.1f}%)")
    print(f"Queries with recall loss > 0.002: {lost_queries_002} ({100*lost_queries_002/len(merged):.1f}%)")

    # Recall distribution
    print(f"\nRecall delta distribution:")
    print(f"  min: {merged['recall_delta'].min():.4f}")
    print(f"  5%: {merged['recall_delta'].quantile(0.05):.4f}")
    print(f"  25%: {merged['recall_delta'].quantile(0.25):.4f}")
    print(f"  50%: {merged['recall_delta'].quantile(0.50):.4f}")
    print(f"  75%: {merged['recall_delta'].quantile(0.75):.4f}")
    print(f"  max: {merged['recall_delta'].max():.4f}")

    return {
        'dataset': dataset_name,
        'pages_saving': pages_saving,
        'recall_delta': recall_delta,
        'lost_queries_001': lost_queries,
        'lost_queries_002': lost_queries_002,
    }


def main():
    print("="*60)
    print("SIFT1M vs SIFT10M Adaptive Budget Comparison")
    print("="*60)

    sift1m = analyze_results(
        'results/adaptive_budget/phase3_online_test/query_io_stats_baseline.csv',
        'results/adaptive_budget/phase3_online_test/query_io_stats_adaptive.csv',
        'SIFT1M'
    )

    sift10m = analyze_results(
        'results/adaptive_budget/sift10m_phase3_test/query_io_stats_baseline.csv',
        'results/adaptive_budget/sift10m_phase3_test/query_io_stats_adaptive.csv',
        'SIFT10M'
    )

    print("\n" + "="*60)
    print("Summary Comparison")
    print("="*60)
    print(f"{'Dataset':<12} {'Pages Saving':>12} {'Recall Delta':>12} {'Lost >0.002':>12}")
    print("-"*48)
    print(f"{'SIFT1M':<12} {sift1m['pages_saving']:>11.1f}% {sift1m['recall_delta']:>12.4f} {sift1m['lost_queries_002']:>11} ({100*sift1m['lost_queries_002']/10000:.1f}%)")
    print(f"{'SIFT10M':<12} {sift10m['pages_saving']:>11.1f}% {sift10m['recall_delta']:>12.4f} {sift10m['lost_queries_002']:>11} ({100*sift10m['lost_queries_002']/10000:.1f}%)")

    print("\n" + "="*60)
    print("Decision Criteria Check")
    print("="*60)

    # Check criteria
    for name, data in [('SIFT1M', sift1m), ('SIFT10M', sift10m)]:
        print(f"\n{name}:")
        print(f"  Pages saving >= 8%: {data['pages_saving']:.1f}% {'✅ PASS' if data['pages_saving'] >= 8 else '❌ FAIL'}")
        print(f"  Recall delta >= -0.002: {data['recall_delta']:.4f} {'✅ PASS' if data['recall_delta'] >= -0.002 else '❌ FAIL'}")
        print(f"  Lost queries >0.002 <= 2%: {100*data['lost_queries_002']/10000:.1f}% {'✅ PASS' if data['lost_queries_002']/10000 <= 0.02 else '❌ FAIL'}")


if __name__ == '__main__':
    main()
