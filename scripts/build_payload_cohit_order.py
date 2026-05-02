#!/usr/bin/env python3
"""Build a deterministic co-hit payload order from payload trace CSV."""

import argparse
import csv
from collections import Counter, defaultdict
from itertools import combinations
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Generate PostingCohitOrderFile from payload trace CSV.")
    parser.add_argument("--trace", required=True, help="Payload trace CSV from EnablePayloadTrace.")
    parser.add_argument("--output", required=True, help="Output TSV: posting_id, vector_id, order_rank, frequency.")
    parser.add_argument(
        "--strategy",
        choices=["adjacent", "page-aware", "query-set"],
        default="adjacent",
        help="Ordering strategy. adjacent=v1 pair co-hit; page-aware=v2 page-fill greedy; query-set=v3 maximize query coverage per page.",
    )
    parser.add_argument("--page-size", type=int, default=4096, help="Target payload page size for page-aware/query-set packing.")
    parser.add_argument(
        "--max-group-size",
        type=int,
        default=128,
        help="Skip pair edges for unusually large per-query/per-posting groups.",
    )
    parser.add_argument(
        "--topr-weights",
        type=str,
        default="",
        help="Comma-separated TopRGlobal weights for multi-trace input, e.g., '256:1.0,512:1.0,768:1.5' to emphasize 768.",
    )
    return parser.parse_args()


def choose_next(remaining, freq, adjacency, previous):
    if previous is None:
        return max(remaining, key=lambda vid: (freq[vid], -vid))

    previous_edges = adjacency.get(previous, {})
    best = max(remaining, key=lambda vid: (previous_edges.get(vid, 0), freq[vid], -vid))
    if previous_edges.get(best, 0) == 0:
        return max(remaining, key=lambda vid: (freq[vid], -vid))
    return best


def build_adjacent_order(query_vectors, freq, max_group_size):
    adjacency = defaultdict(Counter)
    for vector_set in query_vectors.values():
        if len(vector_set) < 2 or len(vector_set) > max_group_size:
            continue
        for lhs, rhs in combinations(sorted(vector_set), 2):
            adjacency[lhs][rhs] += 1
            adjacency[rhs][lhs] += 1

    remaining = set(freq.keys())
    order = []
    previous = None
    while remaining:
        selected = choose_next(remaining, freq, adjacency, previous)
        remaining.remove(selected)
        order.append(selected)
        previous = selected
    return order


def build_page_aware_order(query_vectors, freq, payload_bytes, page_size, max_group_size):
    if page_size <= 0:
        raise ValueError("--page-size must be positive")

    query_ids_by_vector = defaultdict(set)
    usable_query_vectors = {}
    for query_id, vector_set in query_vectors.items():
        if not vector_set or len(vector_set) > max_group_size:
            continue
        usable_query_vectors[query_id] = vector_set
        for vector_id in vector_set:
            query_ids_by_vector[vector_id].add(query_id)

    default_payload_bytes = max(1, min(payload_bytes.values())) if payload_bytes else 1
    remaining = set(freq.keys())
    order = []

    while remaining:
        page = []
        page_bytes = 0
        candidate_scores = Counter()

        while remaining:
            fit_candidates = [
                vector_id
                for vector_id in remaining
                if page_bytes + max(1, payload_bytes.get(vector_id, default_payload_bytes)) <= page_size
            ]
            if not fit_candidates:
                break

            if not page:
                selected = max(fit_candidates, key=lambda vid: (freq[vid], -payload_bytes.get(vid, default_payload_bytes), -vid))
            else:
                selected = max(
                    fit_candidates,
                    key=lambda vid: (
                        candidate_scores.get(vid, 0),
                        freq[vid],
                        -payload_bytes.get(vid, default_payload_bytes),
                        -vid,
                    ),
                )

            remaining.remove(selected)
            page.append(selected)
            page_bytes += max(1, payload_bytes.get(selected, default_payload_bytes))

            for query_id in query_ids_by_vector.get(selected, ()):
                for vector_id in usable_query_vectors.get(query_id, ()):
                    if vector_id in remaining:
                        candidate_scores[vector_id] += 1

        if not page:
            selected = max(remaining, key=lambda vid: (freq[vid], -payload_bytes.get(vid, default_payload_bytes), -vid))
            remaining.remove(selected)
            page.append(selected)

        order.extend(page)

    return order


