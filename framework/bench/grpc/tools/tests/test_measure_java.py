#!/usr/bin/env python3
"""Checks for result/JFR identity and final-recording collection decisions."""

from copy import deepcopy
import importlib.util
from datetime import datetime, timedelta, timezone
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

RUNNER_PATH = Path(__file__).resolve().parents[1] / "jit/measure_java.py"
spec = importlib.util.spec_from_file_location("measure_java", RUNNER_PATH)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class WindowIdentityTest(unittest.TestCase):
    def setUp(self):
        self.window = {
            "window_index": 7, "cell_id": "send-window-0007", "warmup": False,
            "metric": {"active_started_at_ns": 1_000, "active_ended_at_ns": 5_000_001_000},
        }
        candidate = {"window_index": 7, "cell_id": "send-window-0007", "start_ns": 1_000,
                     "end_ns": 5_000_001_000, "clean": True}
        self.summary = {"candidate_count": 1, "candidate_windows": [candidate],
                        "selected_windows": [candidate]}

    def test_matching_actual_boundaries_pass(self):
        runner.validate_windows(self.summary, [self.window])

    def test_wrong_start_end_identity_and_index_fail(self):
        for key, value in (("start_ns", 1_001), ("end_ns", 5_000_001_001),
                           ("cell_id", "other"), ("window_index", 8)):
            with self.subTest(key=key):
                summary = deepcopy(self.summary)
                summary["candidate_windows"][0][key] = value
                with self.assertRaises(ValueError):
                    runner.validate_windows(summary, [self.window])

    def test_missing_or_extra_marker_fails(self):
        for candidates in ([], self.summary["candidate_windows"] * 2):
            summary = dict(self.summary, candidate_windows=candidates,
                           candidate_count=len(candidates))
            with self.assertRaises(ValueError):
                runner.validate_windows(summary, [self.window])

    def test_discarded_marker_is_also_checked(self):
        summary = deepcopy(self.summary)
        summary["selected_windows"] = []
        summary["candidate_windows"][0].update(clean=False, start_ns=9_999)
        with self.assertRaises(ValueError):
            runner.validate_windows(summary, [self.window])

    def test_selected_window_must_be_clean_and_a_candidate(self):
        summary = deepcopy(self.summary)
        summary["selected_windows"] = [dict(summary["selected_windows"][0], clean=False)]
        with self.assertRaises(ValueError):
            runner.validate_windows(summary, [self.window])


class FinalRecordingCollectionTest(unittest.TestCase):
    def test_final_clean_windows_survive_fresh_jvm_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.commit").write_text("exact-commit\n")
            (root / "main.cp").write_text("products.jar\n")
            args = SimpleNamespace(output=root / "results", classpath_dir=root,
                harness_classpath="classes", deadline=datetime.now(timezone.utc) + timedelta(hours=1))
            first = {"selected": [{"attempt": 1}], "discardedCount": 2, "candidateCount": 3}
            second = {"selected": [{"attempt": 2}, {"attempt": 2}],
                      "discardedCount": 1, "candidateCount": 3}
            with patch.object(runner, "run_attempt", side_effect=[first, second]) as run:
                result = runner.run_condition(args, "main", "send-saturation", "zlink-java", {"native"})
            self.assertTrue(result["complete"])
            self.assertEqual(result["selected"], first["selected"] + second["selected"])
            self.assertEqual(result["discardedCount"], 3)
            self.assertEqual([call.args[-2] for call in run.call_args_list], [3, 2])

    def test_external_failure_is_not_retried(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.commit").write_text("exact-commit\n")
            (root / "main.cp").write_text("products.jar\n")
            args = SimpleNamespace(output=root / "results", classpath_dir=root,
                harness_classpath="classes", deadline=datetime.now(timezone.utc) + timedelta(hours=1))
            with patch.object(runner, "run_attempt", side_effect=RuntimeError("HTTP failure")) as run:
                with self.assertRaises(RuntimeError):
                    runner.run_condition(args, "main", "send-saturation", "zlink-java", {"native"})
            self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
