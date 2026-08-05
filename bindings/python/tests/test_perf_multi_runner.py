import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PERF_DIR = ROOT / "perf"


class PerfMultiRunnerTests(unittest.TestCase):
    def test_top_level_multi_wrapper_help(self):
        result = subprocess.run(
            [str(PERF_DIR / "run_benchmarks_multi.sh"), "--help"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("usage: run_benchmarks_multi.sh", result.stdout)
        self.assertIn("--clients", result.stdout)
        self.assertIn("--msg-sizes", result.stdout)

    def test_multi_runner_help(self):
        result = subprocess.run(
            [str(PERF_DIR / "multi" / "run_benchmarks.sh"), "--help"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("usage: run_benchmarks_multi.sh", result.stdout)
        self.assertIn("--clients", result.stdout)
        self.assertIn("--results-dir", result.stdout)
