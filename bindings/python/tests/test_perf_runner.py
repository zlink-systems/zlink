import subprocess
import sys
import unittest
from pathlib import Path
import importlib.util


ROOT = Path(__file__).resolve().parents[1]
PERF_DIR = ROOT / "perf"
METRICS_PATH = PERF_DIR / "perf_metrics.py"
METRICS_SPEC = importlib.util.spec_from_file_location("python_perf_metrics", METRICS_PATH)
METRICS = importlib.util.module_from_spec(METRICS_SPEC)
assert METRICS_SPEC and METRICS_SPEC.loader
METRICS_SPEC.loader.exec_module(METRICS)


class PerfRunnerTests(unittest.TestCase):
    def test_reference_result_fields_do_not_enter_metric_rows(self):
        rows = METRICS.parse_result_lines(
            "\n".join(
                (
                    "RESULT,current,PUBSUB,tcp,256,throughput,1000",
                    "RESULT,current,PUBSUB,tcp,256,latency_ref_p50,1.2",
                    "RESULT,current,PUBSUB,tcp,256,latency_ref_p99,4.2",
                    "RESULT,current,PUBSUB,tcp,256,inflight_floor,8",
                    "RESULT,current,PUBSUB,tcp,256,inflight_floor_ms,0.8",
                    "RESULT,current,PUBSUB,tcp,256,sender_late,3",
                )
            )
        )
        self.assertEqual([row["metric"] for row in rows], ["throughput"])

    def test_single_runner_help(self):
        runner = (
            PERF_DIR / "single" / "run_benchmarks.py"
            if sys.platform == "win32"
            else PERF_DIR / "single" / "run_benchmarks.sh"
        )
        result = subprocess.run(
            [sys.executable, str(runner), "--help"]
            if sys.platform == "win32"
            else [str(runner), "--help"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0)
        self.assertRegex(result.stdout, r"usage: run_benchmarks\.(sh|py)")
        self.assertIn("--msg-sizes", result.stdout)
        self.assertIn("--results-dir", result.stdout)
