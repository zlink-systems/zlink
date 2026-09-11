"""C++ target rejection accounting and compatibility with existing readers."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from benchagg.readers import ReportError, cells_from_cell_json  # noqa: E402

RUNNER = (TOOLS.parent / "cpp" / "run_local.sh").read_text()


def runner_python(function: str, *args: object) -> subprocess.CompletedProcess:
    # Run the real runner's Python body without starting benchmark processes.
    body = RUNNER.split(f"{function}() {{", 1)[1].split("<<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
    return subprocess.run(
        [sys.executable, "-", *(str(arg) for arg in args)],
        input=body, text=True, capture_output=True, check=False,
    )


class CppRejectionTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.result = Path(self.directory.name) / "cells.json"
        self.target = Path(self.directory.name) / "target.json"
        self.cell = {
            "implementation": "zlink-framework-cpp", "pattern": "send-saturation",
            "payload_size": 1024, "submitted": 8, "completed": 8,
            "errors": 0, "abandoned": 0, "server_received_at_close": 4,
            "client_error_summary": [], "client_error_other_count": 0,
            "target_stats": {"received": 5, "errors": 0, "drainMs": 0},
            "throughput_per_second": 4, "bandwidth_mb_s": 0.004096,
            "latency_mean_ms": 1, "latency_p95_ms": 2, "latency_p99_ms": 3,
            "client_cpu_percent": 1, "client_memory_mb": 10,
            "server_cpu_percent": 2, "server_memory_mb": 20,
        }

    def write(self):
        self.result.write_text(json.dumps({"schema": "with-grpc-cell-v1", "cells": [self.cell]}))

    def verify(self, succeeds: bool):
        self.write()
        result = runner_python("verify_counts", self.result)
        self.assertEqual(result.returncode == 0, succeeds, result.stdout + result.stderr)
        return result

    def test_send_requires_observed_rejections_to_explain_difference(self):
        self.verify(False)
        self.cell["server_rejected_count"] = 3
        result = self.verify(True)
        self.assertIn("server_rejected_count=3 difference=3", result.stdout)
        for rejected in (-1, 2, 4, 9):
            with self.subTest(rejected=rejected):
                self.cell["server_rejected_count"] = rejected
                self.verify(False)

    def test_requests_cannot_use_send_drops_to_hide_missing_replies(self):
        self.cell.update(pattern="request-serial", server_rejected_count=3)
        self.verify(False)

    def test_unknown_count_is_preserved_and_rejected_even_when_received_matches(self):
        for target_count in ({}, {"rejected": None}):
            with self.subTest(target_count=target_count):
                self.cell["target_stats"]["received"] = 8
                self.write()
                self.target.write_text(json.dumps({"snapshot": {
                    "received": 8, "errors": 0, **target_count,
                }}))
                result = runner_python("merge_target_stats", self.result, self.target, 20, "false")
                self.assertEqual(result.returncode, 0, result.stderr)
                document = json.loads(self.result.read_text())
                cell = document["cells"][0]
                self.assertIsNone(cell["server_rejected_count"])
                self.assertIsNone(cell["target_stats"]["rejected"])
                result = runner_python("verify_counts", self.result)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("target rejection count unavailable", result.stderr)
                with self.assertRaisesRegex(ReportError, "target rejection count unavailable"):
                    cells_from_cell_json(document, "test")

    def test_missing_result_count_is_not_zero(self):
        self.cell["target_stats"]["received"] = 8
        result = self.verify(False)
        self.assertIn("target rejection count unavailable", result.stderr)

    def test_requests_validate_receipts_without_a_send_drop_count(self):
        self.cell.update(pattern="request-serial", server_rejected_count=None)
        self.cell["target_stats"].update(received=8, rejected=None)
        self.verify(True)
        cells_from_cell_json(json.loads(self.result.read_text()), "test")
        self.cell["target_stats"]["received"] = 7
        self.verify(False)

    def test_legacy_zero_drop_result_and_source_error_equation(self):
        self.cell["target_stats"]["received"] = 8
        self.cell["server_rejected_count"] = 0
        self.verify(True)
        self.cell["submitted"] = 9
        self.verify(False)

    def test_merge_adds_count_without_changing_existing_measurements_or_result_lines(self):
        self.write()
        before = runner_python("emit_final_results", self.result)
        self.assertEqual(before.returncode, 0, before.stderr)
        self.target.write_text(json.dumps({"snapshot": {"received": 5, "errors": 0, "rejected": 3}}))
        result = runner_python("merge_target_stats", self.result, self.target, 20, "false")
        self.assertEqual(result.returncode, 0, result.stderr)
        document = json.loads(self.result.read_text())
        cell = document["cells"][0]
        self.assertEqual(cell["server_rejected_count"], 3)
        self.assertEqual(cell["target_stats"]["rejected"], 3)
        for key in self.cell.keys() - {"target_stats"}:
            self.assertEqual(cell[key], self.cell[key], key)
        self.assertEqual(runner_python("verify_counts", self.result).returncode, 0)
        after = runner_python("emit_final_results", self.result)
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertEqual(after.stdout, before.stdout)
        self.assertEqual(len(after.stdout.splitlines()), 9)
        for line in after.stdout.splitlines():
            self.assertEqual(len(line.split(",")), 7)
        legacy = copy.deepcopy(document)
        del legacy["cells"][0]["server_rejected_count"]
        del legacy["cells"][0]["target_stats"]["rejected"]
        self.assertEqual(cells_from_cell_json(document, "test"), cells_from_cell_json(legacy, "test"))

    def test_compare_results_accepts_additive_diagnostics(self):
        fixture = TOOLS / "tests" / "fixtures" / "compare-results" / "complete"
        candidate = Path(self.directory.name) / "comparison"
        shutil.copytree(fixture, candidate)
        def compare(root):
            result = subprocess.run(
                [sys.executable, str(TOOLS / "compare-results.py"), str(root / "before"), str(root / "after")],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout
        before = compare(candidate)
        for path in candidate.rglob("cells.json"):
            document = json.loads(path.read_text())
            for cell in document["cells"]:
                cell["client_error_summary"] = []
                cell["client_error_other_count"] = 0
                cell["server_rejected_count"] = 17
            path.write_text(json.dumps(document))
        self.assertEqual(compare(candidate), before)


if __name__ == "__main__":
    unittest.main()
