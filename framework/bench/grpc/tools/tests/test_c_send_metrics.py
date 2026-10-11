"""Validate active receive metrics independently of source admission and drain."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from benchagg.readers import cells_from_cell_json


class CSendMetricsTest(unittest.TestCase):
    def payload(self):
        return {"schema": "with-grpc-cell-v1", "metadata": {"durationSeconds": 5},
                "cells": [{"implementation": "zlink-c", "pattern": "send-saturation",
                           "payload_size": 1024, "server_received_at_close": 7,
                           "throughput_per_second": 2.2, "bandwidth_mb_s": 0.0022528,
                           "target_stats": {"received": 11, "errors": 3, "drainMs": 1},
                           "extra": {"submitted": 11, "completed": 7}}]}

    def test_rate_is_read_from_the_authoritative_cell(self):
        payload = self.payload()
        cell = cells_from_cell_json(payload, "run1")[0]
        self.assertEqual(2.2, cell.throughput_per_second)
        self.assertEqual(7, cell.server_received_at_close)
        self.assertEqual(11, cell.target_stats["received"])
        self.assertEqual(3, cell.target_errors)


if __name__ == "__main__":
    unittest.main()
