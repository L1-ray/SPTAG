#!/usr/bin/env python3
"""Evaluate a payload order file against payload traces to estimate pages/KB.

This script simulates the build-side page packing for a given order file
and evaluates the expected unique pages for different TopRGlobal levels.

Usage:
    python3 scripts/evaluate_payload_order.py \
        --order /path/to/cohit_order.tsv \
        --trace-256 /path/to/payload_trace_256.csv \
        --trace-512 /path/to/payload_trace_512.csv \
        --trace-768 /path/to/payload_trace_768.csv \
        --output /path/to/evaluation_results.tsv \
        --page-size 4096
"""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Evaluate payload order against traces.")
    parser.add_argument("--order", required=True, help="Order TSV: posting_id, vector_id, order_rank, frequency.")
    parser.add_argument("--trace-256", default="", help="Payload trace for TopRGlobal=256.")
    parser.add_argument("--trace-512", default="", help="Payload trace for TopRGlobal=512.")
    parser.add_argument("--trace-768", default="", help="Payload trace for TopRGlobal=768.")
    parser.add_argument("--output", default="", help="Output TSV with evaluation results.")
    parser.add_argument("--page-size", type=int, default=4096, help="Payload page size in bytes.")
    parser.add_argument("--payload-bytes", type=int, default=128, help="Payload bytes per vector (default 128 for UInt8 128-dim).")
    return parser.parse_args()


def load_order(order_path):
    """Load order file: posting_id -> list of (vector_id, rank) in order."""
    order = defaultdict(list)
    with open(order_path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            posting_id = int(row["posting_id"])
            vector_id = int(row["vector_id"])
            rank = int(row["order_rank"])
            order[posting_id].append((vector_id, rank))

    # Sort by rank within each posting
    for posting_id in order:
        order[posting_id].sort(key=lambda x: x[1])

    return order


def load_trace(trace_path):
    """Load payload trace: query_id -> list of (posting_id, vector_id, payload_bytes)."""
    if not trace_path:
        return {}

    trace = defaultdict(list)
    with open(trace_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            query_id = int(row["query_id"])
            posting_id = int(row["posting_id"])
            vector_id = int(row["vector_id"])
            payload_bytes = int(row.get("payload_bytes", 0) or 0)
            trace[query_id].append((posting_id, vector_id, payload_bytes))

    return trace


def compute_vector_page_mapping(order, page_size, payload_bytes_per_vector):
    """
    Compute vector_id -> (posting_id, page_index) mapping based on order.

    This simulates the build-side page packing:
    - Vectors are written in order within each posting
    - Page boundaries are at page_size intervals
    - Includes posting header and directory overhead
    """
    vector_to_page = {}
    posting_page_counts = {}

    for posting_id, vectors in order.items():
        # Each posting has header + directory overhead
        # Header: ~24 bytes, Directory entry: ~24 bytes per chunk
        # For single-chunk: ~48 bytes overhead
        header_overhead = 48
        current_page = 0
        current_offset = header_overhead

        for vector_id, _ in vectors:
            # Record current page for this vector
            vector_to_page[(posting_id, vector_id)] = (posting_id, current_page)

            # Advance offset (use provided payload bytes as default)
            current_offset += payload_bytes_per_vector

            # Check if we need to start a new page
            if current_offset >= page_size:
                current_page += 1
                current_offset = payload_bytes_per_vector  # Start fresh on new page

        posting_page_counts[posting_id] = current_page + 1

    return vector_to_page, posting_page_counts


def evaluate_trace(trace, vector_to_page):
    """
    Evaluate a trace against the vector->page mapping.

    Returns:
        - avg_unique_pages: Average unique pages per query
        - avg_payload_kb: Average payload KB per query
        - avg_candidates_per_page: Average candidates per page
    """
    if not trace:
        return None, None, None

    total_pages = 0
    total_kb = 0
    total_candidates = 0
    query_count = len(trace)

    for query_id, candidates in trace.items():
        unique_pages = set()
        total_bytes = 0

        for posting_id, vector_id, payload_bytes in candidates:
            key = (posting_id, vector_id)
            if key in vector_to_page:
                page_key = vector_to_page[key]
                unique_pages.add(page_key)
            total_bytes += payload_bytes if payload_bytes > 0 else 128

        total_pages += len(unique_pages)
        total_kb += total_bytes / 1024
        total_candidates += len(candidates)

    avg_pages = total_pages / query_count if query_count > 0 else 0
    avg_kb = total_kb / query_count if query_count > 0 else 0
    avg_cand_per_page = total_candidates / total_pages if total_pages > 0 else 0

    return avg_pages, avg_kb, avg_cand_per_page


def main():
    args = parse_args()

    print(f"Loading order from {args.order}...")
    order = load_order(args.order)
    total_postings = len(order)
    total_vectors = sum(len(v) for v in order.values())
    print(f"  Loaded {total_postings} postings, {total_vectors} vectors")

    print(f"Computing vector->page mapping (page_size={args.page_size}, payload_bytes={args.payload_bytes})...")
    vector_to_page, posting_page_counts = compute_vector_page_mapping(order, args.page_size, args.payload_bytes)
    print(f"  Mapped {len(vector_to_page)} (posting, vector) pairs to pages")
    print(f"  Total posting pages: {sum(posting_page_counts.values())}")

    results = []

    for topr, trace_path in [("256", args.trace_256), ("512", args.trace_512), ("768", args.trace_768)]:
        if not trace_path:
            continue

        print(f"Evaluating TopRGlobal={topr} from {trace_path}...")
        trace = load_trace(trace_path)

        if trace:
            avg_pages, avg_kb, avg_cand_per_page = evaluate_trace(trace, vector_to_page)
            results.append({
                "topr_global": topr,
                "queries": len(trace),
                "avg_unique_pages": avg_pages,
                "avg_payload_kb": avg_kb,
                "avg_candidates_per_page": avg_cand_per_page,
            })
            print(f"  Queries: {len(trace)}, Pages: {avg_pages:.2f}, KB: {avg_kb:.2f}, Cand/Page: {avg_cand_per_page:.2f}")

    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)

        with output_path.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["topr_global", "queries", "avg_unique_pages", "avg_payload_kb", "avg_candidates_per_page"], delimiter="\t")
            writer.writeheader()
            writer.writerows(results)

        print(f"Results saved to {args.output}")

    # Print summary table
    print("\n" + "=" * 80)
    print("Evaluation Summary")
    print("=" * 80)
    print(f"{'TopR':<8} {'Queries':>10} {'Pages':>12} {'KB':>12} {'Cand/Page':>12}")
    print("-" * 80)
    for r in results:
        print(f"{r['topr_global']:<8} {r['queries']:>10} {r['avg_unique_pages']:>12.2f} {r['avg_payload_kb']:>12.2f} {r['avg_candidates_per_page']:>12.2f}")


if __name__ == "__main__":
    main()
