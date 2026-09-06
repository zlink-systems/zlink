"""Behaviour the Phase 0 material cannot exercise.

``gated2`` has no contaminated cell, no client-saturated cell and no report with
an unreadable unit, so those paths are driven from synthetic reports here. They
are the paths that decide whether a later language can publish a number Phase 0's
rules would have rejected, so they are tested even though nothing has hit them.
"""

from __future__ import annotations

import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)

from benchagg.analysis import build_rows, judge_pair, spread_percent  # noqa: E402
from benchagg.model import Cell, CellKey, RunSet  # noqa: E402
from benchagg.readers import (  # noqa: E402
    ReportError,
    cells_from_report,
    detect_throughput_scale,
    parse_contaminated,
    parse_diagnostics,
    parse_result_lines,
    split_scenario,
)


def result_lines(scenario, size, throughput, bandwidth, **metrics):
    rows = {"throughput": throughput, "bandwidth": bandwidth}
    rows.update(metrics)
    return "".join(
        f"RESULT,current,{scenario},local,{size},{metric},{value:.3f}\n"
        for metric, value in rows.items()
    )


class ScenarioTest(unittest.TestCase):
    def test_splits_language_out_of_the_scenario_name(self):
        self.assertEqual(
            split_scenario("zlink-framework-dotnet-request-window"),
            ("zlink-framework-dotnet", "request-window"),
        )
        self.assertEqual(split_scenario("grpc-c-send-saturation"), ("grpc-c", "send-saturation"))

    def test_rejects_patterns_the_spec_does_not_define(self):
        """The C bench emits two extra patterns; a loose suffix rule would fold
        ``request-saturation`` into a table that spec 2 does not have."""
        self.assertIsNone(split_scenario("grpc-c-request-saturation"))
        self.assertIsNone(split_scenario("zlink-c-send-blocking"))
        self.assertIsNone(split_scenario("request-window"))


class UnitTest(unittest.TestCase):
    def test_reads_kops_from_a_c_shaped_report(self):
        text = result_lines("zlink-c-request-window", 1024, 454.606, 465.517)
        scale, name = detect_throughput_scale(parse_result_lines(text))
        self.assertEqual((scale, name), (1000.0, "KOPS"))

    def test_reads_per_second_from_a_dotnet_shaped_report(self):
        text = result_lines("zlink-dotnet-request-window", 1024, 35994.0, 36.858)
        scale, name = detect_throughput_scale(parse_result_lines(text))
        self.assertEqual((scale, name), (1.0, "per-second"))

    def test_normalizes_both_shapes_to_the_same_number(self):
        c_cells, _ = cells_from_report(
            result_lines("zlink-c-request-window", 1024, 100.0, 102.4), "c"
        )
        net_cells, _ = cells_from_report(
            result_lines("zlink-node-request-window", 1024, 100000.0, 102.4), "node"
        )
        self.assertAlmostEqual(
            c_cells[0].throughput_per_second, net_cells[0].throughput_per_second, places=3
        )

    def test_refuses_a_report_whose_unit_it_cannot_establish(self):
        """Neither guessing nor defaulting: an unreadable unit stops the report."""
        text = result_lines("zlink-c-request-window", 1024, 100.0, 5.0)
        with self.assertRaises(ReportError):
            detect_throughput_scale(parse_result_lines(text))

    def test_refuses_a_report_that_mixes_units(self):
        text = result_lines("zlink-c-request-window", 1024, 100.0, 102.4) + result_lines(
            "grpc-c-request-window", 1024, 100000.0, 102.4
        )
        with self.assertRaises(ReportError):
            detect_throughput_scale(parse_result_lines(text))


class DiagnosticsTest(unittest.TestCase):
    STDOUT = """[bench] request payload=1024 mode=window window=100
[bench] window zlink-node-request-window: peak_in_flight=42 of 100 abandoned=3
[bench] send payload=1024 concurrency=8
[bench] drain zlink-node-send-saturation: 900 ms bound_hit=False
[bench] boundary zlink-node-send-saturation: server_received_at_close=500 post_drain=900 drain_ms=900
[bench] request payload=4096 mode=window window=100
[bench] window zlink-node-request-window: peak_in_flight=77 of 100 abandoned=0
[bench] send payload=4096 concurrency=8
[bench] drain zlink-node-send-saturation: 31000 ms bound_hit=True
"""

    def test_attributes_a_line_to_the_payload_section_that_precedes_it(self):
        """The lines carry no payload of their own; the section marker supplies it."""
        found = parse_diagnostics(self.STDOUT)
        self.assertEqual(found[("zlink-node-request-window", 1024)]["peak_in_flight"], 42)
        self.assertEqual(found[("zlink-node-request-window", 1024)]["abandoned"], 3)
        self.assertEqual(found[("zlink-node-request-window", 4096)]["peak_in_flight"], 77)
        self.assertEqual(found[("zlink-node-send-saturation", 1024)]["drain_ms"], 900.0)
        self.assertFalse(found[("zlink-node-send-saturation", 1024)]["drain_bound_hit"])
        self.assertTrue(found[("zlink-node-send-saturation", 4096)]["drain_bound_hit"])

    def test_reads_the_contaminated_section(self):
        text = (
            "## Drain (FB-008)\n- a: drained in 5 ms\n\n"
            "## Contaminated (excluded from tables and judgement)\n"
            "- zlink-node-request-window@4096: previous cell did not drain in 30000 ms\n"
        )
        self.assertEqual(
            parse_contaminated(text),
            {"zlink-node-request-window@4096": "previous cell did not drain in 30000 ms"},
        )


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

    def _judgement(self, numerator_cells, denominator_cells):
        rows = build_rows(run_set_of(*numerator_cells, *denominator_cells))
        return judge_pair(rows, "zlink-node / zlink-c", "zlink-node", "zlink-c", 1024)

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

    def test_a_client_saturated_row_cannot_decide_a_ratio(self):
        """spec 5.1 and G6: that cell measured the client, not the transport."""
        saturated = [
            cell("zlink-node", "request-window", 1024, f"r{i}", 100.0, client_cpu_percent=99.0)
            for i in range(3)
        ]
        baseline = [cell("zlink-c", "request-window", 1024, f"c{i}", 110.0) for i in range(3)]
        judgement = self._judgement(saturated, baseline)
        self.assertEqual(judgement.status, "unsupported")
        self.assertIn("client-saturated", judgement.reason)
        self.assertIn("spec 5.1", judgement.reason)

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


if __name__ == "__main__":
    unittest.main()
