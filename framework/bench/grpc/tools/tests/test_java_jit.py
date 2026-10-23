"""Compilation-free selection must reject incomplete evidence and both overlap directions."""

import copy
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("analyze_java_jit", TOOLS / "jit" / "analyze_java_jit.py")
JIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(JIT)
SECOND = 1_000_000_000
ORIGIN = 1_800_000_000_000_000_000
MONOTONIC = 100 * SECOND


def recording(windows=9, extras=(), uncertainty=0):
    events = [
        {"type": "setting", "event": "jdk.Compilation", "name": "enabled", "value": "true"},
        {"type": "setting", "event": "jdk.Compilation", "name": "threshold", "value": "0 ns"},
        {"type": "setting", "event": "jdk.DataLoss", "name": "enabled", "value": "true"},
        {"type": "anchor", "timestamp_ns": ORIGIN, "lower_ns": MONOTONIC,
         "upper_ns": MONOTONIC + uncertainty},
        {"type": "phase", "phase": None, "cell_id": None, "start_ns": 0, "duration_ns": 0},
    ]
    events.extend({"type": "phase", "phase": "active", "cell_id": f"cell-{i + 1}",
                   "start_ns": MONOTONIC + (i + 1) * 6 * SECOND, "duration_ns": 5 * SECOND}
                  for i in range(windows))
    events.extend(copy.deepcopy(extras))
    return finish(events)


def finish(events):
    rows = [event for event in events if event["type"] != "end"]
    return rows + [{"type": "end", "records": len(rows)}]


def compilation(start, duration, compile_id=1):
    return {"type": "compilation", "timestamp_ns": ORIGIN + start - MONOTONIC,
            "duration_ns": duration, "compile_id": compile_id, "compiler": "c2",
            "method": "sample.Driver.run()V", "level": 4, "osr": True, "succeeded": True}