def build_query_set_order(query_vectors, freq, payload_bytes, page_size, max_group_size, topr_weights=None, query_weights_map=None):
    """
    P4c query-set/page-objective strategy.

    Objective: Maximize the number of queries whose TopR candidates are fully covered by each page.
    Unlike page-aware which scores by adjacency, this directly optimizes query-set coverage.

    Algorithm:
    1. For each page, greedily select vectors that maximize newly covered queries
    2. A query is "covered" by a page if all its TopR candidates are in that page
    3. Prioritize queries that have fewer total candidates (easier to fully cover)
    4. Support weighted queries from combined multi-TopR traces
    """
    if page_size <= 0:
        raise ValueError("--page-size must be positive")

    # Build query -> vectors mapping, filtering out oversized groups
    usable_query_vectors = {}
    query_weights = {}
    for query_id, vector_set in query_vectors.items():
        if not vector_set or len(vector_set) > max_group_size:
            continue
        usable_query_vectors[query_id] = frozenset(vector_set)
        # Use provided query weights (from combined trace), or default to inverse size
        if query_weights_map and query_id in query_weights_map:
            query_weights[query_id] = query_weights_map[query_id]
        else:
            query_weights[query_id] = 1.0 / len(vector_set)

    # Build vector -> queries mapping
    queries_by_vector = defaultdict(set)
    for query_id, vector_set in usable_query_vectors.items():
        for vector_id in vector_set:
            queries_by_vector[vector_id].add(query_id)

    default_payload_bytes = max(1, min(payload_bytes.values())) if payload_bytes else 1
    remaining = set(freq.keys())
    order = []

    # Track which queries are still "active" (not all candidates placed yet)
    active_queries = set(usable_query_vectors.keys())
    # Track remaining vectors needed per query
    query_remaining_vectors = {qid: set(usable_query_vectors[qid]) for qid in active_queries}

    while remaining:
        page = []
        page_bytes = 0
        page_vectors = set()

        while remaining:
            fit_candidates = [
                vector_id
                for vector_id in remaining
                if page_bytes + max(1, payload_bytes.get(vector_id, default_payload_bytes)) <= page_size
            ]
            if not fit_candidates:
                break

            def score_vector(vid):
                # Count how many queries would be newly "covered" (fully satisfied) by adding this vector
                # A query is covered if all its remaining vectors are in this page
                new_coverage = 0.0
                for qid in queries_by_vector.get(vid, ()):
                    if qid not in active_queries:
                        continue
                    remaining_after = query_remaining_vectors[qid] - page_vectors - {vid}
                    if len(remaining_after) == 0:
                        # This query would be fully covered
                        new_coverage += query_weights.get(qid, 1.0)

                # Secondary: frequency, then payload bytes (smaller preferred)
                return (new_coverage, freq[vid], -payload_bytes.get(vid, default_payload_bytes), -vid)

            selected = max(fit_candidates, key=score_vector)
            remaining.remove(selected)
            page.append(selected)
            page_vectors.add(selected)
            page_bytes += max(1, payload_bytes.get(selected, default_payload_bytes))

            # Update query remaining vectors
            for qid in queries_by_vector.get(selected, ()):
                if qid in query_remaining_vectors:
                    query_remaining_vectors[qid].discard(selected)

        if not page:
            # Fallback: take highest frequency remaining
            selected = max(remaining, key=lambda vid: (freq[vid], -payload_bytes.get(vid, default_payload_bytes), -vid))
            remaining.remove(selected)
            page.append(selected)

        order.extend(page)

        # Clean up completed queries
        for qid in list(active_queries):
            if not query_remaining_vectors.get(qid):
                # All vectors for this query have been placed
                active_queries.discard(qid)

    return order


def main():
    args = parse_args()
    query_vectors_by_posting = defaultdict(lambda: defaultdict(set))
    freq_by_posting = defaultdict(Counter)
    payload_bytes_by_posting = defaultdict(dict)
    query_weights_map = defaultdict(float)  # Aggregate weights per query

    with open(args.trace, newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            posting_id = int(row["posting_id"])
            query_id = int(row["query_id"])
            vector_id = int(row["vector_id"])
            query_vectors_by_posting[posting_id][query_id].add(vector_id)

            # Handle weighted traces
            weight = float(row.get("weight", 1.0))
            freq_by_posting[posting_id][vector_id] += weight
            query_weights_map[query_id] += weight

            payload_bytes_by_posting[posting_id][vector_id] = max(
                payload_bytes_by_posting[posting_id].get(vector_id, 0),
                int(row.get("payload_bytes", 0) or 0),
            )

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("w", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(["posting_id", "vector_id", "order_rank", "frequency"])

        for posting_id in sorted(freq_by_posting):
            freq = freq_by_posting[posting_id]
            if args.strategy == "adjacent":
                order = build_adjacent_order(query_vectors_by_posting[posting_id], freq, args.max_group_size)
            elif args.strategy == "page-aware":
                order = build_page_aware_order(
                    query_vectors_by_posting[posting_id],
                    freq,
                    payload_bytes_by_posting[posting_id],
                    args.page_size,
                    args.max_group_size,
                )
            else:  # query-set
                order = build_query_set_order(
                    query_vectors_by_posting[posting_id],
                    freq,
                    payload_bytes_by_posting[posting_id],
                    args.page_size,
                    args.max_group_size,
                    args.topr_weights,
                    query_weights_map,
                )

            for rank, vector_id in enumerate(order):
                writer.writerow([posting_id, vector_id, rank, freq[vector_id]])


if __name__ == "__main__":
    main()
