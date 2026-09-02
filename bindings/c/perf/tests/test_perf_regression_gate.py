import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "perf_regression_gate.py"
SPEC = importlib.util.spec_from_file_location("c_perf_regression_gate", MODULE_PATH)
GATE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


METRICS = {
    "throughput": 100.0,
    "bandwidth": 10.0,
    "latency": 2.0,
    "latency_p95": 3.0,
    "latency_p99": 4.0,
}


def report(metrics=None, *, skip=0, fail=0, status="complete", duplicate=False):
    values = dict(METRICS)
    values.update(metrics or {})
    lines = [
        f"RESULT,current,PAIR,tcp,1024,{metric},{value}"
        for metric, value in values.items()
    ]
    if duplicate:
        lines.append("RESULT,current,PAIR,tcp,1024,throughput,100.0")
    lines.extend(
        [
            "## Completion",
            f"- skip: {skip}",
            f"- fail: {fail}",
            f"- status: {status}",
        ]
    )
    return "\n".join(lines) + "\n"


class PerfRegressionGateTests(unittest.TestCase):
    def run_gate(self, baseline, candidate):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            baseline_path = root / "baseline.txt"
            candidate_path = root / "candidate.txt"
            baseline_path.write_text(baseline, encoding="utf-8")
            candidate_path.write_text(candidate, encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                exit_code = GATE.main(
                    [
                        "--baseline-single",
                        str(baseline_path),
                        "--candidate-single",
                        str(candidate_path),
                    ]
                )
        return exit_code, output.getvalue()

    def test_passes_each_metric_at_the_five_percent_boundary(self):
        candidate = report(
            {
                "throughput": 95.0,
                "bandwidth": 9.5,
                "latency": 2.1,
                "latency_p95": 3.15,
                "latency_p99": 4.2,
            }
        )
        exit_code, output = self.run_gate(report(), candidate)
        self.assertEqual(exit_code, 0)
        self.assertIn("Final: PASS", output)
        self.assertEqual(output.count("| single | PAIR | tcp | 1024 |"), 5)

    def test_fails_a_throughput_regression(self):
        exit_code, output = self.run_gate(report(), report({"throughput": 94.9}))
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL regression", output)

    def test_fails_zero_baseline_cell(self):
        exit_code, output = self.run_gate(report({"throughput": 0.0}), report())
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL baseline-zero", output)

    def test_fails_missing_candidate_cell(self):
        candidate = report().replace(
            "RESULT,current,PAIR,tcp,1024,latency_p99,4.0\n", ""
        )
        exit_code, output = self.run_gate(report(), candidate)
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL missing-candidate", output)

    def test_fails_duplicate_cell(self):
        exit_code, output = self.run_gate(report(), report(duplicate=True))
        self.assertEqual(exit_code, 1)
        self.assertIn("duplicate cell PAIR/tcp/1024/throughput", output)

    def test_fails_unexpected_skip(self):
        exit_code, output = self.run_gate(report(), report(skip=1))
        self.assertEqual(exit_code, 1)
        self.assertIn("unexpected skip count: 1", output)


if __name__ == "__main__":
    unittest.main()
