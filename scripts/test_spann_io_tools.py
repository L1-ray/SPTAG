#!/usr/bin/env python3
import os
import subprocess
import tempfile
import unittest


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


class SpannIOToolsTest(unittest.TestCase):
    @staticmethod
    def _detect_device() -> str:
        with open("/proc/diskstats", "r", encoding="utf-8") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) < 3:
                    continue
                name = parts[2]
                if name.startswith("loop") or name.startswith("ram"):
                    continue
                return name
        return "sda"

    def test_analyze_fallback_with_legacy_log(self) -> None:
        report_path = os.path.join(tempfile.gettempdir(), "spann_io_report_test.md")
        log_path = os.path.join(REPO_ROOT, "results", "spann_search_16t_nocache.log")
        cmd = [
            "python3",
            os.path.join(REPO_ROOT, "scripts", "analyze_spann_io.py"),
            "--sptag-log",
            log_path,
            "--output-report",
            report_path,
        ]
        subprocess.check_call(cmd, cwd=REPO_ROOT)
        self.assertTrue(os.path.exists(report_path))
        with open(report_path, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("SPANN 搜索 I/O 性能分析报告", content)

    def test_monitor_runs_short_duration(self) -> None:
        out_dir = tempfile.mkdtemp(prefix="spann_io_monitor_test_")
        device = self._detect_device()
        cmd = [
            "python3",
            os.path.join(REPO_ROOT, "scripts", "spann_io_monitor.py"),
            "--device",
            device,
            "--pid",
            str(os.getpid()),
            "--interval-ms",
            "200",
            "--duration-s",
            "1.0",
            "--output-dir",
            out_dir,
        ]
        subprocess.check_call(cmd, cwd=REPO_ROOT)
        self.assertTrue(os.path.exists(os.path.join(out_dir, "disk_stats.csv")))
        self.assertTrue(os.path.exists(os.path.join(out_dir, "summary.txt")))


if __name__ == "__main__":
    unittest.main()
