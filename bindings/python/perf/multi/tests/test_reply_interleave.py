import unittest

from perf_multi_common import RelaySchedulerQuantum
from perf_multi_measurement import MAX_BURST_PER_SENDER


class ReplyInterleaveTest(unittest.TestCase):
    def test_paced_reply_turn_rechecks_at_64_and_128(self):
        replies = RelaySchedulerQuantum(MAX_BURST_PER_SENDER)
        rechecks = [i for i in range(1, 131) if replies.received()]
        self.assertEqual(rechecks, [64, 128])