class JavaJitTest(unittest.TestCase):
    def analyze(self, source=None, target=None, **kwargs):
        return JIT.analyze_recordings(source or recording(), target or recording(0), **kwargs)

    def test_clean_windows_are_selected_after_thirty_seconds_active_warmup(self):
        result = self.analyze()
        self.assertTrue(result["valid"])
        self.assertEqual([7, 8, 9], [w["window_index"] for w in result["selected_windows"]])
        self.assertEqual(30 * SECOND, result["warmup_duration_ns"])

    def test_negative_monotonic_origin_is_valid(self):
        source = recording(extras=[compilation(MONOTONIC + 43 * SECOND, SECOND)])
        target = recording(0)
        for rows in (source, target):
            for event in rows:
                if event["type"] == "anchor":
                    event["lower_ns"] -= 200 * SECOND
                    event["upper_ns"] -= 200 * SECOND
                elif event["type"] == "phase" and event["phase"] is not None:
                    event["start_ns"] -= 200 * SECOND
        result = self.analyze(source, target)
        self.assertEqual(1, result["discarded_count"])
        self.assertLess(result["discarded_windows"][0]["start_ns"], 0)
        self.assertEqual(2, result["clean_count"])

    def test_variable_warmup_durations_preserve_thirty_second_requirement(self):
        rows = recording(0)[:-1]
        start = MONOTONIC + SECOND
        durations = [SECOND // 4] * 80 + [5 * SECOND] * 5
        for index, duration in enumerate(durations, 1):
            rows.append({"type": "phase", "phase": "active", "cell_id": f"cell-{index}",
                         "start_ns": start, "duration_ns": duration})
            start += duration + SECOND
        result = self.analyze(finish(rows), warmup_windows=82)
        self.assertTrue(result["valid"])
        self.assertEqual(30 * SECOND, result["warmup_duration_ns"])
        self.assertEqual([83, 84, 85], [window["window_index"] for window in result["selected_windows"]])
        rows[5]["duration_ns"] -= 1
        with self.assertRaisesRegex(JIT.JitAnalysisError, "warmup duration"):
            self.analyze(finish(rows), warmup_windows=82)

    def test_short_measurement_is_rejected_after_valid_warmup(self):
        rows = recording()
        rows[11]["duration_ns"] = SECOND // 4
        with self.assertRaisesRegex(JIT.JitAnalysisError, "Measurement duration"):
            self.analyze(rows)

    def test_contaminated_window_is_excluded_without_selecting_by_throughput(self):
        source = recording(11, [compilation(MONOTONIC + 43 * SECOND, SECOND)])
        result = self.analyze(source)
        self.assertTrue(result["valid"])
        self.assertEqual(1, result["discarded_count"])
        self.assertEqual([8, 9, 10], [w["window_index"] for w in result["selected_windows"]])
        event = result["discarded_windows"][0]["roles"]["source"]["compilations"][0]
        self.assertEqual((1, 4, True), (event["compile_id"], event["level"], event["osr"]))

    def test_compilation_crossing_either_boundary_is_rejected(self):
        start = MONOTONIC + 42 * SECOND
        for begin, duration in ((start - SECOND, 2 * SECOND),
                                (start + 4 * SECOND, 2 * SECOND),
                                (start - SECOND, 7 * SECOND)):
            with self.subTest(begin=begin):
                result = self.analyze(recording(extras=[compilation(begin, duration)]))
                self.assertFalse(result["valid"])
                self.assertEqual(1, result["discarded_count"])
                self.assertEqual(2, result["clean_count"])
        self.assertTrue(self.analyze()["valid"])

    def test_half_open_boundaries_without_uncertainty_do_not_overlap(self):
        start = MONOTONIC + 42 * SECOND
        source = recording(extras=[compilation(start - SECOND, SECOND),
                                   compilation(start + 5 * SECOND, SECOND, 2)])
        self.assertTrue(self.analyze(source)["valid"])

    def test_target_anchor_uncertainty_is_included(self):
        start = MONOTONIC + 42 * SECOND
        event = compilation(start - SECOND - 1, SECOND)
        self.assertTrue(self.analyze(target=recording(0, [event]))["valid"])
        result = self.analyze(target=recording(0, [event], uncertainty=2))
        self.assertFalse(result["valid"])
        self.assertEqual(1, result["discarded_windows"][0]["roles"]["target"]["count"])

    def test_failed_compilation_is_still_contamination(self):
        event = compilation(MONOTONIC + 43 * SECOND, SECOND)
        event["succeeded"] = False
        self.assertFalse(self.analyze(recording(extras=[event]))["valid"])

    def test_deoptimization_is_reported_separately(self):
        event = {"type": "deoptimization", "timestamp_ns": ORIGIN + 43 * SECOND,
                 "compile_id": 42, "method": "sample.Driver.run()V", "bci": 8,
                 "reason": "unstable_if", "action": "reinterpret"}
        result = self.analyze(recording(extras=[event]))
        self.assertTrue(result["valid"])
        self.assertEqual("unstable_if", result["selected_windows"][0]["roles"]["source"]
                         ["deoptimizations"][0]["reason"])

    def test_data_loss_in_either_recording_fails_closed(self):
        bad = recording(extras=[{"type": "data_loss"}])
        for source, target in ((bad, recording(0)), (recording(), bad)):
            with self.assertRaisesRegex(JIT.JitAnalysisError, "DataLoss"):
                self.analyze(source, target)

    def test_missing_and_disabled_recording_settings_fail_closed(self):
        for event_name, setting_name, bad_value in (("jdk.Compilation", "enabled", "false"),
                                                   ("jdk.Compilation", "threshold", "1 ms"),
                                                   ("jdk.DataLoss", "enabled", "false")):
            rows = recording()
            for event in rows:
                if event.get("event") == event_name and event.get("name") == setting_name:
                    event["value"] = bad_value
            with self.subTest(event=event_name, setting=setting_name):
                with self.assertRaises(JIT.JitAnalysisError):
                    self.analyze(rows)
        with self.assertRaises(JIT.JitAnalysisError):
            self.analyze(finish([e for e in recording() if e["type"] != "setting"]))

    def test_missing_anchor_or_measurements_fail_closed(self):
        for rows in (finish([e for e in recording() if e["type"] != "anchor"]), recording(6)):
            with self.assertRaises(JIT.JitAnalysisError):
                self.analyze(rows)

    def test_truncated_duplicate_and_negative_evidence_fail_closed(self):
        baseline = recording()
        variants = [baseline[:-1], baseline + [baseline[-1]], finish(baseline + [baseline[5]])]
        # Duplicate active window, reversed anchor, negative active time, and malformed registration.
        for key, index, value in (("upper_ns", 3, MONOTONIC - 1),
                                  ("start_ns", 5, -1), ("duration_ns", 5, 0),
                                  ("start_ns", 4, 1), ("duration_ns", 5, -1),
                                  ("phase", 4, ""), ("cell_id", 5, None)):
            rows = copy.deepcopy(baseline)
            rows[index][key] = value
            variants.append(rows)
        for rows in variants:
            with self.assertRaises(JIT.JitAnalysisError):
                self.analyze(rows)
        event = compilation(MONOTONIC + 43 * SECOND, SECOND)
        with self.assertRaisesRegex(JIT.JitAnalysisError, "duplicate compilation"):
            self.analyze(recording(extras=[event, event]))

    def test_exact_duration_and_warmup_count_are_validated(self):
        rows = recording()
        rows[5]["duration_ns"] -= 1
        with self.assertRaises(JIT.JitAnalysisError):
            self.analyze(rows)
        with self.assertRaisesRegex(JIT.JitAnalysisError, "warmup duration"):
            self.analyze(warmup_windows=5)
        with self.assertRaises(JIT.JitAnalysisError):
            self.analyze(required_clean=0)

    def test_provisional_selection_cannot_be_reported_as_final_validity(self):
        result = self.analyze(provisional=True)
        self.assertTrue(result["complete"])
        self.assertFalse(result["valid"])
        self.assertTrue(result["provisional"])

    def test_missing_jfr_writes_invalid_result_and_returns_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = root / "result.json"
            code = JIT.main(["--source", str(root / "missing.jfr"), "--target", str(root / "missing2.jfr"),
                             "--output", str(result)])
            self.assertEqual(1, code)
            self.assertFalse(json.loads(result.read_text())["valid"])

    @unittest.skipUnless(shutil.which("java") and shutil.which("javac"), "JDK required")
    def test_reader_streams_real_jfr_settings_and_custom_events(self):
        source = '''
import java.nio.file.Path;
import jdk.jfr.*;
public class Fixture {
 @Name("measure.MonotonicAnchor") static class Anchor extends Event {long lowerNs; long upperNs;}
 @Name("measure.Phase") static class Phase extends Event {String phase; String cellId; long startNs; long durationNs;}
 public static void main(String[] args) throws Exception {
  try (Recording r = new Recording(Configuration.create(Path.of(args[0])))) {
   r.start(); new Phase().commit();
   Anchor a=new Anchor(); a.lowerNs=System.nanoTime();a.begin();a.upperNs=System.nanoTime();a.commit();
   Phase p=new Phase();p.phase="active";p.cellId="fixture";p.startNs=System.nanoTime();p.durationNs=5_000_000_000L;p.commit();
   r.stop();r.dump(Path.of(args[1]));
  }
 }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Fixture.java").write_text(source)
            subprocess.run([JIT.java_tool("javac"), str(root / "Fixture.java")], check=True, capture_output=True)
            subprocess.run([JIT.java_tool("java"), "-cp", str(root), "Fixture",
                            str(TOOLS / "jit" / "compilation.jfc"), str(root / "fixture.jfr")],
                           check=True, capture_output=True)
            result = JIT.read_pair(root / "fixture.jfr", root / "fixture.jfr")
            decoded = JIT.parse_recording(result["source"], "source")
            self.assertEqual("fixture", decoded["phases"][0]["cell_id"])
            (root / "truncated.jfr").write_bytes((root / "fixture.jfr").read_bytes()[:100])
            with self.assertRaises(JIT.JitAnalysisError):
                JIT.read_pair(root / "truncated.jfr", root / "fixture.jfr")


if __name__ == "__main__":
    unittest.main()
