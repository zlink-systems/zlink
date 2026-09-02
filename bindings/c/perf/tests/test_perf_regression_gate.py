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


def report(
    metrics=None,
    *,
    sizes=GATE.REQUIRED_SIZES,
    per_size=None,
    skip=0,
    fail=0,
    status="complete",
    duplicate=False,
):
    lines = []
    for size in sizes:
        values = dict(METRICS)
        values.update(metrics or {})
        values.update((per_size or {}).get(size, {}))
        lines.extend(
            f"RESULT,current,PAIR,tcp,{size},{metric},{value}"
            for metric, value in values.items()
        )
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
    def run_gate(self, baseline, candidate, *, suite="single"):
        baselines = [baseline] if isinstance(baseline, str) else baseline
        candidates = [candidate] if isinstance(candidate, str) else candidate
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            argv = []
            for index, contents in enumerate(baselines):
                path = root / f"baseline-{index}.txt"
                path.write_text(contents, encoding="utf-8")
                argv.extend([f"--baseline-{suite}", str(path)])
            for index, contents in enumerate(candidates):
                path = root / f"candidate-{index}.txt"
                path.write_text(contents, encoding="utf-8")
                argv.extend([f"--candidate-{suite}", str(path)])
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                exit_code = GATE.main(argv)
        return exit_code, output.getvalue()

    def test_passes_each_metric_at_the_five_percent_boundary(self):
        candidate = report(
            per_size={
                64: {
                    "throughput": 95.0,
                    "bandwidth": 9.5,
                    "latency": 2.1,
                    "latency_p95": 3.15,
                    "latency_p99": 4.2,
                },
                256: {
                    "throughput": 102.0,
                    "bandwidth": 10.2,
                    "latency": 1.96,
                    "latency_p95": 2.94,
                    "latency_p99": 3.92,
                },
                1024: {
                    "throughput": 102.0,
                    "bandwidth": 10.2,
                    "latency": 1.96,
                    "latency_p95": 2.94,
                    "latency_p99": 3.92,
                },
                65536: {
                    "throughput": 102.0,
                    "bandwidth": 10.2,
                    "latency": 1.96,
                    "latency_p95": 2.94,
                    "latency_p99": 3.92,
                },
            }
        )
        exit_code, output = self.run_gate(report(), candidate)
        self.assertEqual(exit_code, 0)
        self.assertIn("Final: PASS", output)
        self.assertEqual(output.count("| single | PAIR | tcp | 1024 |"), 5)
        self.assertIn("Aggregate verdicts (geometric mean", output)

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

    def test_fails_aggregate_when_cells_pass_but_geomean_regresses(self):
        candidate = report(per_size={64: {"throughput": 95.0}})
        exit_code, output = self.run_gate(report(), candidate)
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL aggregate-regression", output)
        self.assertIn("failed_cells=0", output)
        self.assertIn("failed_aggregates=1", output)

    def test_fails_aggregate_when_a_required_size_is_missing(self):
        incomplete = report(sizes=(64, 256, 1024))
        exit_code, output = self.run_gate(incomplete, incomplete)
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL missing sizes=65536", output)

    def test_aggregate_uses_geometric_mean(self):
        candidate = report(
            per_size={
                64: {"throughput": 95.0},
                256: {"throughput": 95.0},
                1024: {"throughput": 105.0},
                65536: {"throughput": 105.0},
            }
        )
        exit_code, output = self.run_gate(report(), candidate)
        self.assertEqual(exit_code, 1)
        self.assertIn(
            "| single | PAIR | tcp | throughput | 0.9500 | 0.9500 | "
            "1.0500 | 1.0500 | 0.9987 |",
            output,
        )

    def test_accepts_repeated_report_arguments_for_multi(self):
        baseline = [report(sizes=(64, 256)), report(sizes=(1024, 65536))]
        candidate = [report(sizes=(64, 256)), report(sizes=(1024, 65536))]
        exit_code, output = self.run_gate(baseline, candidate, suite="multi")
        self.assertEqual(exit_code, 0)
        self.assertIn("| multi | PAIR | tcp | throughput |", output)
        self.assertIn("Final: PASS", output)

    def test_existing_single_report_argument_form_remains_valid(self):
        args = GATE.parse_args(
            [
                "--baseline-single",
                "baseline.txt",
                "--candidate-single",
                "candidate.txt",
            ]
        )
        self.assertEqual(args.baseline_single, [pathlib.Path("baseline.txt")])
        self.assertEqual(args.candidate_single, [pathlib.Path("candidate.txt")])

    def test_help_names_repeatable_inputs_and_geometric_mean(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(SystemExit):
            GATE.parse_args(["--help"])
        self.assertIn("geometric mean", output.getvalue())
        self.assertIn("Each report option is repeatable", output.getvalue())

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
