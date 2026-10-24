#!/usr/bin/env python3
"""Checks for result/JFR identity and final-recording collection decisions."""

from copy import deepcopy
import importlib.util
import json
import os
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
                harness_classpath="classes", deadline=datetime.now(timezone.utc) + timedelta(hours=1),
                version_inputs={"main": {"commit": "exact-commit", "classpath": "classes:products.jar"}})
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
                harness_classpath="classes", deadline=datetime.now(timezone.utc) + timedelta(hours=1),
                version_inputs={"main": {"commit": "exact-commit", "classpath": "classes:products.jar"}})
            with patch.object(runner, "run_attempt", side_effect=RuntimeError("HTTP failure")) as run:
                with self.assertRaises(RuntimeError):
                    runner.run_condition(args, "main", "send-saturation", "zlink-java", {"native"})
            self.assertEqual(run.call_count, 1)


class ImmutableInputTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.classes = self.root / "classes"
        self.classes.mkdir()
        (self.classes / "Harness.class").write_bytes(b"harness bytecode")
        self.core = self.root / "core.jar"
        self.core.write_bytes(b"core artifact")
        self.helper = self.root / "helper.jar"
        self.helper.write_bytes(b"helper artifact")
        self.alias = self.root / "core-link.jar"
        self.alias.symlink_to(self.core)
        self.cp = self.root / "main.cp"
        self.cp.write_text(os.pathsep.join(map(str, (self.alias, self.helper))) + "\n")
        self.commit = self.root / "main.commit"
        self.commit.write_text("exact-commit\n")
        self.manifest = {"versions": {"main": "exact-commit"}, "jars": {"main": {
            path.name: {"path": str(path), "sha256": runner.digest(path)}
            for path in (self.core, self.helper)}}}
        self.manifest_path = self.root / "artifact-manifest.json"
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.tool = self.root / "tool.py"
        self.tool.write_text("frozen tool")
        self.args = SimpleNamespace(versions=["main"], classpath_dir=self.root,
            harness_classpath=str(self.classes), tool_hashes={str(self.tool): runner.digest(self.tool)})
        runner.freeze_inputs(self.args, self.manifest_path, self.manifest)
        self.classpath = self.args.version_inputs["main"]["classpath"]

    def check(self):
        return runner.verify_inputs(self.args, "main", self.classpath)

    def test_frozen_inputs_pass(self):
        actual = self.check()
        self.assertEqual(set(actual), {str(self.core), str(self.helper), str(self.classes / "Harness.class")})

    def test_added_shadow_jar_fails(self):
        shadow = self.root / "shadow.jar"
        shadow.write_bytes(b"shadow class")
        self.cp.write_text(self.cp.read_text().strip() + os.pathsep + str(shadow))
        with self.assertRaises(ValueError):
            self.check()

    def test_removed_jar_fails(self):
        self.cp.write_text(str(self.alias))
        with self.assertRaises(ValueError):
            self.check()

    def test_reordered_jars_fail(self):
        self.cp.write_text(os.pathsep.join(map(str, (self.helper, self.alias))) + "\n")
        with self.assertRaises(ValueError):
            self.check()

    def test_moved_jar_with_identical_bytes_fails(self):
        moved = self.root / "moved.jar"
        self.core.rename(moved)
        self.alias.unlink()
        self.alias.symlink_to(moved)
        with self.assertRaises(ValueError):
            self.check()

    def test_commit_replacement_fails(self):
        self.commit.write_text("different-commit\n")
        with self.assertRaises(ValueError):
            self.check()

    def test_manifest_replacement_fails(self):
        self.manifest_path.write_text(self.manifest_path.read_text() + "\n")
        with self.assertRaises(ValueError):
            self.check()

    def test_product_bytes_replacement_fails(self):
        self.core.write_bytes(b"different bytecode")
        with self.assertRaises(ValueError):
            self.check()

    def test_shadow_class_added_to_harness_fails(self):
        (self.classes / "Shadow.class").write_bytes(b"unexpected class")
        with self.assertRaises(ValueError):
            self.check()

    def test_relative_classpath_is_rejected(self):
        self.cp.write_text("core.jar" + os.pathsep + str(self.helper))
        with self.assertRaisesRegex(ValueError, "absolute"):
            runner.freeze_inputs(self.args, self.manifest_path, self.manifest)


if __name__ == "__main__":
    unittest.main()
