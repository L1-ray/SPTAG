#!/usr/bin/env python3
import argparse
import csv
import os
import time
from dataclasses import dataclass
from typing import Dict, Optional


SECTOR_SIZE = 512.0
MB = 1024.0 * 1024.0


@dataclass
class DiskStats:
    sectors_read: int
    ios_in_progress: int
    weighted_io_ms: int


@dataclass
class ProcIoStats:
    read_bytes: int
    write_bytes: int
    cancelled_write_bytes: int


@dataclass
class CpuStats:
    total: int
    idle: int
    iowait: int


@dataclass
class PsiStats:
    some_total_us: int
    full_total_us: int


def read_diskstats(device: str) -> DiskStats:
    with open("/proc/diskstats", "r", encoding="utf-8") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) < 14:
                continue
            if parts[2] != device:
                continue
            fields = [int(x) for x in parts[3:14]]
            return DiskStats(
                sectors_read=fields[2],
                ios_in_progress=fields[8],
                weighted_io_ms=fields[10],
            )
    raise RuntimeError(f"device '{device}' not found in /proc/diskstats")


def read_proc_io(pid: int) -> ProcIoStats:
    values: Dict[str, int] = {}
    path = f"/proc/{pid}/io"
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if ":" not in line:
                continue
            k, v = line.split(":", 1)
            values[k.strip()] = int(v.strip())
    return ProcIoStats(
        read_bytes=values.get("read_bytes", 0),
        write_bytes=values.get("write_bytes", 0),
        cancelled_write_bytes=values.get("cancelled_write_bytes", 0),
    )


def read_cpu_stat() -> CpuStats:
    with open("/proc/stat", "r", encoding="utf-8") as f:
        first = f.readline().strip().split()
    if len(first) < 6 or first[0] != "cpu":
        raise RuntimeError("failed to parse /proc/stat")
    nums = [int(x) for x in first[1:]]
    total = sum(nums)
    idle = nums[3]
    iowait = nums[4] if len(nums) > 4 else 0
    return CpuStats(total=total, idle=idle, iowait=iowait)


