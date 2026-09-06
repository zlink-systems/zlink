"""Acceptance: Phase 0 raw output must reproduce the Phase 0 summary.

Plan Phase 1 fixes the condition for adopting this aggregator -- feed it the
``gated2`` material and the tables and judgements must be the ones in
``doc/plan/fw-bench-worklog/bench-dotnet-summary.ko.md``. The expected values
below are that document's sections 3.1, 3.2, 3.3, 3.4, 4, 5.1 and 5.2,
transcribed so the check survives the plan document being archived.

One value is deliberately not the summary's. Section 3.4 gives the measured
in-flight depth of ``zlink-framework-dotnet`` at 4096 as 98.6; that comes from
multiplying the table's already-rounded 2.458 KOPS by 40.134 ms. From the
unrounded medians (2458.2/s) the depth is 98.657, which is 98.7. Every other
value in 3.4 is identical under both methods. See ``test_summary_3_4_depth``.
"""

from __future__ import annotations

import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)

from benchagg.analysis import build_rows, judge_language, language_verdict  # noqa: E402
from benchagg.model import CellKey  # noqa: E402
from benchagg.readers import read_runs  # noqa: E402

FIXTURE = os.path.join(_TOOLS, "tests", "fixtures", "gated2")
RUNS = [
    os.path.join(FIXTURE, name)
    for name in (
        "c-router-1",
        "c-router-2",
        "c-router-3",
        "dotnet-router-1",
        "dotnet-router-2",
        "dotnet-router-3",
    )
]

# summary 3.1 and 3.2: throughput (KOPS or KMSG/s), mean, p95, p99, client CPU%,
# client MB, server CPU%, server MB, drain ms.
SUMMARY_CELLS = {
    ("request-serial", "grpc-dotnet", 1024): (6.758, 0.146, 0.272, 0.323, 6.5, 115.8, 5.5, 176.1, None),
    ("request-serial", "zlink-dotnet", 1024): (7.595, 0.130, 0.156, 0.188, 3.6, 120.6, 1.7, 120.8, None),
    ("request-serial", "zlink-framework-dotnet", 1024): (1.995, 0.500, 0.656, 0.834, 3.4, 142.2, 8.2, 437.2, None),
    ("request-window", "grpc-dotnet", 1024): (198.787, 0.471, 1.442, 1.926, 36.9, 158.4, 28.8, 482.4, None),
    ("request-window", "zlink-dotnet", 1024): (36.034, 0.221, 0.356, 0.420, 5.8, 166.0, 2.2, 341.1, None),
    ("request-window", "zlink-framework-dotnet", 1024): (3.663, 28.045, 104.709, 174.524, 3.7, 195.4, 5.2, 486.1, None),
    ("send-saturation", "grpc-dotnet", 1024): (45.335, 0.090, 0.112, 0.147, 9.5, 198.2, 7.6, 488.5, 240),
    ("send-saturation", "zlink-dotnet", 1024): (411.875, 0.377, 1.141, 9.716, 16.0, 207.7, 6.8, 500.8, 360),
    ("send-saturation", "zlink-framework-dotnet", 1024): (46.629, 1770.695, 3359.400, 3513.137, 31.9, 1284.3, 50.9, 2163.5, 16674),
    ("request-serial", "grpc-dotnet", 4096): (6.857, 0.143, 0.190, 0.234, 5.7, 1289.4, 5.4, 504.0, None),
    ("request-serial", "zlink-dotnet", 4096): (7.534, 0.130, 0.158, 0.235, 4.0, 1289.4, 1.6, 548.3, None),
    ("request-serial", "zlink-framework-dotnet", 4096): (2.207, 0.451, 0.554, 0.646, 3.6, 1339.8, 7.6, 2147.3, None),
    ("request-window", "grpc-dotnet", 4096): (127.193, 0.753, 1.732, 2.097, 33.9, 1340.6, 29.2, 509.6, None),
    ("request-window", "zlink-dotnet", 4096): (34.390, 0.225, 0.362, 0.439, 5.7, 1343.7, 2.1, 487.3, None),
    ("request-window", "zlink-framework-dotnet", 4096): (2.458, 40.134, 105.397, 129.874, 2.8, 1402.2, 3.9, 1965.5, None),
    ("send-saturation", "grpc-dotnet", 4096): (41.515, 0.102, 0.129, 0.171, 9.7, 1405.2, 8.8, 511.6, 216),
    ("send-saturation", "zlink-dotnet", 4096): (383.077, 0.615, 4.302, 8.406, 19.0, 1407.8, 8.2, 502.3, 363),
    ("send-saturation", "zlink-framework-dotnet", 4096): (43.863, 1475.071, 3071.936, 3190.037, 30.8, 3963.1, 51.9, 4051.0, 13286),
}

