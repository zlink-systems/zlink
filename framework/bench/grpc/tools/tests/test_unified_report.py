"""The unified run report (spec §4) and the C report -> cell conversion."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import bench_report  # noqa: E402
from benchagg.readers import CELL_JSON_VERSION  # noqa: E402

GRID = TOOLS.parent


def cell(implementation: str, pattern: str, **fields) -> dict:
    document = {
        "implementation": implementation, "pattern": pattern, "payload_size": 4096,
        "throughput_per_second": 12345.6, "bandwidth_mb_s": 12.6,
        "latency_mean_ms": 1.5, "latency_p95_ms": 2.5, "latency_p99_ms": 3.5,
    }
    document.update(fields)
    return document


class UnifiedReportTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.run = Path(self.directory.name)

    def write(self, *cells: dict):
        for document in cells:
            name = f'{document["implementation"]}-{document["pattern"]}-{document["payload_size"]}'
            (self.run / name).mkdir(parents=True, exist_ok=True)
            (self.run / name / "results.json").write_text(
                json.dumps({"schema": CELL_JSON_VERSION, "cells": [document]}))

    def test_request_patterns_are_kops_and_send_is_kmsg(self):
        self.write(cell("zlink-c", "request-backpressure"), cell("zlink-c", "send-saturation"))
        rendered = bench_report.render(str(self.run))
        self.assertIn("12.346 KOPS", rendered)
        self.assertIn("12.346 Kmsg/s", rendered)
        # The two units share a scale; only the name says what was counted.
        self.assertEqual(rendered.count("12.346"), 2)

    def test_columns_include_bandwidth_and_source_and_target_resources(self):
        self.write(cell("zlink-c", "request-serial"))
        header = bench_report.render(str(self.run)).splitlines()[0]
        self.assertEqual(
            [name.strip() for name in header.strip("| ").split("|")],
            ["Scenario", "Size", "Throughput", "Bandwidth(MB/s)",
             "Lat.Mean(ms)", "Lat.P95(ms)", "Lat.P99(ms)",
             "Source CPU(%)", "Source Mem(MB)", "Target CPU(%)", "Target Mem(MB)"])

    def test_bandwidth_reuses_the_normalized_record_measurement(self):
        self.write(cell("zlink-dotnet", "request-serial", bandwidth_mb_s=50.56789),
                   cell("zlink-dotnet", "send-saturation", bandwidth_mb_s=60.76543))
        rows = bench_report.render(str(self.run)).splitlines()[2:4]
        values = [[value.strip() for value in row.strip("| ").split("|")] for row in rows]
        self.assertEqual([row[3] for row in values], ["50.568", "60.765"])

    def test_missing_bandwidth_is_not_invented_and_zero_is_preserved(self):
        self.write(cell("zlink-dotnet", "request-serial", bandwidth_mb_s=None),
                   cell("zlink-dotnet", "send-saturation", bandwidth_mb_s=0))
        rows = bench_report.render(str(self.run)).splitlines()[2:4]
        values = [[value.strip() for value in row.strip("| ").split("|")] for row in rows]
        self.assertEqual([row[3] for row in values], ["n/a", "0.000"])

    def test_bandwidth_explains_counted_direction_and_decimal_units(self):
        self.write(cell("zlink-dotnet", "request-serial"))
        rendered = bench_report.render(str(self.run))
        self.assertIn("derived application payload MB/s (1 MB = 1,000,000 bytes)", rendered)
        self.assertIn("completed requests/s x response Size / 1,000,000", rendered)
        self.assertIn("(64-byte requests excluded)", rendered)
        self.assertIn("send-saturation target-received messages/s x Size / 1,000,000", rendered)
        self.assertIn("Protocol overhead is excluded.", rendered)

    def test_send_bandwidth_uses_target_received_not_source_submitted(self):
        self.write(cell("zlink-dotnet", "send-saturation",
                        role="source",
                        trigger={"runId": "run", "cellId": "send", "pattern": "send-saturation",
                                 "payloadBytes": 4096, "durationMs": 5000, "warmup": 0,
                                 "endpoint": "http://127.0.0.1:5200/bench/start",
                                 "receivedAtUnixMs": 1788937000000},
                        streams={"count": 1, "inFlightPerStream": 8},
                        target_stats={"received": 5000, "errors": 0, "drainMs": 20},
                        submitted=10000, bandwidth_mb_s=8.192))
        row = bench_report.render(str(self.run)).splitlines()[2]
        values = [value.strip() for value in row.strip("| ").split("|")]
        # 5000 target receipts / 5 seconds x 4096 bytes / 1,000,000.
        self.assertEqual(values[2:4], ["1.000 Kmsg/s", "4.096"])

    def test_resource_values_keep_source_and_target_distinct(self):
        self.write(cell("zlink-dotnet", "send-saturation",
                        client_cpu_percent=18.92, client_memory_mb=2951.71875,
                        server_cpu_percent=8.09, server_memory_mb=495.18359375))
        row = bench_report.render(str(self.run)).splitlines()[2]
        values = [value.strip() for value in row.strip("| ").split("|")]
        self.assertEqual(values[7:], ["18.920", "2951.719", "8.090", "495.184"])

    def test_missing_resources_are_not_reported_as_zero(self):
        self.write(cell("zlink-c", "request-serial",
                        client_cpu_percent=0, server_memory_mb=0))
        row = bench_report.render(str(self.run)).splitlines()[2]
        values = [value.strip() for value in row.strip("| ").split("|")]
        self.assertEqual(values[7:], ["0.000", "n/a", "n/a", "0.000"])

    def test_every_language_renders_through_the_same_function(self):
        # A row missing a measurement says so rather than printing a zero.
        self.write(cell("zlink-node", "request-serial", latency_p99_ms=None))
        self.assertIn("n/a", bench_report.render(str(self.run)))


class CConversionTest(unittest.TestCase):
    REPORT = """\
