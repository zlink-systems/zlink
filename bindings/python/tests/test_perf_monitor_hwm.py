import sys
import unittest
from pathlib import Path


PERF_MULTI_DIR = Path(__file__).resolve().parents[1] / "perf" / "multi"
sys.path.insert(0, str(PERF_MULTI_DIR))

from perf_multi_common import resolve_multi_monitor_hwm_bytes


class PerfMonitorHwmTests(unittest.TestCase):
    def test_resolves_exact_nonnegative_byte_values(self):
        self.assertEqual(resolve_multi_monitor_hwm_bytes({}), 4_096_000)
        self.assertEqual(
            resolve_multi_monitor_hwm_bytes({"PERF_MONITOR_HWM_BYTES": "123"}),
            123,
        )
        self.assertEqual(
            resolve_multi_monitor_hwm_bytes(
                {
                    "PERF_MULTI_MONITOR_HWM_BYTES": "456",
                    "PERF_MONITOR_HWM_BYTES": "123",
                }
            ),
            456,
        )
        self.assertEqual(
            resolve_multi_monitor_hwm_bytes({"PERF_MULTI_MONITOR_HWM_BYTES": "0"}),
            0,
        )
        self.assertEqual(
            resolve_multi_monitor_hwm_bytes(
                {
                    "PERF_MULTI_MONITOR_HWM_BYTES": "invalid",
                    "PERF_MONITOR_HWM_BYTES": "789",
                }
            ),
            789,
        )


if __name__ == "__main__":
    unittest.main()
