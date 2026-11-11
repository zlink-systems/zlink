from __future__ import annotations

import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)

from benchagg.analysis import Row, judge_language, language_verdict  # noqa: E402
from benchagg.model import CellKey  # noqa: E402
from benchagg.render import render_judgement_table  # noqa: E402


class JudgementCriteriaTest(unittest.TestCase):
    def rows(self, ratios: dict[tuple[str, int], float] | None = None):
        ratios = ratios or {}
        rows = {}
        for size in (1024, 4096):
            for pattern in ("request-serial", "request-backpressure", "send-saturation"):
                binding_ratio = ratios.get(("binding", size), 0.80)
                framework_ratio = ratios.get((pattern, size), 0.60)
                for implementation, throughput in (
                    ("zlink-c", 100.0),
                    ("zlink-node", 100.0 * binding_ratio),
                    ("zlink-framework-node", 100.0 * binding_ratio * framework_ratio),
                ):
                    key = CellKey(implementation, pattern, size)
                    rows[key] = Row(
                        key=key,
                        run_count=3,
                        values={"throughput_per_second": throughput},
                        spread_percent=0.0,
                        g5_status="pass",
                        abandoned=0,
                        errors=0,
                        send_server_counted=True,
                    )
        return rows

    def judgement(self, rows, impl, pattern, size):
        return next(j for j in judge_language(rows, "node")
                    if j.numerator.implementation == impl
                    and j.numerator.pattern == pattern
                    and j.payload_size == size)

    def test_binding_threshold_boundary(self):
        rows = self.rows({("binding", 1024): 0.79})
        self.assertEqual(self.judgement(rows, "zlink-node", "request-backpressure", 1024).verdict, "fail")
        self.assertEqual(self.judgement(self.rows(), "zlink-node", "request-backpressure", 1024).verdict, "pass")

    def test_framework_threshold_boundary(self):
        rows = self.rows({("send-saturation", 1024): 0.59})
        self.assertEqual(self.judgement(rows, "zlink-framework-node", "send-saturation", 1024).verdict, "fail")
        self.assertEqual(self.judgement(self.rows(), "zlink-framework-node", "send-saturation", 1024).verdict, "pass")

    def test_one_payload_failure_fails_language(self):
        judgements = judge_language(self.rows({("binding", 4096): 0.79}), "node")
        status, _ = language_verdict(judgements)
        self.assertEqual(status, "fail")

    def test_one_framework_pattern_failure_fails_language(self):
        judgements = judge_language(self.rows({("request-serial", 4096): 0.59}), "node")
        status, _ = language_verdict(judgements)
        self.assertEqual(status, "fail")

    def test_grpc_backpressure_is_rendered_n_a_and_not_judged(self):
        judgements = judge_language(self.rows(), "node")
        table = render_judgement_table(judgements)
        self.assertIn("grpc-<lang>", table)
        self.assertIn("| request-backpressure | 1024 | n/a | n/a | n/a |", table)
        self.assertEqual(len(judgements), 8)

    def test_kotlin_supplementary_cells_are_not_layer_judgements(self):
        judgements = judge_language(self.rows(), "kotlin")
        self.assertEqual(judgements, [])
        self.assertEqual(language_verdict(judgements)[0], "n/a")


if __name__ == "__main__":
    unittest.main()
