"""§15.4 PS aggregation: subscriber first receipts intersect the publisher's window-success set."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runner"))
from results import fanout_aggregate, range_intersection_size, range_size, sequence_ranges  # noqa: E402

CONFIG = {"runId": "r", "cellId": "c", "subscriberCount": 2}
LATENCY_SUFFIXES = ("meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs")


def ranges(*pairs):
    return [{"first": str(first), "last": str(last)} for first, last in pairs]


def identity(**fields):
    return {"runId": "r", "cellId": "c", "resetSeq": "1", "phase": "measured", **fields}


def publisher(sent, in_window, failed=0, timeout=0, cancelled=0, inflight=0):
    metrics = {"messages." + key: str(value) for key, value in dict(
        sent=sent, publishedInWindow=in_window, failed=failed, timeout=timeout, cancelled=cancelled,
        inflightAtEnd=inflight).items()}
    return {"role": "publisher", "roleInstance": 0, "metrics": metrics,
            "window": {"measuredSeconds": 2.0}, "nullReasons": {}}


def subscriber(index, duplicates=0):
    reasons = {"/metrics/fanout.deliveryLatency." + suffix:
               {"code": "CLOCK_DOMAIN_UNVERIFIED", "reason": "x"} for suffix in LATENCY_SUFFIXES}
    reasons["/histograms/fanoutDeliveryLatencyMs"] = {"code": "CLOCK_DOMAIN_UNVERIFIED", "reason": "x"}
    return {"role": "subscriber", "roleInstance": index,
            "metrics": {"fanout.duplicateEvents": str(duplicates)},
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

    def test_delivery_uses_only_window_success_intersections(self):
        pub = publisher(101, 99, failed=1, inflight=1)
        pub_file = dict(attemptedRanges=ranges((1, 101)), windowSuccessRanges=ranges((1, 49), (51, 100)))
        a = dict(windowRanges=ranges((1, 60), (101, 101)), duplicateEvents="3")
        b = dict(windowRanges=ranges((1, 39)), duplicateEvents="0")
        delivery, metrics, histograms, reasons = self.run_case(
            pub, pub_file, [(subscriber(0, 3), a), (subscriber(1), b)])
        first = delivery["server-subscriber-0.json"]
        second = delivery["server-subscriber-1.json"]
        self.assertEqual(first["deliveredInWindow"], 59)  # 1..60 without failed publish 50
        self.assertEqual(first["outOfCohortEvents"], 2)  # failed publish 50 and sequence 101
        self.assertEqual(second["deliveredInWindow"], 39)
        self.assertEqual(metrics["fanout.deliveryRatio"], 39 / 99)
        self.assertEqual(metrics["fanout.duplicateEvents"], "3")
        self.assertEqual(metrics["fanout.subscriberCount"], "2")
        self.assertEqual(metrics["fanout.deliveredInWindow"], "98")
        self.assertEqual(metrics["fanout.outOfCohortEvents"], "2")
        self.assertEqual(metrics["fanout.deliveryOpsPerSec"], 59 / 2 + 39 / 2)
        self.assertEqual(metrics["fanout.publishOpsPerSec"], 99 / 2)
        self.assertIsNone(metrics["fanout.deliveryLatency.p99Ms"])
        self.assertEqual(reasons["/metrics/fanout.deliveryLatency.p99Ms"]["code"], "CLOCK_DOMAIN_UNVERIFIED")
        self.assertIsNone(histograms["fanoutDeliveryLatencyMs"])
        self.assertNotIn("uniqueDelivered", first)
        self.assertFalse(any("settle" in key for key in [*metrics, *histograms, *reasons]))

    def test_publisher_success_ranges_must_be_a_subset_of_attempted_ranges(self):
        pub = publisher(10, 10)
        invalid = dict(attemptedRanges=ranges((1, 9)), windowSuccessRanges=ranges((1, 10)))
        sub = dict(windowRanges=ranges((1, 10)), duplicateEvents="0")
        with self.assertRaisesRegex(ValueError, "SchemaMismatch"):
            self.run_case(pub, invalid, [(subscriber(0), sub), (subscriber(1), sub)])
        valid = dict(attemptedRanges=ranges((1, 10)), windowSuccessRanges=ranges((1, 10)))
        self.run_case(pub, valid, [(subscriber(0), sub), (subscriber(1), sub)])

    def test_publisher_counters_reconcile_with_ranges_and_inflight_at_end(self):
        pub_file = dict(attemptedRanges=ranges((1, 10)), windowSuccessRanges=ranges((1, 7)))
        sub = dict(windowRanges=ranges((1, 7)), duplicateEvents="0")
        for bad in (publisher(10, 6, failed=2, inflight=2), publisher(11, 7, failed=1, inflight=3),
                    publisher(10, 7, failed=1)):
            with self.subTest(metrics=bad["metrics"]), self.assertRaisesRegex(ValueError, "SchemaMismatch"):
                self.run_case(bad, pub_file, [(subscriber(0), sub), (subscriber(1), sub)])
        self.run_case(publisher(10, 7, failed=1, inflight=2), pub_file,
                      [(subscriber(0), sub), (subscriber(1), sub)])

    def test_subscriber_identity_and_sequence_shape_are_checked(self):
        pub = publisher(10, 10)
        pub_file = dict(attemptedRanges=ranges((1, 10)), windowSuccessRanges=ranges((1, 10)))
        good = dict(windowRanges=ranges((1, 5)), duplicateEvents="0")
        bad_cases = {
            "subscriber id does not match original": (subscriber(0), good, 1),
            "role is not subscriber": ({**subscriber(0), "role": "publisher"}, good, 0),
            "adjacent ranges are not maximal": (subscriber(0), dict(windowRanges=ranges((1, 3), (4, 5)), duplicateEvents="0"), 0),
            "descending ranges": (subscriber(0), dict(windowRanges=ranges((6, 8), (1, 2)), duplicateEvents="0"), 0),
            "duplicate count differs from original": (subscriber(0), dict(windowRanges=ranges((1, 5)), duplicateEvents="1"), 0),
        }
        for name, (original, sequences, sequence_id) in bad_cases.items():
            with self.subTest(name):
                files = {
                    "publisher-sequences.json": identity(**pub_file),
                    "subscriber-0-sequences.json": identity(subscriberId=sequence_id, **sequences),
                    "subscriber-1-sequences.json": identity(subscriberId=1, **good),
                }
                originals = {"server-publisher-0.json": pub,
                             "server-subscriber-0.json": original, "server-subscriber-1.json": subscriber(1)}
                with self.assertRaisesRegex(ValueError, "SchemaMismatch"):
                    fanout_aggregate(self.cell(files), CONFIG, originals, list(originals), {}, {}, {})
        self.run_case(pub, pub_file, [(subscriber(0), good), (subscriber(1), good)])

    def test_missing_subscriber_original_is_a_collection_failure(self):
        pub = publisher(10, 10)
        pub_file = dict(attemptedRanges=ranges((1, 10)), windowSuccessRanges=ranges((1, 10)))
        sub = dict(windowRanges=ranges((1, 10)), duplicateEvents="0")
        with self.assertRaisesRegex(ValueError, "CollectionFailure"):
            self.run_case(pub, pub_file, [(subscriber(0), sub)])

    def test_zero_window_success_keeps_ratio_null(self):
        pub = publisher(3, 0, failed=2, inflight=1)
        pub_file = dict(attemptedRanges=ranges((1, 3)), windowSuccessRanges=[])
        sub = dict(windowRanges=ranges((1, 3)), duplicateEvents="0")
        _, metrics, _, reasons = self.run_case(pub, pub_file, [(subscriber(0), sub), (subscriber(1), sub)])
        self.assertIsNone(metrics["fanout.deliveryRatio"])
        self.assertEqual(reasons["/metrics/fanout.deliveryRatio"]["code"], "ZERO_DENOMINATOR")
        self.assertEqual(metrics["fanout.outOfCohortEvents"], "6")

    def test_range_arithmetic(self):
        a, b = sequence_ranges(ranges((1, 5), (9, 12)), "a"), sequence_ranges(ranges((4, 10)), "b")
        self.assertEqual(range_size(a), 9)
        self.assertEqual(range_intersection_size(a, b), 2 + 2)
        self.assertEqual(range_intersection_size(a, []), 0)


if __name__ == "__main__":
    unittest.main()
