"""Small CTest fixtures for sanitizer sharding and result aggregation."""

import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


shards = load("sanitizer_shards", "sanitizer-shards.py")
summary = shards.summary


class SanitizerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def target(self, name):
        return {"id": name, "name": name, "type": "EXECUTABLE",
                "paths": {"build": "."}, "artifacts": [{"path": f"{name}.cmake"}]}

    def test_shared_executable_stays_in_one_shard(self):
        targets = [self.target("a"), self.target("b")]
        tests = [{"name": name, "command": [str(self.root / f"{target}.cmake")]}
                 for name, target in [("a1", "a"), ("b1", "b"), ("a2", "a")]]
        plan = shards.make_shards(self.root, targets, tests, 1)
        self.assertEqual(len(plan), 2)
        self.assertEqual(sorted(next(p for p in plan if "a" in p["targets"])["tests"]),
                         ["a1", "a2"])
        self.assertEqual(sorted(n for p in plan for n in p["tests"]), ["a1", "a2", "b1"])

    def test_file_api_target_inventory(self):
        reply = self.root / ".cmake/api/v1/reply"
        reply.mkdir(parents=True)
        target = self.target("test")
        (reply / "target.json").write_text(json.dumps(target))
        (reply / "model.json").write_text(json.dumps({
            "configurations": [{"targets": [{"jsonFile": "target.json"}]}],
        }))
        (reply / "index-1.json").write_text(json.dumps({
            "reply": {"codemodel-v2": {"jsonFile": "model.json"}},
        }))
        self.assertEqual(shards.read_targets(self.root), [target])

    def test_remove_only_selected_artifacts_and_objects(self):
        for name in ("a", "b"):
            (self.root / f"{name}.cmake").write_text("artifact")
            objects = self.root / f"CMakeFiles/{name}.dir"
            objects.mkdir(parents=True)
            (objects / "test.o").write_text("object")
        shards.remove_artifacts(self.root, [self.target("a")])
        self.assertFalse((self.root / "a.cmake").exists())
        self.assertFalse((self.root / "CMakeFiles/a.dir/test.o").exists())
        self.assertTrue((self.root / "b.cmake").exists())
        self.assertTrue((self.root / "CMakeFiles/b.dir/test.o").exists())
        with self.assertRaises(ValueError):
            shards.inside(self.root, "../outside")

    def test_target_path_prefix_is_not_a_dependency(self):
        short = self.target("test")
        long = self.target("test_extra")
        short["artifacts"] = [{"path": "test"}]
        long["artifacts"] = [{"path": "test_extra"}]
        tests = [{"name": "extra", "command": [str(self.root / "test_extra")]},
                 {"name": "short", "command": ["cmake", f"-DEXE={self.root / 'test'}"]}]
        plan = shards.make_shards(self.root, [short, long], tests, 1)
        self.assertEqual([p["targets"] for p in plan], [{"test_extra"}, {"test"}])

    def test_junit_diagnostics_skipped_and_coverage(self):
        junit = self.root / "fixture.xml"
        junit.write_text('''<testsuite>
          <testcase name="pass"><system-out>FAILED is ordinary text</system-out></testcase>
          <testcase name="asan"><failure>Assertion failed</failure>
            <system-out>ERROR: AddressSanitizer: heap-use-after-free
            #0 fixture.cc:12</system-out></testcase>
          <testcase name="missing"><skipped message="Unable to find executable"/></testcase>
        </testsuite>''')
        report = summary.summarize(summary.parse_junit(junit, "framework"))
        self.assertEqual((report["total"], report["passed"]), (3, 1))
        self.assertEqual(report["failed"][0]["sanitizer"],
                         ["AddressSanitizer: heap-use-after-free"])
        self.assertEqual(shards.coverage(junit, ["pass", "asan", "missing"]), [])
        self.assertTrue(shards.coverage(junit, ["pass", "asan"]))
        self.assertTrue(shards.coverage(junit, ["pass", "asan", "missing", "missing"]))
        junit.write_text("malformed XML")
        self.assertTrue(shards.coverage(junit, ["pass"]))
        self.assertTrue(shards.coverage(self.root / "missing.xml", ["pass"]))

    def test_build_keeps_going_and_records_disk_samples(self):
        log = self.root / "shard.build.log"
        with patch.object(shards.subprocess, "run", return_value=SimpleNamespace(returncode=1)) as run:
            code = shards.build_targets(self.root, ["test_one", "test_two"], 2, log)
        self.assertEqual(code, 1)
        command = run.call_args.args[0]
        self.assertEqual(command[-2:], ["-k", "0"])
        self.assertIn("test_two", command)
        self.assertTrue(json.loads(log.with_suffix(".disk.json").read_text()))

    @unittest.skipUnless(shutil.which("ctest"), "CTest required")
    def test_discovery_resolves_unbuilt_executable_and_removes_placeholder(self):
        target = self.target("unbuilt")
        target["artifacts"] = [{"path": "unbuilt.exe"}]
        executable = (self.root / "unbuilt.exe").as_posix()
        (self.root / "CTestTestfile.cmake").write_text(
            f'add_test("unbuilt" "{executable}")\n'
            'set_tests_properties("unbuilt" PROPERTIES LABELS "unit")\n'
        )
        tests = shards.discover(self.root, [target], "^unit$")
        self.assertEqual(tests[0]["command"][0].replace("\\", "/"), executable)
        self.assertFalse((self.root / "unbuilt.exe").exists())

    def test_merge_cli_and_missing_result(self):
        sources = []
        for suite in ("core", "framework"):
            path = self.root / f"{suite}.json"
            path.write_text(json.dumps(summary.summarize([
                {"test": f"{suite}::pass", "returncode": 0},
            ])))
            sources.extend(["--results", str(path)])
        output = self.root / "summary.json"
        command = [sys.executable, str(Path(summary.__file__)), *sources,
                   "--output", str(output), "--log-dir", str(self.root)]
        good = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertEqual(json.loads(output.read_text())["passed"], 2)
        bad = subprocess.run(command + ["--results", str(self.root / "missing.json")],
                             capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1)
        self.assertIn("Could not read result file", output.read_text())
        duplicate = subprocess.run(command + sources[:2], capture_output=True, text=True)
        self.assertEqual(duplicate.returncode, 1)
        self.assertIn("duplicate-results", output.read_text())
        (self.root / "core.json").write_text("invalid JSON")
        malformed = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(malformed.returncode, 1)
        self.assertEqual(json.loads(output.read_text())["passed"], 1)

    @unittest.skipUnless(shutil.which("ctest") and shutil.which("cmake"), "CTest required")
    def test_real_ctest_shards_with_build_and_sanitizer_failures(self):
        targets = [self.target(n) for n in ("pass", "asan", "broken")]
        names = [("pass.one", "pass"), ("pass.two", "pass"),
                 ("asan", "asan"), ("broken", "broken")]
        commands = []
        for name, target in names:
            script = (self.root / f"{target}.cmake").as_posix()
            commands.extend([
                f'add_test("{name}" "{shutil.which("cmake").replace(chr(92), "/")}" "-P" "{script}")',
                f'set_tests_properties("{name}" PROPERTIES LABELS "unit")',
            ])
        (self.root / "CTestTestfile.cmake").write_text("\n".join(commands))
        with (self.root / "CTestTestfile.cmake").open("a") as output:
            output.write('\nadd_test("excluded" "missing-program")\n'
                         'set_tests_properties("excluded" PROPERTIES LABELS "integration")\n')
        logs = self.root / "logs"
        logs.mkdir()
        args = SimpleNamespace(build_dir=self.root, suite="framework", label="^(unit|contract)$",
                               shard_size=1, parallel=2, log_dir=logs)
        built = []

        def fake_build(build, selected, parallel, log):
            built.extend(selected)
            log.write_text("FAILED: broken.cmake\n" if "broken" in selected else "built\n")
            for name in selected:
                if name != "broken":
                    text = 'message("passed")'
                    if name == "asan":
                        text = ('message("ERROR: AddressSanitizer: heap-use-after-free\\n'
                                '#0 fixture.cc:12")\nmessage(FATAL_ERROR "Assertion failed")')
                    (build / f"{name}.cmake").write_text(text)
            return 1 if "broken" in selected else 0

        results = []
        with patch.object(shards, "read_targets", return_value=targets), \
                patch.object(shards, "build_targets", side_effect=fake_build), \
                patch.object(shards, "disk"):
            shards.run(args, results)
        report = summary.summarize(results)
        self.assertEqual(sorted(built), ["asan", "broken", "pass"])
        cases = [r for r in report["results"] if "::build::" not in r["test"]]
        self.assertEqual(len(cases), 4)
        self.assertEqual(report["passed"], 2)
        self.assertEqual(len(report["failed"]), 3)
        self.assertTrue(any(r["sanitizer"] for r in report["failed"]))
        self.assertTrue(any("broken.cmake" in r["assertions"] for r in report["failed"]))
        self.assertTrue(any("fixture.cc:12" in p.read_text() for p in logs.glob("*.ctest.log")))
        self.assertFalse(any(self.root.glob("pass.cmake")))
        self.assertFalse(any(self.root.glob("asan.cmake")))


if __name__ == "__main__":
    unittest.main()
