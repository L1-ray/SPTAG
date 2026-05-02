#!/usr/bin/env python3
"""Evaluate a payload order file against payload traces to estimate pages.

This script simulates the build-side page packing for a given order file
and evaluates the expected unique pages for different TopRGlobal levels.

Key insight:
- The trace contains (posting_id, vector_id, payload_page_id) from the BASELINE index
- The order file defines a NEW ordering of vectors within each posting
- After rebuild, the same vectors will be on DIFFERENT pages
- We simulate the new page assignment and compute unique pages per query

Usage:
    python3 scripts/evaluate_payload_order_v2.py \
        --order /path/to/cohit_order.tsv \
        --trace-256 /path/to/payload_trace_256.csv \
        --trace-512 /path/to/payload_trace_512.csv \
        --trace-768 /path/to/payload_trace_768.csv \
        --output /path/to/evaluation_results.tsv \
        --page-size 4096
"""

import argparse
import csv
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
    return parser.parse_args()


def load_order(order_path):
    """Load order file: posting_id -> {vector_id -> rank}."""
    order = defaultdict(dict)
    with open(order_path, newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            posting_id = int(row["posting_id"])
            vector_id = int(row["vector_id"])
            rank = int(row["order_rank"])
            order[posting_id][vector_id] = rank
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
            payload_bytes = int(row.get("payload_bytes", 0) or 128)
            trace[query_id].append((posting_id, vector_id, payload_bytes))
    return trace


def compute_new_page_mapping(order, trace, page_size):
    """
    Compute (posting_id, vector_id) -> new_page_id after rebuild with order.

    This simulates the build-side page packing:
    1. For each posting, vectors are written in the order defined by order file
    2. Each posting starts at a new page (for simplicity)
    3. Pages are filled sequentially within each posting

    Returns:
        vector_to_page: (posting_id, vector_id) -> (posting_id, page_index_within_posting)
    """
    # First, collect all vectors that appear in trace for each posting
    vectors_in_trace = defaultdict(set)
    all_traces = trace  # This is already the combined trace
    for query_id, candidates in all_traces.items():
        for posting_id, vector_id, _ in candidates:
            vectors_in_trace[posting_id].add(vector_id)

    # For each posting, assign page IDs based on order
    vector_to_page = {}
    global_page_id = 0  # Global page counter

    for posting_id, vector_order in order.items():
        # Sort vectors by rank
        sorted_vectors = sorted(vector_order.items(), key=lambda x: x[1])

        current_page_offset = 0
        posting_start_page = global_page_id

        for vector_id, rank in sorted_vectors:
            # Use 128 bytes per vector (UInt8 128-dim)
            payload_bytes = 128

            # Assign current global page to this vector
            if vector_id in vectors_in_trace[posting_id]:
                vector_to_page[(posting_id, vector_id)] = (posting_id, global_page_id)

            # Advance offset
            current_page_offset += payload_bytes

            # Check if we need a new page
            if current_page_offset >= page_size:
                global_page_id += 1
                current_page_offset = 0

        # Move to next posting's start page
        if current_page_offset > 0:
            global_page_id += 1

    return vector_to_page


def evaluate_trace(trace, vector_to_page):
    """Evaluate a trace against the new page mapping."""
    if not trace:
        return None, None, None

    total_pages = 0
    total_kb = 0
    total_candidates = 0
    query_count = len(trace)

    for query_id, candidates in trace.items():
        unique_pages = set()
        query_kb = 0

        for posting_id, vector_id, payload_bytes in candidates:
            key = (posting_id, vector_id)
            if key in vector_to_page:
                page_key = vector_to_page[key]
                unique_pages.add(page_key[1])  # Use global page ID
            query_kb += payload_bytes if payload_bytes > 0 else 128

        total_pages += len(unique_pages)
        total_kb += query_kb / 1024
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
    print(f"  Loaded {total_postings} postings, {total_vectors} vector orderings")

    # Load all traces first to know which vectors we need
    all_traces = {}
    for topr, trace_path in [("256", args.trace_256), ("512", args.trace_512), ("768", args.trace_768)]:
        if trace_path:
            trace = load_trace(trace_path)
            all_traces.update(trace)  # Merge all traces

    print(f"  Total queries across all traces: {len(all_traces)}")

    print(f"Computing new page mapping (page_size={args.page_size})...")
    vector_to_page = compute_new_page_mapping(order, all_traces, args.page_size)
    print(f"  Mapped {len(vector_to_page)} (posting, vector) pairs to pages")

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