| Scenario | Size | Throughput | Bandwidth |
RESULT,current,zlink-c-request-serial,local,4096,throughput,600.000
RESULT,current,zlink-c-request-serial,local,4096,bandwidth,2457.600
RESULT,current,zlink-c-request-serial,local,4096,latency,1.000
RESULT,current,zlink-c-request-serial,local,4096,latency_p95,2.000
RESULT,current,zlink-c-request-serial,local,4096,latency_p99,3.000
RESULT,current,zlink-c-send-blocking,local,4096,throughput,100.000
RESULT,current,zlink-c-send-blocking,local,4096,bandwidth,409.600
"""

    def convert(self, text: str) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        report = Path(directory.name) / "client.log"
        report.write_text(text)
        out = Path(directory.name) / "run"
        result = subprocess.run(
            [sys.executable, str(TOOLS / "c_cells_from_report.py"), str(report), str(out)],
            capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return out

    def test_kops_report_is_converted_to_completions_per_second(self):
        out = self.convert(self.REPORT)
        document = json.loads((out / "zlink-c-request-serial-4096" / "results.json").read_text())
        self.assertEqual(document["schema"], CELL_JSON_VERSION)
        # 600 KOPS in the report, 600000/s in the record: the scale comes from the
        # report's own bandwidth column, not from an assumption about the runner.
        self.assertAlmostEqual(document["cells"][0]["throughput_per_second"], 600000.0, places=3)

    def test_patterns_outside_the_grid_do_not_become_cells(self):
        out = self.convert(self.REPORT)
        self.assertFalse((out / "zlink-c-send-blocking-4096").exists())
        self.assertEqual(sorted(path.name for path in out.iterdir()),
                         ["zlink-c-request-serial-4096"])

    def test_a_report_with_no_grid_cell_fails_rather_than_writing_nothing(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        report = Path(directory.name) / "client.log"
        report.write_text("RESULT,current,zlink-c-send-blocking,local,1024,throughput,1.000\n"
                          "RESULT,current,zlink-c-send-blocking,local,1024,bandwidth,1.024\n")
        result = subprocess.run(
            [sys.executable, str(TOOLS / "c_cells_from_report.py"), str(report),
             str(Path(directory.name) / "run")],
            capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unreadable client report", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


class RunnerContractTest(unittest.TestCase):
    """Every runner takes the same inputs and rejects the same mistakes (§3.1)."""

    RUNNERS = ("c/run_local.sh", "cpp/run_local.sh", "dotnet/run_local.sh",
               "java/run_local.sh", "java/run_local_kotlin.sh", "node/run_local.sh")

    def run_runner(self, runner: str, *args: str, **environment: str):
        import os
        env = dict(os.environ, **environment)
        return subprocess.run(["bash", str(GRID / runner), *args],
                              capture_output=True, text=True, check=False, env=env)

    def test_every_runner_rejects_an_unknown_argument(self):
        for runner in self.RUNNERS:
            with self.subTest(runner=runner):
                result = self.run_runner(runner, "--patterns", "send-saturation")
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("unsupported runner argument: --patterns", result.stderr)

    def test_dotnet_build_preserves_configuration_for_external_references(self):
        runner = (TOOLS.parent / "dotnet" / "run_local.sh").read_text()
        self.assertIn('-c "${CONFIGURATION}"', runner)
        self.assertIn('-p:ShouldUnsetParentConfigurationAndPlatform=false', runner)

    def test_dotnet_help_lists_options_and_serial_example(self):
        for flag in ("--help", "-h"):
            with self.subTest(flag=flag):
                result = self.run_runner("dotnet/run_local.sh", flag)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")
                for option in ("--help", "--scenario", "--implementation",
                               "--payload-sizes", "--duration-seconds",
                               "--warmup-seconds", "--skip-build", "--output"):
                    self.assertIn(option, result.stdout)
                self.assertIn("bash run_local.sh --scenario request-serial", result.stdout)
                self.assertIn("64-byte request / 4096-byte response", result.stdout)

    def test_dotnet_help_does_not_validate_environment_or_create_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "not-created"
            result = self.run_runner(
                "dotnet/run_local.sh", "--output", str(output),
                "--scenario", "request-serial", "--help",
                OUTROOT="retired-input", SCENARIO="invalid-scenario",
                DURATION_SECONDS="invalid-duration")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertIn("Usage:", result.stdout)
            self.assertFalse(output.exists())

    def test_every_runner_rejects_a_retired_input_by_name(self):
        for runner in self.RUNNERS:
            with self.subTest(runner=runner):
                result = self.run_runner(runner, OUTROOT="/tmp/somewhere")
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("retired runner input: OUTROOT -> OUTPUT", result.stderr)

    def test_warmup_is_a_time_every_runner_takes_the_same_way(self):
        # A call count warms each runtime for a different length of time; §3.1 makes warmup
        # one wall-clock input so the rows spec 7.2 divides were produced the same way.
        for runner in self.RUNNERS:
            with self.subTest(runner=runner):
                result = self.run_runner(runner, WARMUP="1000")
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("retired runner input: WARMUP -> WARMUP_SECONDS", result.stderr)
                result = self.run_runner(runner, "--warmup-seconds", "0")
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("WARMUP_SECONDS must be a positive integer", result.stderr)

    def test_every_runner_rejects_a_payload_size_outside_the_spec(self):
        for runner in self.RUNNERS:
            with self.subTest(runner=runner):
                result = self.run_runner(runner, "--payload-sizes", "1024")
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn("1024", result.stderr)


if __name__ == "__main__":
    unittest.main()
