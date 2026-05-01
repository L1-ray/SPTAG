#!/usr/bin/env python3
import argparse
import csv
from collections import Counter
from typing import Dict, List, Tuple


def read_rows(path: str) -> Dict[int, Dict[str, str]]:
    with open(path, "r", encoding="utf-8") as fp:
        rows = {}
        for row in csv.DictReader(fp):
            query_id = int(row["query_id"])
            rows[query_id] = row
        return rows


def safe_float(value: str) -> float:
    try:
        return float(value)
    except Exception:
        return 0.0


def compare_rows(
    baseline: Dict[int, Dict[str, str]],
    candidate: Dict[int, Dict[str, str]],
) -> Tuple[Counter, List[Tuple[int, str, str, str]]]:
    counts: Counter = Counter()
    samples: List[Tuple[int, str, str, str]] = []
    shared_ids = sorted(set(baseline.keys()) & set(candidate.keys()))
    counts["shared_queries"] = len(shared_ids)
    counts["baseline_only"] = len(set(baseline.keys()) - set(candidate.keys()))
    counts["candidate_only"] = len(set(candidate.keys()) - set(baseline.keys()))

    for query_id in shared_ids:
        base = baseline[query_id]
        cand = candidate[query_id]

        same_coarse = base.get("coarse_candidate_hash", "") == cand.get("coarse_candidate_hash", "")
        same_payload = base.get("payload_page_hash", "") == cand.get("payload_page_hash", "")
        same_final = base.get("final_result_hash", "") == cand.get("final_result_hash", "")

        counts["coarse_match" if same_coarse else "coarse_mismatch"] += 1
        counts["payload_match" if same_payload else "payload_mismatch"] += 1
        counts["final_match" if same_final else "final_mismatch"] += 1

        if same_coarse and same_payload and same_final:
            counts["all_match"] += 1
            continue

        if not same_coarse:
            stage = "coarse"
        elif not same_payload:
            stage = "payload"
        else:
            stage = "final"
        counts[f"first_diverge_{stage}"] += 1

        if len(samples) < 20:
            samples.append(
                (
                    query_id,
                    stage,
                    base.get(f"{stage}_candidate_hash", base.get(f"{stage}_page_hash", base.get("final_result_hash", ""))),
                    cand.get(f"{stage}_candidate_hash", cand.get(f"{stage}_page_hash", cand.get("final_result_hash", ""))),
                )
            )

        base_recall = safe_float(base.get("recall", "0"))
        cand_recall = safe_float(cand.get("recall", "0"))
        if cand_recall < base_recall:
            counts["recall_drop_queries"] += 1
        elif cand_recall > base_recall:
            counts["recall_gain_queries"] += 1
        else:
            counts["recall_tie_queries"] += 1

    return counts, samples


def stage_hash(row: Dict[str, str], stage: str) -> str:
    if stage == "coarse":
        return row.get("coarse_candidate_hash", "")
    if stage == "payload":
        return row.get("payload_page_hash", "")
    return row.get("final_result_hash", "")


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare SPANN per-query stage hashes across runs.")
    parser.add_argument("--baseline", required=True, help="Baseline query_io_stats.csv path")
    parser.add_argument("--candidate", nargs="+", required=True, help="Candidate query_io_stats.csv paths")
    args = parser.parse_args()

    baseline_rows = read_rows(args.baseline)

    for candidate_path in args.candidate:
        candidate_rows = read_rows(candidate_path)
        counts, sample_queries = compare_rows(baseline_rows, candidate_rows)

        print(f"== {candidate_path} ==")
        print(f"shared_queries={counts['shared_queries']}")
        print(f"baseline_only={counts['baseline_only']} candidate_only={counts['candidate_only']}")
        print(
            "coarse_match={} coarse_mismatch={} payload_match={} payload_mismatch={} final_match={} final_mismatch={}".format(
                counts["coarse_match"],
                counts["coarse_mismatch"],
                counts["payload_match"],
                counts["payload_mismatch"],
                counts["final_match"],
                counts["final_mismatch"],
            )
        )
        print(
            "first_diverge_coarse={} first_diverge_payload={} first_diverge_final={}".format(
                counts["first_diverge_coarse"],
                counts["first_diverge_payload"],
                counts["first_diverge_final"],
            )
        )
        print(
            "recall_drop_queries={} recall_gain_queries={} recall_tie_queries={}".format(
                counts["recall_drop_queries"],
                counts["recall_gain_queries"],
                counts["recall_tie_queries"],
            )
        )

        if sample_queries:
            print("sample_divergences:")
            for query_id, stage, _, _ in sample_queries:
                base = baseline_rows[query_id]
                cand = candidate_rows[query_id]
                print(
                    "  query_id={} stage={} baseline_hash={} candidate_hash={} baseline_recall={:.6f} candidate_recall={:.6f}".format(
                        query_id,
                        stage,
                        stage_hash(base, stage),
                        stage_hash(cand, stage),
                        safe_float(base.get("recall", "0")),
                        safe_float(cand.get("recall", "0")),
                    )
                )
        print("")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
