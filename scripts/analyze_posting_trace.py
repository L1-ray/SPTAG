#!/usr/bin/env python3
"""Analyze per-posting I/O trace to identify bad postings for M2-H selective hybrid.

This script computes per-posting metrics from the posting trace CSV output:
- Total I/O wait contribution per posting
- Total bytes requested per posting
- Average I/O wait per access
- Access frequency (how many queries touch each posting)

The goal is to identify postings that:
1. Contribute disproportionately to total I/O wait (top candidates for hybrid)
2. Have high bytes but low candidates-per-page (locality issues)
3. Are frequently accessed across queries (hot postings)

Usage:
    python scripts/analyze_posting_trace.py --trace results/m2h/posting_trace.csv --output results/m2h/bad_postings.tsv
"""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Analyze posting trace for M2-H bad posting identification.")
    parser.add_argument("--trace", required=True, help="Posting trace CSV from EnablePostingTrace.")
    parser.add_argument("--output", default="", help="Output TSV with per-posting metrics.")
    parser.add_argument("--top-percentile", type=float, default=10.0,
                        help="Top percentile of postings by I/O wait to flag as bad (default 10%%).")
    parser.add_argument("--min-wait-contribution", type=float, default=30.0,
                        help="Minimum %% of total wait that top postings should contribute for M2-H to be viable.")
    return parser.parse_args()


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(math.ceil(pct * len(ordered) / 100.0)) - 1))
    return ordered[index]


def main():
    args = parse_args()

    # Per-posting aggregation
    posting_stats = defaultdict(lambda: {
        "total_io_wait_ms": 0.0,
        "total_requested_bytes": 0,
        "access_count": 0,
        "cache_hit_count": 0,
        "list_page_count": 0,
        "list_ele_count": 0,
    })

    total_queries = 0
    total_io_wait_ms = 0.0
    total_requested_bytes = 0

    # Read trace
    with open(args.trace, newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            query_id = int(row["query_id"])
            posting_id = int(row["posting_id"])
            list_page_count = int(row["list_page_count"])
            list_ele_count = int(row["list_ele_count"])
            requested_bytes = int(row["requested_bytes"])
            io_wait_ms = float(row["io_wait_ms"])
            cache_hit = int(row["cache_hit"])

            posting_key = posting_id
            posting_stats[posting_key]["total_io_wait_ms"] += io_wait_ms
            posting_stats[posting_key]["total_requested_bytes"] += requested_bytes
            posting_stats[posting_key]["access_count"] += 1
            posting_stats[posting_key]["cache_hit_count"] += cache_hit
            posting_stats[posting_key]["list_page_count"] = list_page_count
            posting_stats[posting_key]["list_ele_count"] = list_ele_count

            total_io_wait_ms += io_wait_ms
            total_requested_bytes += requested_bytes

    total_queries = max(1, len(posting_stats))

    if not posting_stats:
        print("No posting records found in trace.")
        return

    # Compute per-posting metrics
    results = []
    for posting_id, stats in posting_stats.items():
        avg_io_wait = stats["total_io_wait_ms"] / stats["access_count"] if stats["access_count"] > 0 else 0.0
        cache_hit_rate = stats["cache_hit_count"] / stats["access_count"] if stats["access_count"] > 0 else 0.0
        wait_contribution = (stats["total_io_wait_ms"] / total_io_wait_ms * 100) if total_io_wait_ms > 0 else 0.0
        bytes_contribution = (stats["total_requested_bytes"] / total_requested_bytes * 100) if total_requested_bytes > 0 else 0.0

        results.append({
            "posting_id": posting_id,
            "list_page_count": stats["list_page_count"],
            "list_ele_count": stats["list_ele_count"],
            "access_count": stats["access_count"],
            "cache_hit_rate": cache_hit_rate,
            "total_io_wait_ms": stats["total_io_wait_ms"],
            "avg_io_wait_ms": avg_io_wait,
            "wait_contribution_pct": wait_contribution,
            "total_requested_bytes": stats["total_requested_bytes"],
            "bytes_contribution_pct": bytes_contribution,
        })

    # Sort by I/O wait contribution (descending)
    results.sort(key=lambda x: x["total_io_wait_ms"], reverse=True)

    # Calculate cumulative wait contribution
    cumulative_wait = 0.0
    for i, r in enumerate(results):
        cumulative_wait += r["total_io_wait_ms"]
        r["cumulative_wait_pct"] = (cumulative_wait / total_io_wait_ms * 100) if total_io_wait_ms > 0 else 0.0

    # Identify top percentile threshold
    top_n = max(1, int(len(results) * args.top_percentile / 100.0))
    top_postings = results[:top_n]
    top_wait_contribution = sum(r["wait_contribution_pct"] for r in top_postings)

    # Summary statistics
    print(f"total_postings\t{len(results)}")
    print(f"total_io_wait_ms\t{total_io_wait_ms:.3f}")
    print(f"total_requested_bytes\t{total_requested_bytes}")
    print(f"top_{args.top_percentile}%_postings_count\t{top_n}")
    print(f"top_{args.top_percentile}%_wait_contribution\t{top_wait_contribution:.2f}%")
    print(f"m2h_viable\t{top_wait_contribution >= args.min_wait_contribution}")

    # Output detailed results
    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", newline="") as handle:
            fieldnames = [
                "posting_id", "list_page_count", "list_ele_count", "access_count",
                "cache_hit_rate", "total_io_wait_ms", "avg_io_wait_ms",
                "wait_contribution_pct", "cumulative_wait_pct",
                "total_requested_bytes", "bytes_contribution_pct",
            ]
            writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
            writer.writeheader()
            writer.writerows(results)

        print(f"detailed_output\t{args.output}")

    # Output top bad postings list (for M2-H allowlist)
    bad_postings_file = ""
    if args.output:
        bad_postings_file = str(Path(args.output).with_suffix(".bad_postings.txt"))
        with open(bad_postings_file, "w") as handle:
            handle.write(f"# Bad postings for M2-H selective hybrid (top {args.top_percentile}% by I/O wait)\n")
            handle.write(f"# Generated from: {args.trace}\n")
            handle.write(f"# Total wait contribution: {top_wait_contribution:.2f}%\n")
            handle.write(f"# Format: posting_id [avg_io_wait_ms]\n")
            for r in top_postings:
                handle.write(f"{r['posting_id']} {r['avg_io_wait_ms']:.3f}\n")

        print(f"bad_postings_list\t{bad_postings_file}")


if __name__ == "__main__":
    main()
