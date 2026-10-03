"""Behaviour the Phase 0 material cannot exercise.

``gated2`` has no contaminated cell and no client-saturated cell, so those paths
are driven from synthetic cells here. They are the paths that decide whether a later language can publish a number Phase 0's
rules would have rejected, so they are tested even though nothing has hit them.
"""

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)

from benchagg.analysis import build_rows, judge_pair, spread_percent  # noqa: E402
from benchagg.model import Cell, CellKey, RunSet  # noqa: E402
from benchagg.readers import ReportError, read_run  # noqa: E402


def cell(impl, pattern, size, run, throughput, **kwargs):
    return Cell(
        key=CellKey(impl, pattern, size),
        run=run,
        throughput_per_second=throughput,
        latency_mean_ms=1.0,
        **kwargs,
    )


def run_set_of(*cells):
    run_set = RunSet()
    for item in cells:
        run_set.add(item)
    return run_set


class ExclusionTest(unittest.TestCase):
    def test_spread_is_the_widest_run_relative_to_the_median(self):
        self.assertAlmostEqual(spread_percent([387.9, 310.5, 230.8]), 25.66, places=1)
        self.assertIsNone(spread_percent([1.0]))

    def test_a_contaminated_run_does_not_move_a_median(self):
        """FB-008: a contaminated cell is not measured badly, it is not measured."""
        run_set = run_set_of(
            cell("zlink-node", "request-window", 1024, "r1", 100.0),
            cell("zlink-node", "request-window", 1024, "r2", 100.0),
            cell("zlink-node", "request-window", 1024, "r3", 100.0),
            cell(
                "zlink-node", "request-window", 1024, "r4", 5.0,
                contaminated=True, contamination_reason="previous cell did not drain",
            ),
        )
        row = build_rows(run_set)[CellKey("zlink-node", "request-window", 1024)]
        self.assertEqual(row.run_count, 3)
        self.assertEqual(row.throughput, 100.0)
        self.assertEqual(row.excluded_runs, ["r4"])
        self.assertEqual(len(run_set.contaminated()), 1)

    def _judgement(self, numerator_cells, denominator_cells, numerator="zlink-node",
                   pattern="request-window"):
        """These cases exercise G5, saturation and publication, not pattern choice.

        They name the pattern explicitly so that changing which pattern the spec
        makes the judgement reference cannot silently rewrite what they assert.
        """
        rows = build_rows(run_set_of(*numerator_cells, *denominator_cells))
        return judge_pair(
            rows, f"{numerator} / zlink-c", numerator, "zlink-c", 1024, pattern
        )

    def test_default_judgement_pattern_is_backpressure(self):
        """spec 7.2: the ratios reference the pattern with no imposed depth.

        A ratio taken at a depth the numerator never reaches reports the imposed
        depth rather than the layer cost, so the default must not be the fixed
        window.
        """
        from benchagg.model import JUDGEMENT_PATTERN

        self.assertEqual(JUDGEMENT_PATTERN, "request-backpressure")
        steady = [cell("zlink-node", "request-backpressure", 1024, f"r{i}", 100.0)
                  for i in range(3)]
        baseline = [cell("zlink-c", "request-backpressure", 1024, f"c{i}", 110.0)
                    for i in range(3)]
        rows = build_rows(run_set_of(*steady, *baseline))
        judgement = judge_pair(rows, "zlink-node / zlink-c", "zlink-node", "zlink-c", 1024)
        self.assertEqual(judgement.status, "published")

    def test_publishes_only_when_both_sides_pass_g5(self):
        steady = [cell("zlink-node", "request-window", 1024, f"r{i}", 100.0) for i in range(3)]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        judgement = self._judgement(steady, baseline)
        self.assertEqual(judgement.status, "published")
        self.assertEqual(judgement.verdict, "pass")
        self.assertAlmostEqual(judgement.value, 100 / 110, places=6)

    def test_names_the_row_that_failed_and_its_spread(self):
        steady = [cell("zlink-node", "request-window", 1024, f"r{i}", 100.0) for i in range(3)]
        noisy = [
            cell("zlink-c", "request-window", 1024, "c1", 60.0),
            cell("zlink-c", "request-window", 1024, "c2", 100.0),
            cell("zlink-c", "request-window", 1024, "c3", 140.0),
        ]
        judgement = self._judgement(steady, noisy)
        self.assertEqual(judgement.status, "unsupported")
        self.assertIsNone(judgement.verdict)
        self.assertIn("denominator", judgement.reason)
        self.assertIn("zlink-c-request-window@1024", judgement.reason)
        self.assertIn("40.0%", judgement.reason)
        self.assertIsNotNone(judgement.value)

    def test_two_runs_are_not_enough_for_g5(self):
        """Plan 6 fixes three runs; two cannot show a median deviation."""
        two = [cell("zlink-node", "request-window", 1024, f"r{i}", 100.0) for i in range(2)]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        judgement = self._judgement(two, baseline)
        self.assertEqual(judgement.status, "unsupported")
        self.assertIn("G5 needs 3", judgement.reason)

    def test_a_saturated_single_threaded_client_cannot_decide_a_ratio(self):
        """spec 5.1, FB-019: a Node-shaped client pegging its one core.

        On a 20-core machine this reads 4.9% of the machine. The percentage rule
        this replaced would have called it idle and published the ratio.
        """
        saturated = [
            cell(
                "zlink-node", "request-window", 1024, f"r{i}", 100.0,
                client_cpu_percent=4.9, client_cores=0.98, client_parallelism_ceiling=1,
            )
            for i in range(3)
        ]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        judgement = self._judgement(saturated, baseline)
        self.assertEqual(judgement.status, "unsupported")
        self.assertIn("client-saturated", judgement.reason)
        self.assertIn("0.98 of 1 declared client_cores", judgement.reason)
        self.assertIn("spec 5.1", judgement.reason)

    def test_a_declared_metric_other_than_cores_decides_saturation(self):
        """FB-023: the harness names its instrument; the aggregator does not assume.

        The Node shape. Process cores read 1.35 because the binding's native I/O
        threads are counted, but those threads run no user code, so cores against
        a ceiling of 1 would mark this cell no matter what the JS thread was
        doing. The declared instrument is event loop utilization, and here it is
        0.42 -- well below the ceiling -- so the cell is NOT saturated and stays
        eligible to decide a ratio.
        """
        node = [
            cell(
                "zlink-node", "request-window", 1024, f"r{i}", 100.0,
                client_cpu_percent=6.8, client_cores=1.35,
                client_parallelism_ceiling=1.0,
                client_saturation_metric="event_loop_utilization",
                event_loop_utilization=0.42,
            )
            for i in range(3)
        ]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        rows = build_rows(run_set_of(*node, *baseline))
        row = rows[CellKey("zlink-node", "request-window", 1024)]
        self.assertEqual(row.saturation_metric, "event_loop_utilization")
        self.assertFalse(row.client_saturated)
        self.assertEqual(row.saturation_text(), "no")
        judgement = self._judgement(node, baseline)
        self.assertEqual(judgement.status, "published")

    def test_a_saturated_event_loop_blocks_a_ratio(self):
        """FB-023: the same instrument at 0.97 does exclude the cell."""
        node = [
            cell(
                "zlink-node", "request-window", 1024, f"r{i}", 100.0,
                client_cores=1.35, client_parallelism_ceiling=1.0,
                client_saturation_metric="event_loop_utilization",
                event_loop_utilization=0.97,
            )
            for i in range(3)
        ]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        judgement = self._judgement(node, baseline)
        self.assertEqual(judgement.status, "unsupported")
        self.assertIn("0.97 of 1 declared event_loop_utilization", judgement.reason)

    def test_submit_thread_cores_decides_saturation_for_cpp(self):
        """FB-037: the cpp shape, from the measured span.

        ``zlink-cpp`` request-window reads 1.92 PROCESS cores because Core runs
        native I/O threads in the client process, and 0.955 SUBMIT cores because
        exactly one application thread runs the harness's submit loop and its
        completion drain. The declared ceiling is that one thread, so the
        instrument reads 0.955 of 1 and the cell IS saturated: the client
        runtime, not the transport, set this ceiling.
        """
        cpp = [
            cell(
                "zlink-cpp", "request-window", 1024, f"r{i}", 100.0,
                client_cpu_percent=9.6, client_cores=1.92,
                client_parallelism_ceiling=1.0,
                client_saturation_metric="submit_thread_cores",
                submit_thread_cores=0.955,
            )
            for i in range(3)
        ]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        rows = build_rows(run_set_of(*cpp, *baseline))
        row = rows[CellKey("zlink-cpp", "request-window", 1024)]
        self.assertEqual(row.saturation_metric, "submit_thread_cores")
        self.assertTrue(row.saturation_evaluated)
        self.assertTrue(row.client_saturated)
        judgement = self._judgement(cpp, baseline, numerator="zlink-cpp")
        self.assertEqual(judgement.status, "unsupported")
        # The reason names the instrument and the ceiling, so a reader can see
        # WHICH measurement excluded the row rather than only that one did.
        self.assertIn("of 1 declared submit_thread_cores", judgement.reason)
        self.assertIn("client-saturated", judgement.reason)

    def test_process_cores_would_compare_unlike_quantities_across_the_cpp_rows(self):
        """FB-037: why cpp declares an instrument instead of counting the process.

        The two rows a spec 7.2 judgement divides read almost the same on the
        DECLARED instrument (0.955 and 0.695 submit cores) but very differently
        on process cores (1.92 and 0.698), because only the ZLink row carries
        Core's native I/O threads. Reading the gRPC row on process cores leaves
        it unsaturated while the ZLink row is marked -- the mark would then be
        reporting which row links Core, not which row was client-bound.
        """
        grpc = [
            cell(
                "grpc-cpp", "request-window", 1024, f"r{i}", 64.0,
                client_cores=0.698, client_parallelism_ceiling=1.0,
                client_saturation_metric="submit_thread_cores",
                submit_thread_cores=0.695,
            )
            for i in range(3)
        ]
        rows = build_rows(run_set_of(*grpc))
        row = rows[CellKey("grpc-cpp", "request-window", 1024)]
        self.assertFalse(row.client_saturated)
        self.assertEqual(row.saturation_text(), "no")
        # The same row read on process cores against the same declared ceiling
        # would sit at 0.698, still under; the ZLink row at 1.92 would be over.
        # The instrument is what keeps the two commensurate.
        self.assertAlmostEqual(row.values["client_cores"], 0.698)
        self.assertAlmostEqual(row.values["submit_thread_cores"], 0.695)

    def test_a_multi_core_client_below_its_ceiling_is_not_saturated(self):
        """The gated2 shape: 7.38 cores against a declared ceiling of 20."""
        busy = [
            cell(
                "zlink-node", "request-window", 1024, f"r{i}", 100.0,
                client_cpu_percent=36.9, client_cores=7.38, client_parallelism_ceiling=20,
            )
            for i in range(3)
        ]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        rows = build_rows(run_set_of(*busy, *baseline))
        row = rows[CellKey("zlink-node", "request-window", 1024)]
        self.assertTrue(row.saturation_evaluated)
        self.assertFalse(row.client_saturated)
        self.assertEqual(row.saturation_text(), "no")
        judgement = self._judgement(busy, baseline)
        self.assertEqual(judgement.status, "published")

    def test_the_boundary_is_the_declared_fraction_not_the_ceiling(self):
        """0.95 of the ceiling, so 19 of 20 cores is already saturated."""
        for cores, expected in ((18.9, False), (19.0, True), (20.0, True)):
            rows = build_rows(
                run_set_of(
                    *[
                        cell(
                            "zlink-node", "request-window", 1024, f"r{i}", 100.0,
                            client_cores=cores, client_parallelism_ceiling=20,
                        )
                        for i in range(3)
                    ]
                )
            )
            row = rows[CellKey("zlink-node", "request-window", 1024)]
            self.assertEqual(row.client_saturated, expected, f"{cores} cores of 20")

    def test_saturation_is_not_judged_without_a_declared_ceiling(self):
        """Legacy output declares no ceiling. That is unjudged, not unsaturated.

        It must not block a judgement either, or every result measured before
        FB-019 would become unpublishable retroactively.
        """
        legacy = [
            cell("zlink-node", "request-window", 1024, f"r{i}", 100.0, client_cpu_percent=36.9)
            for i in range(3)
        ]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        rows = build_rows(run_set_of(*legacy, *baseline))
        row = rows[CellKey("zlink-node", "request-window", 1024)]
        self.assertFalse(row.saturation_evaluated)
        self.assertFalse(row.client_saturated)
        self.assertEqual(row.saturation_text(), "not judged")
        self.assertEqual(self._judgement(legacy, baseline).status, "published")

    def test_a_client_counted_send_row_cannot_decide_a_ratio(self):
        """G3 and FB-014: without a server receive count the number is a submit rate."""
        counted = [
            cell("zlink-node", "send-saturation", 1024, f"r{i}", 100.0, server_received_at_close=10)
            for i in range(3)
        ]
        uncounted = [cell("zlink-c", "send-saturation", 1024, f"c{i}", 110.0) for i in range(3)]
        rows = build_rows(run_set_of(*counted, *uncounted))
        judgement = judge_pair(
            rows, "zlink-node / zlink-c", "zlink-node", "zlink-c", 1024, pattern="send-saturation"
        )
        self.assertEqual(judgement.status, "unsupported")
        self.assertIn("client-counted", judgement.reason)

    def test_depth_uses_throughput_times_mean_latency(self):
        row = build_rows(
            run_set_of(
                *[
                    Cell(
                        key=CellKey("zlink-node", "request-window", 1024),
                        run=f"r{i}",
                        throughput_per_second=40000.0,
                        latency_mean_ms=0.2,
                    )
                    for i in range(3)
                ]
            )
        )[CellKey("zlink-node", "request-window", 1024)]
        self.assertAlmostEqual(row.in_flight_depth, 8.0, places=6)



