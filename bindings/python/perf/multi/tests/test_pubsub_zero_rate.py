import unittest

from perf_multi_measurement import CatchUp


class PubSubZeroRateTest(unittest.TestCase):
    def test_zero_rate_cannot_catch_up(self):
        tracker = CatchUp()
        tracker.observe(0, [0], 0, 0)
        self.assertFalse(tracker.observe(0.5, [0], 0, 0))
