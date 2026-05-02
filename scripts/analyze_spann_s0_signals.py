#!/usr/bin/env python3
"""S0 diagnosis for official baseline and experimental runs.

Inputs:
- query_io_stats.csv from EnableDetailedIOStats
- optional payload_trace.csv from EnablePayloadTrace

Outputs:
- JSON summary with derived metrics and directional recommendations
- optional Markdown report for fast review
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import statistics
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


def _to_float(row: Dict[str, str], key: str, default: float = 0.0) -> float:
    raw = row.get(key, "")
    if raw is None or raw == "":
        return default
    try:
        return float(raw)
    except ValueError:
        return default


def _to_int(row: Dict[str, str], key: str, default: int = 0) -> int:
    raw = row.get(key, "")
    if raw is None or raw == "":
        return default
    try:
        return int(float(raw))
    except ValueError:
        return default


def _quantile(sorted_values: Sequence[float], p: float) -> float:
    if not sorted_values:
        return 0.0
    if p <= 0:
        return float(sorted_values[0])
    if p >= 1:
        return float(sorted_values[-1])
    idx = (len(sorted_values) - 1) * p
    lower = math.floor(idx)
    upper = math.ceil(idx)
    if lower == upper:
        return float(sorted_values[lower])
    frac = idx - lower
    return float(sorted_values[lower] * (1 - frac) + sorted_values[upper] * frac)


def _share_top_k(values: Sequence[float], ratio: float) -> float:
    if not values:
        return 0.0
    total = sum(values)
    if total <= 0:
        return 0.0
    k = max(1, int(math.ceil(len(values) * ratio)))
    top_sum = sum(sorted(values, reverse=True)[:k])
    return float(top_sum / total)


def _gini(values: Sequence[float]) -> float:
    clean = [float(v) for v in values if v >= 0]
    n = len(clean)
    if n == 0:
        return 0.0
    s = sum(clean)
    if s <= 0:
        return 0.0
    sorted_vals = sorted(clean)
    weighted = 0.0
    for i, v in enumerate(sorted_vals, start=1):
        weighted += i * v
    return (2.0 * weighted) / (n * s) - (n + 1.0) / n


@dataclass
class QueryRow:
    query_id: int
    query_start_ns: int
    query_end_ns: int
    total_latency_ms: float
    ex_latency_ms: float
    io_wait_ms: float
    batch_read_total_ms: float
    payload_read_wait_ms: float
    requested_read_bytes: float
    payload_physical_bytes: float
    pages_read: float
    unique_payload_pages: float
    payload_candidates: float
    duplicate_vector_count: float
    postings_touched: float
    recall: float


def load_query_rows(path: str) -> List[QueryRow]:
    rows: List[QueryRow] = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                QueryRow(
                    query_id=_to_int(row, "query_id", len(rows)),
                    query_start_ns=_to_int(row, "query_start_ns", 0),
                    query_end_ns=_to_int(row, "query_end_ns", 0),
                    total_latency_ms=_to_float(row, "total_latency_ms", 0.0),
                    ex_latency_ms=_to_float(row, "ex_latency_ms", 0.0),
                    io_wait_ms=_to_float(row, "io_wait_ms", 0.0),
                    batch_read_total_ms=_to_float(row, "batch_read_total_ms", 0.0),
                    payload_read_wait_ms=_to_float(row, "payload_read_wait_ms", 0.0),
                    requested_read_bytes=_to_float(row, "requested_read_bytes", 0.0),
                    payload_physical_bytes=_to_float(row, "payload_physical_bytes", 0.0),
                    pages_read=_to_float(row, "pages_read", 0.0),
                    unique_payload_pages=_to_float(row, "unique_payload_pages", 0.0),
                    payload_candidates=_to_float(row, "payload_candidates", 0.0),
                    duplicate_vector_count=_to_float(row, "duplicate_vector_count", 0.0),
                    postings_touched=_to_float(row, "postings_touched", 0.0),
                    recall=_to_float(row, "recall", -1.0),
                )
            )
    return rows


def summarize_query_rows(rows: Sequence[QueryRow]) -> Dict[str, object]:
    if not rows:
        return {"query_count": 0}

    total_latency = [r.total_latency_ms for r in rows]
    ex_latency = [r.ex_latency_ms for r in rows]
    io_wait = [r.io_wait_ms for r in rows]
    batch_wait = [r.batch_read_total_ms for r in rows]
    payload_wait = [r.payload_read_wait_ms for r in rows]
    requested = [r.requested_read_bytes for r in rows]
    payload_bytes = [r.payload_physical_bytes for r in rows]
    pages = [r.pages_read for r in rows]
    unique_pages = [r.unique_payload_pages for r in rows]
    candidates = [r.payload_candidates for r in rows]
    postings = [r.postings_touched for r in rows]
    dup_vec = [r.duplicate_vector_count for r in rows]
    recalls = [r.recall for r in rows if r.recall >= 0]

    payload_stats_supported = any(
        (r.payload_physical_bytes > 0.0 or r.unique_payload_pages > 0.0 or r.payload_candidates > 0.0) for r in rows
    )
    payload_wait_supported = any(r.payload_read_wait_ms > 0.0 for r in rows)

    avg_total = statistics.mean(total_latency)
    avg_ex = statistics.mean(ex_latency)
    avg_io_wait = statistics.mean(io_wait)
    avg_batch_wait = statistics.mean(batch_wait)
    avg_payload_wait = statistics.mean(payload_wait)
    avg_requested = statistics.mean(requested)
    avg_payload = statistics.mean(payload_bytes)
    avg_pages = statistics.mean(pages)
    avg_unique_pages = statistics.mean(unique_pages)
    avg_candidates = statistics.mean(candidates)
    avg_postings = statistics.mean(postings)
    avg_dup = statistics.mean(dup_vec)
    avg_recall = statistics.mean(recalls) if recalls else -1.0

    payload_wait_over_ex = avg_payload_wait / avg_ex if (avg_ex > 0 and payload_wait_supported) else -1.0
    io_wait_over_ex = avg_io_wait / avg_ex if avg_ex > 0 else 0.0
    approx_qps = 1000.0 / avg_total if avg_total > 0 else 0.0
    avg_candidates_per_unique_page = (
        avg_candidates / avg_unique_pages if (payload_stats_supported and avg_unique_pages > 0) else -1.0
    )
    avg_duplicate_page_like_ratio = (
        (avg_pages - avg_unique_pages) / avg_pages if (payload_stats_supported and avg_pages > 0) else -1.0
    )

    summary = {
        "query_count": len(rows),
        "avg_recall": avg_recall,
        "avg_total_latency_ms": avg_total,
        "avg_ex_latency_ms": avg_ex,
        "avg_io_wait_ms": avg_io_wait,
        "avg_batch_read_total_ms": avg_batch_wait,
        "avg_payload_read_wait_ms": avg_payload_wait,
        "avg_requested_read_bytes": avg_requested,
        "avg_payload_physical_bytes": avg_payload,
        "avg_pages_read": avg_pages,
        "avg_unique_payload_pages": avg_unique_pages,
        "avg_payload_candidates": avg_candidates,
        "avg_postings_touched": avg_postings,
        "avg_duplicate_vector_count": avg_dup,
        "payload_stats_supported": payload_stats_supported,
        "payload_wait_supported": payload_wait_supported,
        "payload_read_wait_over_ex": payload_wait_over_ex,
        "io_wait_over_ex": io_wait_over_ex,
        "approx_single_thread_qps": approx_qps,
        "avg_candidates_per_unique_payload_page": avg_candidates_per_unique_page,
        "avg_duplicate_page_like_ratio": avg_duplicate_page_like_ratio,
        "p50_total_latency_ms": _quantile(sorted(total_latency), 0.5),
        "p95_total_latency_ms": _quantile(sorted(total_latency), 0.95),
        "p99_total_latency_ms": _quantile(sorted(total_latency), 0.99),
        "p95_ex_latency_ms": _quantile(sorted(ex_latency), 0.95),
        "p95_payload_read_wait_ms": _quantile(sorted(payload_wait), 0.95) if payload_wait_supported else -1.0,
        "gini_payload_bytes_per_query": _gini(payload_bytes) if payload_stats_supported else -1.0,
        "gini_pages_per_query": _gini(pages),
        "gini_payload_wait_per_query": _gini(payload_wait) if payload_wait_supported else -1.0,
        "top10_query_share_payload_bytes": _share_top_k(payload_bytes, 0.10),
        "top10_query_share_pages": _share_top_k(pages, 0.10),
        "top10_query_share_payload_wait": _share_top_k(payload_wait, 0.10) if payload_wait_supported else -1.0,
        "top1_query_share_payload_wait": _share_top_k(payload_wait, 0.01) if payload_wait_supported else -1.0,
    }
    return summary


def _summarize_trace_hits(
    query_to_pages: Dict[int, set],
    query_to_postings: Dict[int, set],
    page_freq: Counter,
    posting_freq: Counter,
    page_to_queries: Dict[int, set],
    rows: int,
    trace_prefix: str,
) -> Dict[str, object]:
    unique_pages = len(page_freq)
    unique_postings = len(posting_freq)
    total_page_hits = sum(page_freq.values())
    total_posting_hits = sum(posting_freq.values())

    def _top_share(counter: Counter, ratio: float) -> float:
        if not counter:
            return 0.0
        values = sorted(counter.values(), reverse=True)
        k = max(1, int(math.ceil(len(values) * ratio)))
        return sum(values[:k]) / sum(values)

    cross_query_reuse_pages = sum(1 for s in page_to_queries.values() if len(s) > 1)
    cross_query_reuse_ratio = cross_query_reuse_pages / unique_pages if unique_pages > 0 else 0.0

    avg_unique_pages_per_query = (
        statistics.mean([len(v) for v in query_to_pages.values()]) if query_to_pages else 0.0
    )
    avg_unique_postings_per_query = (
        statistics.mean([len(v) for v in query_to_postings.values()]) if query_to_postings else 0.0
    )

    return {
        f"{trace_prefix}_rows": rows,
        f"{trace_prefix}_query_count": len(query_to_pages),
        f"{trace_prefix}_unique_pages": unique_pages,
        f"{trace_prefix}_unique_postings": unique_postings,
        f"{trace_prefix}_total_page_hits": total_page_hits,
        f"{trace_prefix}_total_posting_hits": total_posting_hits,
        f"{trace_prefix}_avg_unique_pages_per_query": avg_unique_pages_per_query,
        f"{trace_prefix}_avg_unique_postings_per_query": avg_unique_postings_per_query,
        f"{trace_prefix}_top1pct_page_hit_share": _top_share(page_freq, 0.01),
        f"{trace_prefix}_top10pct_page_hit_share": _top_share(page_freq, 0.10),
        f"{trace_prefix}_top1pct_posting_hit_share": _top_share(posting_freq, 0.01),
        f"{trace_prefix}_top10pct_posting_hit_share": _top_share(posting_freq, 0.10),
        f"{trace_prefix}_cross_query_reused_pages": cross_query_reuse_pages,
        f"{trace_prefix}_cross_query_reuse_page_ratio": cross_query_reuse_ratio,
    }


def load_payload_trace(path: str) -> Dict[str, object]:
    query_to_pages: Dict[int, set] = defaultdict(set)
    query_to_postings: Dict[int, set] = defaultdict(set)
    page_freq: Counter = Counter()
    posting_freq: Counter = Counter()
    page_to_queries: Dict[int, set] = defaultdict(set)

    rows = 0
    with open(path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows += 1
            qid = _to_int(row, "query_id", -1)
            page_id = _to_int(row, "payload_page_id", -1)
            posting_id = _to_int(row, "posting_id", -1)
            if qid < 0 or page_id < 0 or posting_id < 0:
                continue
            query_to_pages[qid].add(page_id)
            query_to_postings[qid].add(posting_id)
            page_freq[page_id] += 1
            posting_freq[posting_id] += 1
            page_to_queries[page_id].add(qid)

    return _summarize_trace_hits(
        query_to_pages, query_to_postings, page_freq, posting_freq, page_to_queries, rows, "trace"
    )


def load_io_request_trace(path: str) -> Dict[str, object]:
    query_to_pages: Dict[int, set] = defaultdict(set)
    query_to_postings: Dict[int, set] = defaultdict(set)
    page_freq: Counter = Counter()
    posting_freq: Counter = Counter()
    page_to_queries: Dict[int, set] = defaultdict(set)

    rows = 0
    with open(path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows += 1
            qid = _to_int(row, "query_id", -1)
            page_id = _to_int(row, "page_id", -1)
            posting_id = _to_int(row, "posting_id", -1)
            page_count = max(1, _to_int(row, "page_count", 1))
            if qid < 0 or page_id < 0 or posting_id < 0:
                continue
            query_to_postings[qid].add(posting_id)
            query_to_pages[qid].add(page_id)
            posting_freq[posting_id] += 1
            for p in range(page_count):
                pid = page_id + p
                page_freq[pid] += 1
                page_to_queries[pid].add(qid)

    return _summarize_trace_hits(
        query_to_pages, query_to_postings, page_freq, posting_freq, page_to_queries, rows, "io_trace"
    )


def build_recommendation(summary: Dict[str, object]) -> Dict[str, object]:
    payload_stats_supported = bool(summary.get("payload_stats_supported", False))
    payload_wait_supported = bool(summary.get("payload_wait_supported", False))

    payload_wait_ratio = float(summary.get("payload_read_wait_over_ex", -1.0))
    top10_wait_share = float(summary.get("top10_query_share_payload_wait", -1.0))
    avg_cand_per_page = float(summary.get("avg_candidates_per_unique_payload_page", -1.0))
    gini_wait = float(summary.get("gini_payload_wait_per_query", -1.0))

    trace_top10_page = max(
        float(summary.get("trace_top10pct_page_hit_share", 0.0)),
        float(summary.get("io_trace_top10pct_page_hit_share", 0.0)),
    )
    trace_top10_posting = max(
        float(summary.get("trace_top10pct_posting_hit_share", 0.0)),
        float(summary.get("io_trace_top10pct_posting_hit_share", 0.0)),
    )
    trace_reuse = max(
        float(summary.get("trace_cross_query_reuse_page_ratio", 0.0)),
        float(summary.get("io_trace_cross_query_reuse_page_ratio", 0.0)),
    )

    m1_score = 0
    m2h_score = 0
    m4_score = 0

    if payload_wait_supported and payload_wait_ratio >= 0.55:
        m1_score += 2
    if payload_wait_supported and top10_wait_share >= 0.30:
        m1_score += 1
    if payload_wait_supported and gini_wait >= 0.35:
        m1_score += 1
    if trace_top10_page >= 0.30 or trace_reuse >= 0.10:
        m1_score += 2

    if payload_stats_supported and 0 < avg_cand_per_page <= 6.0:
        m2h_score += 2
    if trace_top10_posting >= 0.30:
        m2h_score += 2
    if trace_top10_page >= 0.30:
        m2h_score += 1

    avg_dup_vec = float(summary.get("avg_duplicate_vector_count", 0.0))
    if avg_dup_vec > 0:
        m4_score += 1
    if trace_reuse >= 0.10:
        m4_score += 1
    if payload_stats_supported and 0 < avg_cand_per_page <= 6.0:
        m4_score += 1

    ranked = sorted(
        [("M1", m1_score), ("M2-H", m2h_score), ("M4", m4_score)],
        key=lambda x: x[1],
        reverse=True,
    )

    reasons: List[str] = []
    if not payload_stats_supported:
        reasons.append("payload-locality columns are unavailable/zero in this run; provide payload trace or richer instrumentation")
    if not payload_wait_supported:
        reasons.append("payload read wait columns are unavailable/zero; M1 scoring is conservative")
    if payload_wait_supported and payload_wait_ratio >= 0.55:
        reasons.append("payload_read_wait_over_ex is high; read wait is likely dominant")
    if payload_stats_supported and 0 < avg_cand_per_page <= 6.0:
        reasons.append("candidates per unique payload page is low; page fanout/locality is weak")
    if trace_top10_page >= 0.30:
        reasons.append("page hotness concentration is high; cache/coalescing should have room")
    if trace_reuse >= 0.10:
        reasons.append("cross-query page reuse is visible; global page broker can exploit sharing")
    if not reasons:
        reasons.append("current traces do not yet show strong concentration/reuse signals")

    return {
        "ranking": [{"direction": d, "score": s} for d, s in ranked],
        "top_direction": ranked[0][0] if ranked else "M1",
        "reasons": reasons,
    }


def write_markdown(path: str, title: str, summary: Dict[str, object]) -> None:
    rec = summary.get("recommendation", {})
    ranking = rec.get("ranking", [])
    lines: List[str] = []
    lines.append(f"# {title}")
    lines.append("")
    lines.append("## Snapshot")
    lines.append("")
    lines.append(f"- Query count: `{summary.get('query_count', 0)}`")
    lines.append(f"- Avg recall: `{float(summary.get('avg_recall', -1.0)):.6f}`")
    lines.append(f"- Avg total latency: `{float(summary.get('avg_total_latency_ms', 0.0)):.3f} ms`")
    lines.append(f"- Approx single-thread QPS: `{float(summary.get('approx_single_thread_qps', 0.0)):.2f}`")
    payload_wait_ratio = float(summary.get("payload_read_wait_over_ex", -1.0))
    candidates_per_page = float(summary.get("avg_candidates_per_unique_payload_page", -1.0))
    lines.append(f"- Payload wait / ex latency: `{'N/A' if payload_wait_ratio < 0 else f'{payload_wait_ratio:.3f}'}`")
    lines.append(
        f"- Avg candidates per unique payload page: `{'N/A' if candidates_per_page < 0 else f'{candidates_per_page:.3f}'}`"
    )
    lines.append("")
    lines.append("## Concentration")
    lines.append("")
    top10_payload_wait = float(summary.get("top10_query_share_payload_wait", -1.0))
    gini_payload_wait = float(summary.get("gini_payload_wait_per_query", -1.0))
    lines.append(
        f"- Top10 query share of payload wait: `{'N/A' if top10_payload_wait < 0 else f'{top10_payload_wait:.3f}'}`"
    )
    lines.append(
        f"- Gini(payload wait per query): `{'N/A' if gini_payload_wait < 0 else f'{gini_payload_wait:.3f}'}`"
    )
    if "trace_rows" in summary:
        lines.append(f"- Trace rows: `{int(summary.get('trace_rows', 0))}`")
        lines.append(
            f"- Top10 page-hit share: `{float(summary.get('trace_top10pct_page_hit_share', 0.0)):.3f}`"
        )
        lines.append(
            f"- Top10 posting-hit share: `{float(summary.get('trace_top10pct_posting_hit_share', 0.0)):.3f}`"
        )
        lines.append(
            f"- Cross-query reuse page ratio: `{float(summary.get('trace_cross_query_reuse_page_ratio', 0.0)):.3f}`"
        )
    if "io_trace_rows" in summary:
        lines.append(f"- IO trace rows: `{int(summary.get('io_trace_rows', 0))}`")
        lines.append(
            f"- IO trace top10 page-hit share: `{float(summary.get('io_trace_top10pct_page_hit_share', 0.0)):.3f}`"
        )
        lines.append(
            f"- IO trace top10 posting-hit share: `{float(summary.get('io_trace_top10pct_posting_hit_share', 0.0)):.3f}`"
        )
        lines.append(
            f"- IO trace cross-query reuse page ratio: `{float(summary.get('io_trace_cross_query_reuse_page_ratio', 0.0)):.3f}`"
        )
    lines.append("")
    lines.append("## Direction Ranking")
    lines.append("")
    for item in ranking:
        lines.append(f"- `{item['direction']}` score `{item['score']}`")
    lines.append("")
    lines.append("## Why")
    lines.append("")
    for reason in rec.get("reasons", []):
        lines.append(f"- {reason}")
    lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze S0 signals from query_io_stats and optional payload trace.")
    parser.add_argument("--query-csv", required=True, help="Path to query_io_stats.csv")
    parser.add_argument("--payload-trace", default="", help="Optional payload trace CSV")
    parser.add_argument("--io-request-trace", default="", help="Optional IO request trace CSV")
    parser.add_argument("--output-json", required=True, help="Output JSON summary path")
    parser.add_argument("--output-md", default="", help="Optional Markdown report path")
    parser.add_argument("--title", default="SPANN S0 Diagnosis", help="Markdown title")
    args = parser.parse_args()

    rows = load_query_rows(args.query_csv)
    summary = summarize_query_rows(rows)
    summary["input_query_csv"] = os.path.abspath(args.query_csv)

    if args.payload_trace:
        trace_summary = load_payload_trace(args.payload_trace)
        summary.update(trace_summary)
        summary["input_payload_trace"] = os.path.abspath(args.payload_trace)
    if args.io_request_trace:
        io_trace_summary = load_io_request_trace(args.io_request_trace)
        summary.update(io_trace_summary)
        summary["input_io_request_trace"] = os.path.abspath(args.io_request_trace)

    summary["recommendation"] = build_recommendation(summary)

    out_dir = os.path.dirname(os.path.abspath(args.output_json))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.output_json, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, sort_keys=True)

    if args.output_md:
        md_dir = os.path.dirname(os.path.abspath(args.output_md))
        if md_dir:
            os.makedirs(md_dir, exist_ok=True)
        write_markdown(args.output_md, args.title, summary)


if __name__ == "__main__":
    main()
