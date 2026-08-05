import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PERF_DIR = ROOT / "perf"


class PerfRunnerTests(unittest.TestCase):
    def test_single_runner_help(self):
        result = subprocess.run(
            [str(PERF_DIR / "single" / "run_benchmarks.sh"), "--help"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("usage: run_benchmarks.sh", result.stdout)
        self.assertIn("--msg-sizes", result.stdout)
        self.assertIn("--results-dir", result.stdout)