class JvmThreadCoresInstrumentTest(unittest.TestCase):
    """FB-023 extended: java declares a third instrument.

    Process cores cannot judge the java client. In a ``zlink-java`` send cell 94%
    of process CPU is Core's native I/O threads running no user code, against 3%
    in ``grpc-java``, so ``client_cores`` compares unlike quantities between the
    two rows a 0.80 ratio divides; and the largest reading ever observed on this
    machine was 0.154 of a 20-core ceiling, so the mark could not fire at all.
    """

    def _cell(self, **kwargs):
        cell = Cell(key=CellKey("zlink-java", "request-window", 1024), run="java-router-1")
        cell.throughput_per_second = 1000.0
        for name, value in kwargs.items():
            setattr(cell, name, value)
        return cell

    def test_declared_instrument_is_read_from_its_own_field(self):
        cell = self._cell(
            client_saturation_metric="jvm_thread_cores",
            jvm_thread_cores=0.97,
            client_cores=2.4,
            client_parallelism_ceiling=1.0,
        )
        self.assertEqual(cell.saturation_metric, "jvm_thread_cores")
        self.assertEqual(cell.saturation_value, 0.97)
        self.assertTrue(cell.saturation_evaluated)
        # 0.97 >= 0.95 * 1.0
        self.assertTrue(cell.client_saturated)

    def test_process_cores_do_not_decide_when_jvm_cores_are_declared(self):
        # Process cores are far above the ceiling because Core's native I/O
        # threads are counted; the declared instrument must ignore them.
        cell = self._cell(
            client_saturation_metric="jvm_thread_cores",
            jvm_thread_cores=0.10,
            client_cores=2.17,
            client_parallelism_ceiling=1.0,
        )
        self.assertFalse(cell.client_saturated)

    def test_missing_reading_is_not_judged_rather_than_unsaturated(self):
        cell = self._cell(
            client_saturation_metric="jvm_thread_cores",
            client_parallelism_ceiling=1.0,
        )
        self.assertFalse(cell.saturation_evaluated)
        self.assertFalse(cell.client_saturated)

    def test_send_cell_is_judged_against_the_send_concurrency(self):
        cell = Cell(key=CellKey("zlink-java", "send-saturation", 4096), run="java-router-1")
        cell.throughput_per_second = 1000.0
        cell.client_saturation_metric = "jvm_thread_cores"
        cell.jvm_thread_cores = 7.7
        cell.client_parallelism_ceiling = 8.0
        self.assertTrue(cell.client_saturated)
        cell.jvm_thread_cores = 3.0
        self.assertFalse(cell.client_saturated)

    def test_reader_accepts_the_field_from_cells_json(self):
        payload = {
            "schema": "with-grpc-cell-v1",
            "cells": [
                {
                    "implementation": "zlink-java",
                    "pattern": "request-window",
                    "payload_size": 1024,
                    "throughput_per_second": 1234.0,
                    "bandwidth_mb_s": 1.263,
                    "client_saturation_metric": "jvm_thread_cores",
                    "jvm_thread_cores": 0.42,
                    "client_parallelism_ceiling": 1,
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            cell_dir = pathlib.Path(directory) / "run1" / "zlink-java-request-window-1024"
            cell_dir.mkdir(parents=True)
            (cell_dir / "results.json").write_text(json.dumps(payload), encoding="utf-8")
            cells, _notes = read_run(cell_dir.parent)
        self.assertEqual(len(cells), 1)
        self.assertEqual(cells[0].jvm_thread_cores, 0.42)
        self.assertEqual(cells[0].saturation_value, 0.42)


class RunLayoutTest(unittest.TestCase):
    """The only input is <run>/<cell>/results.json (README section 11)."""

    def _run(self, directory, files):
        run = pathlib.Path(directory) / "run1"
        for relative, text in files.items():
            path = run / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return run

    def test_a_contaminated_cell_with_no_metrics_is_still_reported(self):
        """FB-008: a contaminated cell was never measured but must stay visible."""
        payload = {
            "schema": "with-grpc-cell-v1",
            "cells": [
                {
                    "implementation": "zlink-node",
                    "pattern": "request-serial",
                    "payload_size": 4096,
                    "contaminated": True,
                    "contamination_reason": "previous cell did not drain",
                }
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            run = self._run(directory, {"zlink-node-request-serial-4096/results.json": json.dumps(payload)})
            cells, _ = read_run(str(run))
        self.assertEqual(len(cells), 1)
        self.assertTrue(cells[0].contaminated)
        self.assertIsNone(cells[0].throughput_per_second)

    def test_a_run_with_only_report_text_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            run = self._run(directory, {"report.txt": "RESULT,current,zlink-c-request-serial,local,1024,throughput,1\n",
                                        "zlink-c-request-serial-1024/report.txt": "x"})
            with self.assertRaisesRegex(ReportError, "no results.json"):
                read_run(str(run))

    def test_a_results_json_that_is_not_a_cell_document_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            run = self._run(directory, {"cell-a/results.json": json.dumps({"metadata": {}, "results": []})})
            with self.assertRaisesRegex(ReportError, "unsupported cell schema"):
                read_run(str(run))


if __name__ == "__main__":
    unittest.main()
