import unittest

from perf_multi_measurement import CatchUp


class PubSubCatchUpTest(unittest.TestCase):
    def test_one_observation_per_subscriber_may_be_missing(self):
        for delta, expected in ((9, True), (8, False)):
            with self.subTest(delta=delta):
                tracker = CatchUp()
                self.assertFalse(tracker.observe(0, [92, 100], 100, 20))
                self.assertEqual(tracker.observe(0.5, [102, 100 + delta], 110, 20), expected)

    def test_growth_restarts_the_window(self):
        tracker = CatchUp()
        self.assertFalse(tracker.observe(0, [92, 92], 100, 20))
        self.assertFalse(tracker.observe(0.125, [93, 93], 102, 20))
        self.assertFalse(tracker.observe(0.5, [102, 102], 110, 20))
        self.assertTrue(tracker.observe(0.625, [104, 104], 112, 20))
