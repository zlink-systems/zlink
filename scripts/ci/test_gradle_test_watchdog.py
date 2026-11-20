"""Exercise the Java workflow's watchdog command from its configured cwd."""

import os
from pathlib import Path
import re
import select
import signal
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/framework-java.yml"


class GradleTestWatchdogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.env = dict(os.environ, GITHUB_WORKSPACE=str(ROOT))
        self.env["PATH"] = str(self.bin) + os.pathsep + self.env["PATH"]

    def tool(self, name, body):
        path = self.bin / name
        path.write_text("#!/usr/bin/env bash\nset -eu\n" + body)
        path.chmod(0o755)

    def start_watchdog(self, output):
        workflow = WORKFLOW.read_text()
        step = workflow.split(
            "- name: Run Framework Java core, integration and Spring starter tests", 1
        )[1].split("\n      - name:", 1)[0]
        cwd = re.search(r"working-directory: (.+)", step).group(1).strip()
        command = re.search(
            r'^\s*(bash .+gradle-test-watchdog\.sh[^\n]+) &$', step, re.MULTILINE
        ).group(1)
        # Retain the workflow's executable path and output argument; shorten observation only.
        command = command.replace(" 3600 600 ", " 0 1 ")
        log = (output.parent / (output.name + ".log")).open("w+")
        self.addCleanup(log.close)
        process = subprocess.Popen(
            ["bash", "-c", command],
            cwd=ROOT / cwd,
            env=dict(self.env, dump_dir=str(output)),
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

        def stop():
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=10)

        self.addCleanup(stop)
        return process, log

    def wait_for_dump(self, output, process, log):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            dumps = list(output.glob("*.txt"))
            if dumps and "DUMP_COMPLETE" in dumps[0].read_text():
                return dumps
            if process.poll() is not None:
                break
            time.sleep(0.01)
        log.seek(0)
        self.fail(f"watchdog exit={process.poll()}, dump absent: {log.read()}")

    def test_workflow_path_produces_dump_40_times(self):
        self.tool(
            "jps",
            'if [[ -e "$WATCHDOG_BASELINE" ]]; then\n'
            '  echo "99999 worker.org.gradle.process.internal.worker.GradleWorkerMain"\n'
            'else touch "$WATCHDOG_BASELINE"; fi\n',
        )
        self.tool("jstack", '[[ "$*" == "-l 99999" ]]\necho DUMP_COMPLETE\n')
        failures = 0
        for iteration in range(1, 41):
            self.env["WATCHDOG_BASELINE"] = str(self.directory / f"baseline-{iteration}")
            with self.subTest(iteration=iteration):
                process, log = self.start_watchdog(self.directory / f"dumps-{iteration}")
                try:
                    dumps = self.wait_for_dump(
                        self.directory / f"dumps-{iteration}", process, log
                    )
                    self.assertEqual(1, len(dumps))
                except AssertionError:
                    failures += 1
                    raise
                finally:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                    process.wait(timeout=10)
        print(f"SUMMARY attempts=40 failures={failures} passed={40 - failures}", flush=True)

    def test_real_jvm_dump_and_no_target_control(self):
        self.tool(
            "jps",
            'touch "$WATCHDOG_BASELINE"\n'
            'if [[ -e "$WATCHDOG_TARGET_FILE" ]]; then\n'
            '  echo "$(cat "$WATCHDOG_TARGET_FILE") worker.org.gradle.process.internal.worker.GradleWorkerMain"\n'
            'fi\n',
        )
        self.env.update(
            WATCHDOG_TARGET_FILE=str(self.directory / "target"),
            WATCHDOG_BASELINE=str(self.directory / "baseline"),
        )
        self.tool("jstack", '"$JAVA_HOME/bin/jstack" "$@"\necho DUMP_COMPLETE\n')
        output = self.directory / "real-dumps"
        process, log = self.start_watchdog(output)
        deadline = time.monotonic() + 15
        while not (self.directory / "baseline").exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue((self.directory / "baseline").exists())
        source = self.directory / "WatchdogSleeper.java"
        source.write_text(
            'class WatchdogSleeper { public static void main(String[] args) throws Exception { '
            'System.out.println("READY"); Thread.sleep(120000); } }\n'
        )
        jvm = subprocess.Popen(
            ["java", str(source)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
        )

        def stop_jvm():
            jvm.terminate()
            jvm.wait(timeout=10)
            jvm.stdout.close()

        self.addCleanup(stop_jvm)
        self.assertTrue(select.select([jvm.stdout], [], [], 15)[0], "JVM did not become ready")
        self.assertEqual(b"READY\n", jvm.stdout.readline())
        (self.directory / "target").write_text(str(jvm.pid))
        dumps = self.wait_for_dump(output, process, log)
        dump = dumps[0].read_text()
        self.assertIn("Full thread dump", dump)
        self.assertIn("java.lang.Thread.sleep", dump)
        print(dump, flush=True)
        print("REAL_JVM Full thread dump and java.lang.Thread.sleep confirmed", flush=True)
        self.tool("jps", "exit 0\n")
        output = self.directory / "no-target"
        process, log = self.start_watchdog(output)
        time.sleep(1.2)
        self.assertIsNone(process.poll())
        self.assertEqual([], list(output.glob("*.txt")))
        print("NO_TARGET dumps=0", flush=True)


if __name__ == "__main__":
    unittest.main()