def read_psi_io() -> PsiStats:
    some_total = 0
    full_total = 0
    with open("/proc/pressure/io", "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            head, rest = line.split(" ", 1)
            fields = {}
            for kv in rest.split():
                if "=" not in kv:
                    continue
                k, v = kv.split("=", 1)
                fields[k] = v
            total = int(float(fields.get("total", "0")))
            if head == "some":
                some_total = total
            elif head == "full":
                full_total = total
    return PsiStats(some_total_us=some_total, full_total_us=full_total)


def pid_alive(pid: int) -> bool:
    return os.path.exists(f"/proc/{pid}")


def clamp_non_negative(value: float) -> float:
    return value if value >= 0 else 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description="SPANN system-level I/O monitor")
    parser.add_argument("--device", required=True, help="disk device name, e.g. nvme0n1 or sda")
    parser.add_argument("--pid", required=True, type=int, help="target process PID")
    parser.add_argument("--interval-ms", type=int, default=100, help="sampling interval in milliseconds")
    parser.add_argument("--device-max-read-mbps", type=float, default=0.0, help="device baseline read bandwidth (MB/s)")
    parser.add_argument("--output-dir", required=True, help="output directory for csv/summary")
    parser.add_argument("--duration-s", type=float, default=0.0, help="optional max duration in seconds")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    disk_csv_path = os.path.join(args.output_dir, "disk_stats.csv")
    proc_csv_path = os.path.join(args.output_dir, "process_io_stats.csv")
    cpu_csv_path = os.path.join(args.output_dir, "cpu_stats.csv")
    psi_csv_path = os.path.join(args.output_dir, "psi_io_stats.csv")
    summary_path = os.path.join(args.output_dir, "summary.txt")

    interval_s = max(0.01, args.interval_ms / 1000.0)
    start_wall = time.time()

    prev_t_ns = time.monotonic_ns()
    prev_disk = read_diskstats(args.device)
    prev_cpu = read_cpu_stat()
    prev_psi = read_psi_io()
    prev_proc: Optional[ProcIoStats] = read_proc_io(args.pid) if pid_alive(args.pid) else None

    first_proc = prev_proc
    last_proc = prev_proc

    total_read_bytes = 0.0
    total_elapsed_s = 0.0
    queue_depth_sum = 0.0
    queue_depth_peak = 0.0
    cpu_idle_sum = 0.0
    cpu_iowait_sum = 0.0
    sample_count = 0
    psi_some_delta_total = 0
    psi_full_delta_total = 0
    weighted_io_delta_total = 0.0

    with open(disk_csv_path, "w", newline="", encoding="utf-8") as disk_f, \
         open(proc_csv_path, "w", newline="", encoding="utf-8") as proc_f, \
         open(cpu_csv_path, "w", newline="", encoding="utf-8") as cpu_f, \
         open(psi_csv_path, "w", newline="", encoding="utf-8") as psi_f:

        disk_w = csv.writer(disk_f)
        proc_w = csv.writer(proc_f)
        cpu_w = csv.writer(cpu_f)
        psi_w = csv.writer(psi_f)

        disk_w.writerow([
            "sample_start_ns", "sample_end_ns", "device",
            "sectors_read_delta", "read_bytes_delta", "read_bandwidth_mbs",
            "io_in_progress", "avg_queue_depth", "weighted_io_ms_delta"
        ])
        proc_w.writerow([
            "sample_start_ns", "sample_end_ns", "pid",
            "read_bytes", "write_bytes", "cancelled_write_bytes",
            "read_bytes_delta", "write_bytes_delta"
        ])
        cpu_w.writerow([
            "sample_start_ns", "sample_end_ns",
            "cpu_idle_percent", "cpu_iowait_percent",
            "total_jiffies_delta", "idle_jiffies_delta", "iowait_jiffies_delta"
        ])
        psi_w.writerow([
            "sample_start_ns", "sample_end_ns",
            "psi_io_some_total_us", "psi_io_full_total_us",
            "psi_io_some_delta_us", "psi_io_full_delta_us"
        ])

        while True:
            if args.duration_s > 0 and (time.time() - start_wall) >= args.duration_s:
                break
            if args.duration_s <= 0 and not pid_alive(args.pid):
                break

            time.sleep(interval_s)

            t_ns = time.monotonic_ns()
            elapsed_s = max(1e-9, (t_ns - prev_t_ns) / 1e9)
            elapsed_ms = elapsed_s * 1000.0

            cur_disk = read_diskstats(args.device)
            cur_cpu = read_cpu_stat()
            cur_psi = read_psi_io()
            cur_proc = read_proc_io(args.pid) if pid_alive(args.pid) else None

            delta_sectors = cur_disk.sectors_read - prev_disk.sectors_read
            read_bytes_delta = delta_sectors * SECTOR_SIZE
            read_mbs = clamp_non_negative((read_bytes_delta / MB) / elapsed_s)
            weighted_delta = cur_disk.weighted_io_ms - prev_disk.weighted_io_ms
            avg_qd = clamp_non_negative(weighted_delta / max(1e-9, elapsed_ms))

            total_jiffies_delta = cur_cpu.total - prev_cpu.total
            idle_jiffies_delta = cur_cpu.idle - prev_cpu.idle
            iowait_jiffies_delta = cur_cpu.iowait - prev_cpu.iowait
            cpu_idle_percent = 0.0
            cpu_iowait_percent = 0.0
            if total_jiffies_delta > 0:
                cpu_idle_percent = clamp_non_negative(idle_jiffies_delta * 100.0 / total_jiffies_delta)
                cpu_iowait_percent = clamp_non_negative(iowait_jiffies_delta * 100.0 / total_jiffies_delta)

            psi_some_delta = cur_psi.some_total_us - prev_psi.some_total_us
            psi_full_delta = cur_psi.full_total_us - prev_psi.full_total_us

            proc_read_delta = 0
            proc_write_delta = 0
            if cur_proc is not None and prev_proc is not None:
                proc_read_delta = cur_proc.read_bytes - prev_proc.read_bytes
                proc_write_delta = cur_proc.write_bytes - prev_proc.write_bytes

            disk_w.writerow([
                prev_t_ns, t_ns, args.device,
                delta_sectors, int(read_bytes_delta), f"{read_mbs:.6f}",
                cur_disk.ios_in_progress, f"{avg_qd:.6f}", weighted_delta
            ])
            cpu_w.writerow([
                prev_t_ns, t_ns,
                f"{cpu_idle_percent:.6f}", f"{cpu_iowait_percent:.6f}",
                total_jiffies_delta, idle_jiffies_delta, iowait_jiffies_delta
            ])
            psi_w.writerow([
                prev_t_ns, t_ns,
                cur_psi.some_total_us, cur_psi.full_total_us,
                psi_some_delta, psi_full_delta
            ])
            if cur_proc is not None:
                proc_w.writerow([
                    prev_t_ns, t_ns, args.pid,
                    cur_proc.read_bytes, cur_proc.write_bytes, cur_proc.cancelled_write_bytes,
                    proc_read_delta, proc_write_delta
                ])
                last_proc = cur_proc

            total_read_bytes += read_bytes_delta
            total_elapsed_s += elapsed_s
            queue_depth_sum += cur_disk.ios_in_progress
            queue_depth_peak = max(queue_depth_peak, float(cur_disk.ios_in_progress))
            cpu_idle_sum += cpu_idle_percent
            cpu_iowait_sum += cpu_iowait_percent
            sample_count += 1
            psi_some_delta_total += psi_some_delta
            psi_full_delta_total += psi_full_delta
            weighted_io_delta_total += weighted_delta

            prev_t_ns = t_ns
            prev_disk = cur_disk
            prev_cpu = cur_cpu
            prev_psi = cur_psi
            prev_proc = cur_proc if cur_proc is not None else prev_proc

    avg_bw = (total_read_bytes / MB) / total_elapsed_s if total_elapsed_s > 0 else 0.0
    read_util = 0.0
    if args.device_max_read_mbps > 0:
        read_util = avg_bw / args.device_max_read_mbps
    avg_qd_instant = (queue_depth_sum / sample_count) if sample_count > 0 else 0.0
    avg_qd_weighted = (weighted_io_delta_total / (total_elapsed_s * 1000.0)) if total_elapsed_s > 0 else 0.0
    avg_idle = (cpu_idle_sum / sample_count) if sample_count > 0 else 0.0
    avg_iowait = (cpu_iowait_sum / sample_count) if sample_count > 0 else 0.0
    proc_read_delta_total = 0
    if first_proc is not None and last_proc is not None:
        proc_read_delta_total = last_proc.read_bytes - first_proc.read_bytes

    with open(summary_path, "w", encoding="utf-8") as f:
        f.write("=== I/O Monitor Summary ===\n")
        f.write(f"duration_s: {total_elapsed_s:.3f}\n")
        f.write(f"read_bandwidth_mbs: {avg_bw:.3f}\n")
        f.write(f"device_baseline_read_mbps: {args.device_max_read_mbps:.3f}\n")
        f.write(f"read_bandwidth_utilization: {read_util:.6f}\n")
        f.write(f"instant_queue_depth_avg: {avg_qd_instant:.3f}\n")
        f.write(f"avg_queue_depth_from_weighted_io_time: {avg_qd_weighted:.3f}\n")
        f.write(f"peak_queue_depth: {int(queue_depth_peak)}\n")
        f.write(f"process_read_bytes_delta: {proc_read_delta_total}\n")
        f.write(f"cpu_idle_percent: {avg_idle:.3f}\n")
        f.write(f"cpu_iowait_percent: {avg_iowait:.3f}\n")
        f.write(f"psi_io_some_delta_us: {psi_some_delta_total}\n")
        f.write(f"psi_io_full_delta_us: {psi_full_delta_total}\n")
        f.write(f"sample_count: {sample_count}\n")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
