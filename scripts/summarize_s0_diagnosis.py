#!/usr/bin/env python3
"""Merge per-run S0 JSON summaries into one TSV."""

from __future__ import annotations

import argparse
import glob
import json
import os
from typing import Dict, List


FIELDS = [
    "run",
    "query_count",
    "avg_recall",
    "avg_total_latency_ms",
    "p95_total_latency_ms",
    "approx_single_thread_qps",
    "avg_requested_read_bytes",
    "avg_pages_read",
    "payload_stats_supported",
    "payload_wait_supported",
    "payload_read_wait_over_ex",
    "avg_candidates_per_unique_payload_page",
    "top_direction",
    "top_score",
    "reason_1",
]


def _pick(run_name: str, data: Dict[str, object]) -> Dict[str, object]:
    ranking = data.get("recommendation", {}).get("ranking", [])
    reasons = data.get("recommendation", {}).get("reasons", [])
    top_direction = ""
    top_score = ""
    if ranking:
        top_direction = ranking[0].get("direction", "")
        top_score = ranking[0].get("score", "")
    return {
        "run": run_name,
        "query_count": data.get("query_count", ""),
        "avg_recall": data.get("avg_recall", ""),
        "avg_total_latency_ms": data.get("avg_total_latency_ms", ""),
        "p95_total_latency_ms": data.get("p95_total_latency_ms", ""),
        "approx_single_thread_qps": data.get("approx_single_thread_qps", ""),
        "avg_requested_read_bytes": data.get("avg_requested_read_bytes", ""),
        "avg_pages_read": data.get("avg_pages_read", ""),
        "payload_stats_supported": data.get("payload_stats_supported", ""),
        "payload_wait_supported": data.get("payload_wait_supported", ""),
        "payload_read_wait_over_ex": data.get("payload_read_wait_over_ex", ""),
        "avg_candidates_per_unique_payload_page": data.get("avg_candidates_per_unique_payload_page", ""),
        "top_direction": top_direction,
        "top_score": top_score,
        "reason_1": reasons[0] if reasons else "",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize S0 diagnosis JSON files into TSV.")
    parser.add_argument("--input-glob", required=True, help="Glob for *_s0_summary.json files")
    parser.add_argument("--output-tsv", required=True, help="Output TSV path")
    args = parser.parse_args()

    files = sorted(glob.glob(args.input_glob))
    if not files:
        raise FileNotFoundError(f"No files matched: {args.input_glob}")

    rows: List[Dict[str, object]] = []
    for path in files:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        run_name = os.path.basename(path).replace("_s0_summary.json", "")
        rows.append(_pick(run_name, data))

    os.makedirs(os.path.dirname(os.path.abspath(args.output_tsv)), exist_ok=True)
    with open(args.output_tsv, "w", encoding="utf-8") as out:
        out.write("\t".join(FIELDS) + "\n")
        for row in rows:
            out.write("\t".join(str(row.get(k, "")) for k in FIELDS) + "\n")


if __name__ == "__main__":
    main()
