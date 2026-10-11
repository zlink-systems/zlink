"""Validate active receive metrics independently of source admission and drain."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchagg.readers import ReportError, cells_from_cell_json


class CSendMetricsTest(unittest.TestCase):
    def payload(self):
        return {"schema": "with-grpc-cell-v1", "metadata": {"durationSeconds": 5},
                "cells": [{"implementation": "zlink-c", "pattern": "send-saturation",
                           "payload_size": 1024, "server_received_at_close": 7,
                           "throughput_per_second": 2.2,
                           "target_stats": {"received": 11, "errors": 0, "drainMs": 1},
                           "extra": {"submitted": 11, "completed": 7}}]}

    def test_rate_uses_active_receipts_and_checks_settled_count(self):
        payload = self.payload()
        cell = cells_from_cell_json(payload, "run1")[0]
        self.assertEqual(1.4, cell.throughput_per_second)
        payload["cells"][0]["server_received_at_close"] = 12
        with self.assertRaises(ReportError):
            cells_from_cell_json(payload, "run1")


if __name__ == "__main__":
    unittest.main()