# summary 3.3: the C baseline the judgement divides by.
SUMMARY_BASELINE = {1024: (430.617, 5.6, "pass"), 4096: (310.523, 25.7, "fail")}

# summary 3.4: request-window depth, throughput x mean latency.
SUMMARY_DEPTH = {
    ("grpc-c", 1024): 98.9,
    ("grpc-c", 4096): 98.8,
    ("zlink-c", 1024): 83.5,
    ("zlink-c", 4096): 87.6,
    ("grpc-dotnet", 1024): 93.6,
    ("grpc-dotnet", 4096): 95.8,
    ("zlink-dotnet", 1024): 8.0,
    ("zlink-dotnet", 4096): 7.7,
    ("zlink-framework-dotnet", 1024): 102.7,
    ("zlink-framework-dotnet", 4096): 98.7,  # summary prints 98.6; see module docstring
}

# summary 5.1 (rows that meet G5) and 5.2 (rows that do not).
SUMMARY_G5_PASS = {
    ("grpc-dotnet", "request-serial", 1024): 1.4, ("grpc-dotnet", "request-serial", 4096): 0.5,
    ("grpc-dotnet", "request-window", 1024): 1.7, ("grpc-dotnet", "request-window", 4096): 3.6,
    ("grpc-dotnet", "send-saturation", 1024): 2.3, ("grpc-dotnet", "send-saturation", 4096): 1.2,
    ("zlink-dotnet", "request-serial", 1024): 7.8, ("zlink-dotnet", "request-serial", 4096): 2.0,
    ("zlink-dotnet", "request-window", 1024): 1.0, ("zlink-dotnet", "request-window", 4096): 2.0,
    ("zlink-dotnet", "send-saturation", 1024): 5.0, ("zlink-dotnet", "send-saturation", 4096): 2.9,
    ("zlink-framework-dotnet", "request-serial", 1024): 2.6,
    ("zlink-framework-dotnet", "request-serial", 4096): 2.0,
    ("zlink-framework-dotnet", "send-saturation", 1024): 2.0,
    ("zlink-framework-dotnet", "send-saturation", 4096): 2.4,
    ("grpc-c", "request-window", 1024): 3.1, ("grpc-c", "request-window", 4096): 2.0,
    ("grpc-c", "send-saturation", 1024): 2.8, ("grpc-c", "send-saturation", 4096): 2.9,
    ("zlink-c", "request-window", 1024): 5.6, ("zlink-c", "send-saturation", 4096): 1.6,
}
SUMMARY_G5_FAIL = {
    ("zlink-framework-dotnet", "request-window", 1024): 29.6,
    ("zlink-framework-dotnet", "request-window", 4096): 11.5,
    ("zlink-c", "request-window", 4096): 25.7,
    ("zlink-c", "request-serial", 1024): 23.5,
    ("zlink-c", "request-serial", 4096): 14.6,
    ("zlink-c", "send-saturation", 1024): 20.9,
    ("grpc-c", "request-serial", 1024): 173.8,
    ("grpc-c", "request-serial", 4096): 165.0,
}


class AcceptanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.run_set = read_runs(RUNS)
        cls.rows = build_rows(cls.run_set)
        cls.judgements = judge_language(cls.rows, "dotnet")

    def row(self, impl, pattern, size):
        row = self.rows.get(CellKey(impl, pattern, size))
        self.assertIsNotNone(row, f"{impl}-{pattern}@{size} missing")
        return row

    def test_eighteen_dotnet_cells(self):
        """Plan G1: three patterns x two payloads x three implementations."""
        keys = [k for k in self.rows if k.implementation.endswith("dotnet")]
        self.assertEqual(len(keys), 18)
        for key in keys:
            self.assertEqual(self.rows[key].run_count, 3, f"{key} was not measured 3 times")

    def test_summary_3_1_and_3_2_tables(self):
        for (pattern, impl, size), expected in SUMMARY_CELLS.items():
            row = self.row(impl, pattern, size)
            value = row.values
            got = (
                round(value["throughput_per_second"] / 1000.0, 3),
                round(value["latency_mean_ms"], 3),
                round(value["latency_p95_ms"], 3),
                round(value["latency_p99_ms"], 3),
                round(value["client_cpu_percent"], 1),
                round(value["client_memory_mb"], 1),
                round(value["server_cpu_percent"], 1),
                round(value["server_memory_mb"], 1),
                None if value["drain_ms"] is None else round(value["drain_ms"]),
            )
            self.assertEqual(got, expected, f"{impl}-{pattern}@{size}")

    def test_summary_3_3_baseline(self):
        for size, (throughput, spread, status) in SUMMARY_BASELINE.items():
            row = self.row("zlink-c", "request-window", size)
            self.assertAlmostEqual(row.throughput / 1000.0, throughput, places=3)
            self.assertAlmostEqual(row.spread_percent, spread, delta=0.05)
            self.assertEqual(row.g5_status, status)

    def test_summary_3_4_depth(self):
        for (impl, size), expected in SUMMARY_DEPTH.items():
            row = self.row(impl, "request-window", size)
            self.assertAlmostEqual(row.in_flight_depth, expected, delta=0.05, msg=f"{impl}@{size}")

    def test_summary_5_1_and_5_2_g5(self):
        for (impl, pattern, size), spread in SUMMARY_G5_PASS.items():
            row = self.row(impl, pattern, size)
            self.assertAlmostEqual(row.spread_percent, spread, delta=0.05, msg=f"{impl} {pattern}@{size}")
            self.assertEqual(row.g5_status, "pass", f"{impl} {pattern}@{size}")
        for (impl, pattern, size), spread in SUMMARY_G5_FAIL.items():
            row = self.row(impl, pattern, size)
            self.assertAlmostEqual(row.spread_percent, spread, delta=0.05, msg=f"{impl} {pattern}@{size}")
            self.assertEqual(row.g5_status, "fail", f"{impl} {pattern}@{size}")

    def test_summary_4_judgements(self):
        """One published value, three unsupported, each naming the row that failed."""
        expected = [
            ("zlink-dotnet / zlink-c", 1024, "published", "fail", 0.084, None),
            ("zlink-dotnet / zlink-c", 4096, "unsupported", None, 0.111, ("denominator", "zlink-c", 25.7)),
            ("zlink-framework-dotnet / zlink-dotnet", 1024, "unsupported", None, 0.102,
             ("numerator", "zlink-framework-dotnet", 29.6)),
            ("zlink-framework-dotnet / zlink-dotnet", 4096, "unsupported", None, 0.071,
             ("numerator", "zlink-framework-dotnet", 11.5)),
        ]
        self.assertEqual(len(self.judgements), len(expected))
        by_key = {(j.formula, j.payload_size): j for j in self.judgements}
        for formula, size, status, verdict, value, blame in expected:
            judgement = by_key[(formula, size)]
            self.assertEqual(judgement.status, status, f"{formula}@{size}")
            self.assertEqual(judgement.verdict, verdict, f"{formula}@{size}")
            self.assertAlmostEqual(judgement.value, value, delta=0.0005, msg=f"{formula}@{size}")
            if blame:
                role, impl, spread = blame
                self.assertIn(role, judgement.reason)
                self.assertIn(impl, judgement.reason)
                self.assertIn(f"{spread:.1f}%", judgement.reason)

    def test_language_verdict_is_not_a_pass(self):
        status, reason = language_verdict(self.judgements)
        self.assertEqual(status, "incomplete")
        self.assertIn("both payload sizes", reason)

    def test_no_contaminated_cells(self):
        """FB-008: the gated2 runs drained inside the bound, so nothing is excluded."""
        self.assertEqual(self.run_set.contaminated(), [])

    def test_peak_in_flight_against_measured_depth(self):
        """FB-017 against G8: the window was reached, the depth was not."""
        row = self.row("zlink-dotnet", "request-window", 1024)
        self.assertEqual(row.peak_in_flight, 100)
        self.assertEqual(row.request_window, 100)
        self.assertEqual(row.abandoned, 0)
        self.assertLess(row.in_flight_depth, 10)

    def test_send_throughput_provenance(self):
        """G3 / FB-014: the C runner reports no server receive count."""
        self.assertTrue(self.row("zlink-dotnet", "send-saturation", 1024).send_server_counted)
        self.assertFalse(self.row("zlink-c", "send-saturation", 1024).send_server_counted)

    def test_out_of_spec_c_patterns_are_dropped_visibly(self):
        patterns = {k.pattern for k in self.rows}
        self.assertNotIn("request-saturation", patterns)
        self.assertNotIn("send-blocking", patterns)
        self.assertTrue(any("out-of-spec" in note for note in self.run_set.notes))


if __name__ == "__main__":
    unittest.main()
