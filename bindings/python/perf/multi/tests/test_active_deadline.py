import unittest

from perf_multi_measurement import PHASE_ACTIVE, record_receive
from perf_metrics import new_payload, stamp_payload


class ActiveDeadlineTest(unittest.TestCase):
    def test_active_receive_excludes_the_deadline_and_tail(self):
        payload = stamp_payload(new_payload(64), PHASE_ACTIVE, run_id=1, sent_ts_ns=50)
        self.assertTrue(record_receive(payload, 64, 1, 99, 100, None))
        self.assertFalse(record_receive(payload, 64, 1, 100, 100, None))
        self.assertFalse(record_receive(payload, 64, 1, 101, 100, None))
