from types import SimpleNamespace
import unittest

import zlink
from perf_multi_common import MultiSendTurnCoordinator


class ActiveRotationTest(unittest.TestCase):
    def test_each_sender_gets_a_bounded_turn(self):
        coordinator = MultiSendTurnCoordinator(None, None, 2)
        visits = []

        def submit(slot):
            visits.append(slot)
            return SimpleNamespace(result=zlink.SubmitResult.OK)

        coordinator._submit_round(float("inf"), submit)
        self.assertEqual(visits, [0] * 64 + [1] * 64)
