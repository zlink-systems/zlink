"""Server-driven cell input, A/B joining, and document output."""

from __future__ import annotations

import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)

from benchagg.analysis import build_rows, judge_pair  # noqa: E402
from benchagg.model import PATTERNS, CellKey  # noqa: E402
from benchagg.readers import (  # noqa: E402
    ReportError,
    cells_from_server_document,
    merge_server_driven_cells,
    read_run,
    read_runs,
)
from benchagg.render import (  # noqa: E402
    render_companion_table,
    render_doc_table,
    render_result_lines,
    render_spec4_table,
)

FIXTURE = os.path.join(_TOOLS, "tests", "fixtures")
S2S = os.path.join(FIXTURE, "s2s")
PAIRED = [os.path.join(S2S, f"paired-{index}") for index in range(1, 4)]
C_RUNS = [os.path.join(FIXTURE, "gated2", f"c-router-{index}") for index in range(1, 4)]


class ServerDrivenMergeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.run_set = read_runs(PAIRED)
        cls.rows = build_rows(cls.run_set)

    def test_source_and_target_files_join_by_trigger_identity(self):
        cells, notes = read_run(PAIRED[0])
        self.assertEqual(len(cells), 5)
        raw = next(
            cell
            for cell in cells
            if cell.key == CellKey("zlink-dotnet", "request-window", 1024)
        )
        self.assertTrue(raw.complete)
        self.assertEqual((raw.run_id, raw.cell_id), ("s2s-1", "raw-window-1024"))
        self.assertEqual(raw.streams, {"count": 1, "inFlightPerStream": 100})
        self.assertEqual(raw.server_cpu_percent, 4.0)
        self.assertEqual(raw.server_memory_mb, 80.0)
        self.assertEqual(raw.server_received_at_close, 500000)
        self.assertEqual(raw.target_errors, 0)
        self.assertEqual(raw.drain_ms, 20.0)
        send = next(cell for cell in cells if cell.key.pattern == "send-saturation")
        self.assertEqual(send.throughput_per_second, 50000.0)
        self.assertEqual(send.bandwidth_mb_s, 51.2)
        self.assertTrue(any("4 file(s)" in note for note in notes))

    def test_three_complete_pairs_make_the_median_and_g5(self):
        row = self.rows[CellKey("zlink-dotnet", "request-window", 1024)]
        self.assertEqual(row.run_count, 3)
        self.assertEqual(row.throughput, 100000.0)
        self.assertEqual(row.values["server_cpu_percent"], 4.0)
        self.assertEqual(row.g5_status, "pass")
        self.assertAlmostEqual(row.spread_percent, 2.0)
        self.assertEqual((row.stream_count, row.in_flight_per_stream), (1, 100))

    def test_missing_target_is_visible_and_excluded(self):
        cells, notes = read_run(os.path.join(S2S, "missing-target"))
        self.assertEqual(len(cells), 1)
        self.assertFalse(cells[0].complete)
        self.assertIn("target", cells[0].incomplete_reason)
        self.assertTrue(any("incomplete" in note for note in notes))

        rows = build_rows(read_runs([os.path.join(S2S, "missing-target")]))
        key = CellKey("grpc-dotnet", "request-serial", 1024)
        self.assertEqual(rows[key].run_count, 0)
        judgement = judge_pair(
            rows,
            "grpc-dotnet / grpc-dotnet",
            "grpc-dotnet",
            "grpc-dotnet",
            1024,
            "request-serial",
        )
        self.assertEqual(judgement.status, "unsupported")
        self.assertIn("incomplete", judgement.reason)

    def test_server_and_client_driven_inputs_share_one_run_set(self):
        mixed = read_runs(PAIRED + C_RUNS)
        self.assertIn(CellKey("zlink-dotnet", "request-window", 1024), mixed.keys())
        self.assertIn(CellKey("zlink-c", "request-window", 1024), mixed.keys())
        legacy = next(cell for cell in mixed.cells if cell.key.implementation == "zlink-c")
        self.assertIsNone(legacy.role)
        self.assertTrue(legacy.complete)

    def test_root_level_server_document_keeps_the_existing_results_container(self):
        payload = {
            "role": "source",
            "trigger": {
                "runId": "root-run",
                "cellId": "root-cell",
                "pattern": "request-window",
                "payloadBytes": 1024,
                "durationMs": 5000,
                "warmup": 1000,
                "endpoint": "http://127.0.0.1:5205/bench/start",
                "receivedAtUnixMs": 1788937000000,
            },
            "streams": {"count": 1, "inFlightPerStream": 100},
            "target_stats": {"received": 500000, "errors": 0, "drainMs": 20},
            "metadata": {"implementation": "zlink-dotnet", "logicalCores": 20},
            "results": [
                {
                    "implementation": "zlink-dotnet",
                    "pattern": "request-window",
                    "payloadSize": 1024,
                    "durationSeconds": 5,
                    "throughput": 100000,
                    "meanMicros": 500,
                    "p95Micros": 800,
                    "p99Micros": 1000,
                    "clientCpuSeconds": 5,
                    "clientWorkingSetMb": 100,
                    "serverCpuSeconds": 4,
                    "serverWorkingSetMb": 80,
                    "clientCores": 1,
                    "clientParallelismCeiling": 20,
                    "peakInFlight": 100,
                    "requestWindow": 100,
                    "abandoned": 0,
                    "errors": 0,
                }
            ],
        }
        cells = merge_server_driven_cells(
            cells_from_server_document(payload, "root-run", "results.json")
        )
        self.assertEqual(len(cells), 1)
        self.assertTrue(cells[0].complete)
        self.assertEqual(cells[0].throughput_per_second, 100000)
        self.assertEqual(cells[0].client_cpu_percent, 5.0)
        self.assertEqual(cells[0].server_cpu_percent, 4.0)

    def test_trigger_field_aliases_are_not_invented(self):
        payload = {
            "role": "source",
            "trigger": {
                "runId": "root-run",
                "cellId": "root-cell",
                "pattern": "request-window",
                "payloadBytes": 1024,
                "durationMs": 5000,
                "receivedAtUnixMs": 1,
            },
            "streams": {"count": 1, "inFlightPerStream": 100},
            "metadata": {"implementation": "zlink-dotnet"},
            "results": [{}],
        }
        with self.assertRaisesRegex(ReportError, "warmup, endpoint"):
            cells_from_server_document(payload, "root-run", "results.json")


class ServerDrivenRenderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = build_rows(read_runs(PAIRED + C_RUNS))

    def test_spec4_uses_source_target_names_and_pattern_units(self):
        table = render_spec4_table(self.rows, (1024,))
        self.assertIn("Source CPU", table)
        self.assertIn("Target Mem", table)
        self.assertNotIn("Client CPU", table)
        self.assertIn("5.32 KOPS", table)
        self.assertIn("50.00 KMSG/s", table)

    def test_current_grid_has_three_patterns_and_omits_archived_window_rows(self):
        self.assertEqual(
            PATTERNS,
            ("request-serial", "request-backpressure", "send-saturation"),
        )
        table = render_spec4_table(self.rows, (1024,))
        self.assertNotIn("request-window", table)

    def test_result_lines_use_the_three_run_median(self):
        output = render_result_lines(
            self.rows, (1024,), implementations=("zlink-dotnet",)
        )
        self.assertNotIn("request-window", output)
        self.assertIn(
            "RESULT,current,zlink-dotnet-send-saturation,local,1024,throughput,50000.000",
            output,
        )

    def test_companion_table_carries_streams_and_trigger_endpoint(self):
        table = render_companion_table(
            self.rows, (1024,), implementations=("zlink-dotnet",)
        )
        self.assertNotIn("request-window", table)
        self.assertIn(
            "| send-saturation | 1024 | `zlink-dotnet` | 8 | 1 | http://127.0.0.1:5200/bench/start |",
            table,
        )
        self.assertIn("does not infer one from spec 9 ports", table)

    def test_doc_table_is_language_scoped_markdown(self):
        table = render_doc_table(self.rows, (1024,), "dotnet")
        self.assertTrue(table.startswith("| Language | Pattern | Payload |"))
        self.assertNotIn("request-window", table)
        self.assertIn("| dotnet | send-saturation | 1024B | `zlink-dotnet` | 50.000 | KMSG/s |", table)
        self.assertNotIn("`zlink-c`", table)


if __name__ == "__main__":
    unittest.main()
