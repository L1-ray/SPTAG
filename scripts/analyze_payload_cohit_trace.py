#!/usr/bin/env python3
"""Analyze two-stage payload co-hit traces.

The script computes a conservative oracle lower bound: for each query and
posting, it packs only the payload bytes actually requested by that query into
4KB pages, then sums the per-posting page counts. This is not an implementable
global layout by itself; it is a quick upper-bound test for whether payload
layout work has enough headroom to justify a build-side prototype.
"""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Analyze payload trace page locality and oracle packing headroom.")
    parser.add_argument("--trace", required=True, help="Payload trace CSV from EnablePayloadTrace.")
    parser.add_argument("--query-stats", default="", help="Optional detailed I/O stats CSV for page-count validation.")
    parser.add_argument("--output", default="", help="Optional per-query output TSV.")
    parser.add_argument("--page-size", type=int, default=4096, help="Payload page size in bytes.")
    parser.add_argument("--mismatch-tolerance", type=float, default=0.02, help="Allowed trace-vs-stats mismatch ratio.")
    return parser.parse_args()


def read_query_stats(path):
    if not path:
        return {}

    stats = {}
    with open(path, newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            query_id = int(row["query_id"])
            stats[query_id] = int(float(row["unique_payload_pages"]))
    return stats


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(math.ceil(pct * len(ordered))) - 1))
    return ordered[index]


def main():
    args = parse_args()
    if args.page_size <= 0:
        raise ValueError("--page-size must be positive")

    current_pages = defaultdict(set)
    bytes_by_query_posting = defaultdict(lambda: defaultdict(int))
    candidates_by_query = defaultdict(int)

    with open(args.trace, newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            query_id = int(row["query_id"])
            posting_id = int(row["posting_id"])
            page_id = int(row["payload_page_id"])
            page_count = int(row["payload_page_count"])
            payload_bytes = int(row["payload_bytes"])

            candidates_by_query[query_id] += 1
            bytes_by_query_posting[query_id][posting_id] += payload_bytes
            for page_delta in range(max(1, page_count)):
                current_pages[query_id].add((posting_id, page_id + page_delta))

    query_stats = read_query_stats(args.query_stats)
    rows = []
    for query_id in sorted(candidates_by_query):
        current = len(current_pages[query_id])
        oracle = 0
        for payload_bytes in bytes_by_query_posting[query_id].values():
            oracle += int(math.ceil(payload_bytes / args.page_size))

        candidates = candidates_by_query[query_id]
        reduction = ((current - oracle) / current) if current > 0 else 0.0
        stats_pages = query_stats.get(query_id)
        mismatch_ratio = None
        if stats_pages is not None and stats_pages > 0:
            mismatch_ratio = abs(current - stats_pages) / stats_pages

        rows.append(
            {
                "query_id": query_id,
                "candidates": candidates,
                "current_pages": current,
                "oracle_best_pages": oracle,
                "oracle_page_reduction": reduction,
                "candidates_per_current_page": (candidates / current) if current else 0.0,
                "candidates_per_oracle_page": (candidates / oracle) if oracle else 0.0,
                "stats_unique_payload_pages": stats_pages if stats_pages is not None else "",
                "trace_stats_mismatch_ratio": mismatch_ratio if mismatch_ratio is not None else "",
            }
        )

    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        with output_path.open("w", newline="") as handle:
            fieldnames = [
                "query_id",
                "candidates",
                "current_pages",
                "oracle_best_pages",
                "oracle_page_reduction",
                "candidates_per_current_page",
                "candidates_per_oracle_page",
                "stats_unique_payload_pages",
                "trace_stats_mismatch_ratio",
            ]
            writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)

    query_count = len(rows)
    if query_count == 0:
        print("queries\t0")
        return

    sums = defaultdict(float)
    for row in rows:
        for key in [
            "candidates",
            "current_pages",
            "oracle_best_pages",
            "oracle_page_reduction",
            "candidates_per_current_page",
            "candidates_per_oracle_page",
        ]:
            sums[key] += float(row[key])

    mismatches = [
        float(row["trace_stats_mismatch_ratio"])
        for row in rows
        if row["trace_stats_mismatch_ratio"] != ""
        and float(row["trace_stats_mismatch_ratio"]) > args.mismatch_tolerance
    ]

    print(f"queries\t{query_count}")
    for key in [
        "candidates",
        "current_pages",
        "oracle_best_pages",
        "oracle_page_reduction",
        "candidates_per_current_page",
        "candidates_per_oracle_page",
    ]:
        print(f"avg_{key}\t{sums[key] / query_count:.6f}")
    print(f"p50_oracle_page_reduction\t{percentile([row['oracle_page_reduction'] for row in rows], 0.50):.6f}")
    print(f"p95_oracle_page_reduction\t{percentile([row['oracle_page_reduction'] for row in rows], 0.95):.6f}")
    print(f"trace_stats_mismatch_queries\t{len(mismatches)}")


if __name__ == "__main__":
    main()
