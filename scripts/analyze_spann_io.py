#!/usr/bin/env python3
import argparse
import csv
import math
import re
from typing import Dict, List, Optional, Tuple


def safe_float(v: str, default: float = 0.0) -> float:
    try:
        return float(v)
    except Exception:
        return default


def safe_int(v: str, default: int = 0) -> int:
    try:
        return int(v)
    except Exception:
        return default


def read_csv_rows(path: Optional[str]) -> List[Dict[str, str]]:
    if not path:
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            return list(csv.DictReader(f))
    except FileNotFoundError:
        return []


def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    arr = sorted(values)
    idx = int(len(arr) * p)
    if idx >= len(arr):
        idx = len(arr) - 1
    return arr[idx]


def pearson(xs: List[float], ys: List[float]) -> float:
    if len(xs) != len(ys) or len(xs) < 2:
        return 0.0
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    num = 0.0
    denx = 0.0
    deny = 0.0
    for x, y in zip(xs, ys):
        dx = x - mx
        dy = y - my
        num += dx * dy
        denx += dx * dx
        deny += dy * dy
    if denx <= 0 or deny <= 0:
        return 0.0
    return num / math.sqrt(denx * deny)


def parse_sptag_log(path: Optional[str]) -> Dict[str, float]:
    if not path:
        return {}
    metrics: Dict[str, float] = {}
    qps_pattern = re.compile(r"actuallQPS is\s+([0-9.]+)")
    runtime_pattern = re.compile(r"总运行时间:\s*([0-9.]+)")
    search_stage_pattern = re.compile(r"SearchSSDIndex:\s*([0-9.]+)")
    read_mb_pattern = re.compile(r"累计读取:\s*([0-9.]+)")
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            m = qps_pattern.search(line)
            if m:
                metrics["qps"] = safe_float(m.group(1))
            m = runtime_pattern.search(line)
            if m:
                metrics["total_runtime_s"] = safe_float(m.group(1))
            m = search_stage_pattern.search(line)
            if m:
                metrics["search_stage_s"] = safe_float(m.group(1))
            m = read_mb_pattern.search(line)
            if m:
                metrics["read_mb"] = safe_float(m.group(1))
    return metrics


def overlap(a0: int, a1: int, b0: int, b1: int) -> bool:
    return a0 < b1 and b0 < a1


