import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PERF_DIR = ROOT / "perf"


class PerfMultiRunnerTests(unittest.TestCase):
    def test_top_level_multi_wrapper_help(self):
        runner = (
            PERF_DIR / "multi" / "run_benchmarks.py"
            if sys.platform == "win32"
            else PERF_DIR / "run_benchmarks_multi.sh"
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
        self.assertRegex(result.stdout, r"usage: run_benchmarks_multi\.(sh|py)")
        self.assertIn("--clients", result.stdout)
        self.assertIn("--msg-sizes", result.stdout)

    def test_multi_runner_help(self):
        runner = (
            PERF_DIR / "multi" / "run_benchmarks.py"
            if sys.platform == "win32"
            else PERF_DIR / "multi" / "run_benchmarks.sh"
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
        self.assertRegex(result.stdout, r"usage: run_benchmarks_multi\.(sh|py)")
        self.assertIn("--clients", result.stdout)
        self.assertIn("--results-dir", result.stdout)
