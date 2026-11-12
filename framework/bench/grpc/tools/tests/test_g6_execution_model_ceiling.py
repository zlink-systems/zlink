from __future__ import annotations

import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)

from benchagg.analysis import Row, _block_reason  # noqa: E402
from benchagg.model import CellKey  # noqa: E402


def row(
    declared: float | None,
    model: float | None,
    reading: float,
    metric: str = "client_cores",
) -> Row:
    return Row(
        key=CellKey("zlink-client", "request-backpressure", 1024),
        run_count=3,
        values={"throughput_per_second": 100.0, metric: reading},
        spread_percent=0.0,
        g5_status="pass",
        client_saturation_metric=metric,
        client_parallelism_ceiling=declared,
        execution_model_ceiling=model,
        abandoned=0,
        errors=0,
        send_server_counted=True,
    )


class G6ExecutionModelCeilingTest(unittest.TestCase):
    def test_node_event_loop_at_its_declared_model_ceiling_is_judged_and_marked(self):
        cell = row(1.0, 1.0, 0.96, "event_loop_utilization")
        self.assertTrue(cell.client_saturated)
        self.assertTrue(cell.execution_model_ceiling_reached)
        self.assertFalse(cell.g6_excluded)
        self.assertEqual(cell.saturation_text(), "실행 모델 상한 도달")
        self.assertIsNone(_block_reason("numerator", cell, cell.key))

    def test_declaring_less_than_the_model_excludes_a_saturated_cell(self):
        cell = row(4.0, 8.0, 3.9)
        self.assertTrue(cell.g6_excluded)
        self.assertIn("G6", _block_reason("numerator", cell, cell.key))

    def test_missing_declaration_excludes_a_saturated_cell(self):
        cell = row(None, 1.0, 0.96)
        self.assertTrue(cell.g6_excluded)
        self.assertIn("G6", _block_reason("numerator", cell, cell.key))

    def test_cpp_java_dotnet_at_their_stream_ceiling_remain_judged(self):
        for language in ("cpp", "java", "dotnet"):
            with self.subTest(language=language):
                cell = row(8.0, 8.0, 7.7)
                self.assertTrue(cell.execution_model_ceiling_reached)
                self.assertFalse(cell.g6_excluded)
                self.assertIsNone(_block_reason("numerator", cell, cell.key))


if __name__ == "__main__":
    unittest.main()
