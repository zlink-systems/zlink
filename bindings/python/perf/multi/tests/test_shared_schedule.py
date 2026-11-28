import unittest

from perf_multi_measurement import Schedule, wait_ms


class SharedScheduleTest(unittest.TestCase):
    def test_epoch_rate_and_wait_floor(self):
        epoch = 1.0
        schedule = Schedule(epoch, 100, 2)
        self.assertEqual(schedule.rate, 40)
        self.assertFalse(schedule.due(epoch - 0.000_000_001))
        self.assertEqual(schedule.reserve(), epoch)
        self.assertEqual(schedule.reserve(), epoch + 0.025)
        self.assertEqual(schedule.next(), epoch + 0.05)
        self.assertEqual(wait_ms(epoch, epoch + 0.000_999_999), 0)
        self.assertEqual(wait_ms(epoch, epoch + 0.001), 1)
        self.assertEqual(wait_ms(epoch, epoch + 0.001_999_999), 1)
        self.assertEqual(wait_ms(epoch, epoch + 0.15), 50)