def build_query_system_join(
    query_rows: List[Dict[str, str]],
    disk_rows: List[Dict[str, str]],
) -> Tuple[List[float], List[float], List[float]]:
    query_latency: List[float] = []
    query_requested_bytes: List[float] = []
    query_avg_qd: List[float] = []
    parsed_disk = []
    for r in disk_rows:
        parsed_disk.append(
            (
                safe_int(r.get("sample_start_ns", "0")),
                safe_int(r.get("sample_end_ns", "0")),
                safe_float(r.get("avg_queue_depth", "0")),
            )
        )
    for r in query_rows:
        q0 = safe_int(r.get("query_start_ns", "0"))
        q1 = safe_int(r.get("query_end_ns", "0"))
        latency = safe_float(r.get("total_latency_ms", "0"))
        requested = safe_float(r.get("requested_read_bytes", "0"))
        qd_vals = [qd for (s0, s1, qd) in parsed_disk if overlap(q0, q1, s0, s1)]
        avg_qd = sum(qd_vals) / len(qd_vals) if qd_vals else 0.0
        query_latency.append(latency)
        query_requested_bytes.append(requested)
        query_avg_qd.append(avg_qd)
    return query_latency, query_requested_bytes, query_avg_qd


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze SPANN I/O metrics")
    parser.add_argument("--query-csv", default="", help="per-query detailed CSV from SPTAG")
    parser.add_argument("--disk-csv", default="", help="disk_stats.csv from spann_io_monitor.py")
    parser.add_argument("--process-csv", default="", help="process_io_stats.csv from spann_io_monitor.py")
    parser.add_argument("--cpu-csv", default="", help="cpu_stats.csv from spann_io_monitor.py")
    parser.add_argument("--psi-csv", default="", help="psi_io_stats.csv from spann_io_monitor.py")
    parser.add_argument("--sptag-log", default="", help="SPTAG search log")
    parser.add_argument("--output-report", required=True, help="output markdown report")
    args = parser.parse_args()

    query_rows = read_csv_rows(args.query_csv)
    disk_rows = read_csv_rows(args.disk_csv)
    proc_rows = read_csv_rows(args.process_csv)
    cpu_rows = read_csv_rows(args.cpu_csv)
    psi_rows = read_csv_rows(args.psi_csv)
    sptag_metrics = parse_sptag_log(args.sptag_log) if args.sptag_log else {}

    query_latency = [safe_float(r.get("total_latency_ms", "0")) for r in query_rows]
    query_io_wait = [safe_float(r.get("io_wait_ms", "0")) for r in query_rows]
    requested_bytes = [safe_float(r.get("requested_read_bytes", "0")) for r in query_rows]

    duplicate_ratio = []
    distance_ratio = []
    final_ratio = []
    for r in query_rows:
        raw = safe_float(r.get("posting_elements_raw", "0"))
        dup = safe_float(r.get("duplicate_vector_count", "0"))
        dist_eval = safe_float(r.get("distance_evaluated_count", "0"))
        final = safe_float(r.get("final_result_count", "0"))
        if raw > 0:
            duplicate_ratio.append(dup / raw)
            distance_ratio.append(dist_eval / raw)
            final_ratio.append(final / raw)

    q_latency_join, q_req_join, q_qd_join = build_query_system_join(query_rows, disk_rows)
    corr_latency_req = pearson(q_latency_join, q_req_join)
    corr_latency_qd = pearson(q_latency_join, q_qd_join)

    avg_bw = 0.0
    if disk_rows:
        bws = [safe_float(r.get("read_bandwidth_mbs", "0")) for r in disk_rows]
        avg_bw = sum(bws) / len(bws) if bws else 0.0
    avg_cpu_iowait = 0.0
    if cpu_rows:
        vals = [safe_float(r.get("cpu_iowait_percent", "0")) for r in cpu_rows]
        avg_cpu_iowait = sum(vals) / len(vals) if vals else 0.0
    psi_some_delta = sum(safe_int(r.get("psi_io_some_delta_us", "0")) for r in psi_rows)
    psi_full_delta = sum(safe_int(r.get("psi_io_full_delta_us", "0")) for r in psi_rows)
    proc_read_delta = sum(safe_int(r.get("read_bytes_delta", "0")) for r in proc_rows)

    avg_latency = sum(query_latency) / len(query_latency) if query_latency else 0.0
    p95_latency = percentile(query_latency, 0.95) if query_latency else 0.0
    p99_latency = percentile(query_latency, 0.99) if query_latency else 0.0
    avg_io_wait = sum(query_io_wait) / len(query_io_wait) if query_io_wait else 0.0
    io_wait_ratio = (avg_io_wait / avg_latency) if avg_latency > 0 else 0.0

    bottleneck = "暂无法判定（缺少查询级数据）"
    if query_rows:
        if io_wait_ratio >= 0.35 and (corr_latency_qd > 0.2 or psi_some_delta > 0):
            bottleneck = "I/O 等待是主要瓶颈"
        elif corr_latency_req > 0.3:
            bottleneck = "读取放大显著，且与延迟相关"
        else:
            bottleneck = "存在混合瓶颈，I/O 不是唯一主导"

    with open(args.output_report, "w", encoding="utf-8") as f:
        f.write("# SPANN 搜索 I/O 性能分析报告\n\n")
        f.write("## 输入概览\n")
        f.write(f"- query_rows: {len(query_rows)}\n")
        f.write(f"- disk_rows: {len(disk_rows)}\n")
        f.write(f"- process_rows: {len(proc_rows)}\n")
        f.write(f"- cpu_rows: {len(cpu_rows)}\n")
        f.write(f"- psi_rows: {len(psi_rows)}\n")
        if sptag_metrics:
            f.write(f"- log_qps: {sptag_metrics.get('qps', 0.0):.3f}\n")
            f.write(f"- log_total_runtime_s: {sptag_metrics.get('total_runtime_s', 0.0):.3f}\n")
            f.write(f"- log_search_stage_s: {sptag_metrics.get('search_stage_s', 0.0):.3f}\n")
            f.write(f"- log_read_mb: {sptag_metrics.get('read_mb', 0.0):.3f}\n")
        f.write("\n")

        f.write("## 瓶颈分析\n")
        f.write(f"- 结论: **{bottleneck}**\n")
        f.write(f"- avg_latency_ms: {avg_latency:.3f}\n")
        f.write(f"- p95_latency_ms: {p95_latency:.3f}\n")
        f.write(f"- p99_latency_ms: {p99_latency:.3f}\n")
        f.write(f"- avg_io_wait_ms: {avg_io_wait:.3f}\n")
        f.write(f"- io_wait_ratio: {io_wait_ratio:.3f}\n")
        f.write(f"- corr(latency, requested_bytes): {corr_latency_req:.3f}\n")
        f.write(f"- corr(latency, avg_queue_depth): {corr_latency_qd:.3f}\n")
        f.write("\n")

        f.write("## 系统级信号\n")
        f.write(f"- avg_read_bandwidth_mbs: {avg_bw:.3f}\n")
        f.write(f"- avg_cpu_iowait_percent: {avg_cpu_iowait:.3f}\n")
        f.write(f"- psi_io_some_delta_us: {psi_some_delta}\n")
        f.write(f"- psi_io_full_delta_us: {psi_full_delta}\n")
        f.write(f"- process_read_bytes_delta: {proc_read_delta}\n")
        f.write("\n")

        f.write("## 查询级效率指标\n")
        if duplicate_ratio:
            f.write(f"- avg_duplicate_vector_read_ratio: {sum(duplicate_ratio) / len(duplicate_ratio):.6f}\n")
        if distance_ratio:
            f.write(f"- avg_distance_eval_ratio: {sum(distance_ratio) / len(distance_ratio):.6f}\n")
        if final_ratio:
            f.write(f"- avg_final_result_ratio: {sum(final_ratio) / len(final_ratio):.6f}\n")
        if requested_bytes:
            f.write(f"- avg_requested_read_bytes: {sum(requested_bytes) / len(requested_bytes):.3f}\n")
        f.write("\n")

        f.write("## 备注\n")
        f.write("- 当缺少 query-csv 时，报告会退化为日志与系统级摘要。\n")
        f.write("- 相关性分析基于同一 monotonic ns 时间窗重叠 join。\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
