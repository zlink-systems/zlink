"""§15.4 PS aggregation: subscriber first receipts intersect the publisher's window-success set (both directions)."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runner"))
from results import fanout_aggregate, range_intersection_size, range_size, sequence_ranges, settle_status  # noqa: E402

CONFIG = {"runId": "r", "cellId": "c", "subscriberCount": 2}
LATENCY_KEYS = [f"{prefix}.{suffix}" for prefix in ("fanout.deliveryLatency", "fanout.settleDeliveryLatency")
                for suffix in ("meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs")]


def ranges(*pairs):
    return [{"first": str(first), "last": str(last)} for first, last in pairs]


def identity(**fields):
    return {"runId": "r", "cellId": "c", "resetSeq": "1", "phase": "measured", **fields}


def publisher(sent, published, in_window, settle, failed=0):
    metrics = {"messages." + key: str(value) for key, value in
               dict(sent=sent, published=published, publishedInWindow=in_window, settlePublished=settle, failed=failed,
                    timeout=0, cancelled=0, unresolved=0).items()}
    return {"metrics": metrics, "window": {"measuredSeconds": 2.0}, "nullReasons": {}}


def subscriber(index, duplicates=0):
    reasons = {"/metrics/" + key: {"code": "CLOCK_DOMAIN_UNVERIFIED", "reason": "x"} for key in LATENCY_KEYS}
    reasons.update({"/histograms/" + key: {"code": "CLOCK_DOMAIN_UNVERIFIED", "reason": "x"}
                    for key in ("fanoutDeliveryLatencyMs", "fanoutSettleDeliveryLatencyMs")})
    return {"roleInstance": index, "metrics": {"fanout.duplicateEvents": str(duplicates)},
            "window": {"measuredSeconds": 2.0}, "nullReasons": reasons}


class FanoutAggregateTests(unittest.TestCase):
    def cell(self, files: dict) -> Path:
        root = Path(tempfile.mkdtemp())
        for name, value in files.items():
            (root / name).write_text(json.dumps(value))
        return root

    def run_case(self, publisher_original, publisher_file, subscribers):
        files = {"publisher-sequences.json": identity(**publisher_file)}
        originals = {"server-publisher-0.json": publisher_original}
        for index, (original, sequences) in enumerate(subscribers):
            files[f"subscriber-{index}-sequences.json"] = identity(subscriberId=index, **sequences)
            originals[f"server-subscriber-{index}.json"] = original
        metrics, histograms, reasons = {}, {}, {}
        owners = list(originals)
        delivery = fanout_aggregate(self.cell(files), CONFIG, originals, owners, metrics, histograms, reasons)
        return delivery, metrics, histograms, reasons

    def test_delivery_is_the_intersection_with_window_success_and_the_minimum_ratio_is_kept(self):
        # Publisher: 1..100 attempted, 50 failed, 101 succeeded only in settle (sent = 101).
        pub = publisher(101, 100, 99, 1, failed=1)
        pub_file = dict(attemptedRanges=ranges((1, 101)), windowSuccessRanges=ranges((1, 49), (51, 100)),
                        settleSuccessRanges=ranges((101, 101)))
        a = dict(windowRanges=ranges((1, 60)), settleRanges=ranges((61, 101)), duplicateEvents="3")
        b = dict(windowRanges=ranges((1, 39)), settleRanges=[], duplicateEvents="0")
        delivery, metrics, histograms, reasons = self.run_case(pub, pub_file, [(subscriber(0, 3), a), (subscriber(1), b)])
        self.assertEqual(delivery["server-subscriber-0.json"]["deliveredInWindow"], 59)   # 1..60 without 50
        self.assertEqual(delivery["server-subscriber-0.json"]["settleDelivered"], 40)     # 61..100; 101 is outside the cohort
        self.assertEqual(delivery["server-subscriber-0.json"]["outOfCohortEvents"], 2)    # 50 (failed publish) and 101
        self.assertEqual(delivery["server-subscriber-1.json"]["uniqueDelivered"], 39)
        self.assertEqual(metrics["fanout.deliveryRatio"], 39 / 99)                        # the worst subscriber, not the mean
        self.assertEqual(metrics["fanout.duplicateEvents"], "3")
        self.assertEqual(metrics["fanout.subscriberCount"], "2")
        self.assertEqual(metrics["fanout.deliveryOpsPerSec"], 59 / 2 + 39 / 2)
        self.assertEqual(metrics["fanout.publishOpsPerSec"], 99 / 2)
        self.assertIsNone(metrics["fanout.deliveryLatency.p99Ms"])
        self.assertEqual(reasons["/metrics/fanout.deliveryLatency.p99Ms"]["code"], "CLOCK_DOMAIN_UNVERIFIED")
        self.assertIsNone(histograms["fanoutDeliveryLatencyMs"])

    def test_publisher_counters_that_disagree_with_its_ranges_are_rejected(self):
        pub_file = dict(attemptedRanges=ranges((1, 10)), windowSuccessRanges=ranges((1, 10)), settleSuccessRanges=[])
        sub = dict(windowRanges=ranges((1, 10)), settleRanges=[], duplicateEvents="0")
        for bad in (publisher(10, 10, 9, 0), publisher(11, 10, 10, 0), publisher(10, 10, 10, 0, failed=1)):
            with self.subTest(bad=bad["metrics"]), self.assertRaisesRegex(ValueError, "SchemaMismatch"):
                self.run_case(bad, pub_file, [(subscriber(0), sub), (subscriber(1), sub)])
        # The same inputs with consistent counters pass, so the rejections above are not a fixture problem.
        self.run_case(publisher(10, 10, 10, 0), pub_file, [(subscriber(0), sub), (subscriber(1), sub)])

    def test_malformed_subscriber_originals_are_rejected(self):
        pub = publisher(10, 10, 10, 0)
        pub_file = dict(attemptedRanges=ranges((1, 10)), windowSuccessRanges=ranges((1, 10)), settleSuccessRanges=[])
        good = dict(windowRanges=ranges((1, 5)), settleRanges=ranges((6, 10)), duplicateEvents="0")
        cases = {
            "a sequence first received in both window and settle": dict(windowRanges=ranges((1, 6)), settleRanges=ranges((6, 10)), duplicateEvents="0"),
            "adjacent ranges that are not maximal": dict(windowRanges=ranges((1, 3), (4, 5)), settleRanges=[], duplicateEvents="0"),
            "descending ranges": dict(windowRanges=ranges((6, 8), (1, 3)), settleRanges=[], duplicateEvents="0"),
            "a duplicate count that disagrees with the original": dict(windowRanges=ranges((1, 5)), settleRanges=[], duplicateEvents="1"),
        }
        for name, bad in cases.items():
            with self.subTest(name), self.assertRaisesRegex(ValueError, "SchemaMismatch"):
                self.run_case(pub, pub_file, [(subscriber(0), bad), (subscriber(1), good)])
        self.run_case(pub, pub_file, [(subscriber(0), good), (subscriber(1), good)])
        with self.assertRaisesRegex(ValueError, "CollectionFailure"):
            self.run_case(pub, pub_file, [(subscriber(0), good)])  # one subscriber original for two configured

    def test_no_window_success_leaves_the_ratio_null_with_a_zero_denominator_reason(self):
        pub = publisher(3, 0, 0, 0, failed=3)
        pub_file = dict(attemptedRanges=ranges((1, 3)), windowSuccessRanges=[], settleSuccessRanges=[])
        sub = dict(windowRanges=ranges((1, 3)), settleRanges=[], duplicateEvents="0")
        _, metrics, _, reasons = self.run_case(pub, pub_file, [(subscriber(0), sub), (subscriber(1), sub)])
        self.assertIsNone(metrics["fanout.deliveryRatio"])
        self.assertEqual(reasons["/metrics/fanout.deliveryRatio"]["code"], "ZERO_DENOMINATOR")
        self.assertEqual(metrics["fanout.outOfCohortEvents"], "6")

    def test_range_arithmetic(self):
        a, b = sequence_ranges(ranges((1, 5), (9, 12)), "a"), sequence_ranges(ranges((4, 10)), "b")
        self.assertEqual(range_size(a), 9)
        self.assertEqual(range_intersection_size(a, b), 2 + 2)
        self.assertEqual(range_intersection_size(a, []), 0)


class SettleStatusTests(unittest.TestCase):
    OWNERS = ["server-publisher-0.json", "server-subscriber-0.json", "server-subscriber-1.json"]

    def stats(self, published, *received):
        result = {self.OWNERS[0]: {"metrics": {"messages.publishedInWindow": str(published)}}}
        for name, count in zip(self.OWNERS[1:], received):
            result[name] = {"runtimeMetrics": {"fanoutReceipts": {"value": {"uniqueInWindow": str(count - 1), "uniqueInSettle": "1"}}}}
        return result

    def test_fanout_settle_ends_only_when_every_subscriber_reached_the_window_success_count(self):
        reached = settle_status("fanout-sequences", self.stats(100, 100, 130), self.OWNERS, 1.0, 5.0)
        self.assertEqual((reached["done"], reached["boundReached"]), (True, False))
        waiting = settle_status("fanout-sequences", self.stats(100, 100, 99), self.OWNERS, 1.0, 5.0)  # one subscriber is behind
        self.assertEqual((waiting["done"], waiting["boundReached"]), (False, False))
        self.assertEqual(waiting["progress"]["received"]["server-subscriber-1.json"], 99)
        bound = settle_status("fanout-sequences", self.stats(100, 100, 99), self.OWNERS, 5.0, 5.0)
        self.assertEqual((bound["done"], bound["boundReached"]), (True, True))  # the bound is recorded, not hidden
        late = settle_status("fanout-sequences", self.stats(100, 100, 100), self.OWNERS, 9.0, 5.0)
        self.assertEqual((late["done"], late["boundReached"]), (True, False))  # reached wins over a late poll

    def test_echo_settle_is_the_source_phase_complete(self):
        status = settle_status("echo", {}, ["server-channel-0.json"], 0.1, 5.0)
        self.assertEqual((status["done"], status["boundReached"]), (True, False))


if __name__ == "__main__":
    unittest.main()
