#!/usr/bin/env python3
"""Focused coordinator contract tests: CLI consumers, identity and histogram aggregation."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from results import BOUNDS, MAX_U64, aggregate, export_latency, histogram_merge, u64, write_json
from runner import comparison, options


def histogram(samples, overflow=0):
    counts = ["0"] * len(BOUNDS)
    for bucket, count in samples.items():
        counts[bucket] = str(count)
    return {"unit": "ms", "ticksUnit": "ns", "bounds": BOUNDS.copy(), "counts": counts,
            "overflow": str(overflow), "count": str(sum(samples.values()) + overflow),
            "sumNs": str(sum(int(BOUNDS[b] * 1e6) * n for b, n in samples.items()) + overflow * 2_000_000_000),
            "maxNs": "2000000000" if overflow else str(int(BOUNDS[max(samples)] * 1e6)) if samples else None,
            "percentileMethod": "nearest-rank-bucket-upper-bound"}


class HarnessTests(unittest.TestCase):
    def test_cli_rejects_missing_consumer_and_nonfinite_values(self):
        bad = [
            ["single"],
            ["single", "--scenario", "session-echo-only", "--logical-streams", "1"],
            ["single", "--scenario", "session-echo-only", "--channel-topology", "routemesh"],
            ["single", "--scenario", "channel-echo-only", "--connections", "1"],
            ["single", "--scenario", "channel-echo-only", "--connect-concurrency", "1"],
            ["single", "--scenario", "channel-echo-only", "--client-count", "2"],
            ["single", "--scenario", "session-echo-only", "--spot-count", "1"],
            ["single", "--scenario", "session-echo-only", "--mode", "send-send"],
            ["single", "--scenario", "session-echo-only", "--codec", "protobuf"],
            ["single", "--scenario", "session-echo-only", "--duration-seconds", "nan"],
            ["single", "--scenario", "session-echo-only", "--inflight", "2147483648"],
            ["single", "--scenario", "session-echo-only", "--client-index", "0"],
            ["matrix", "--payload-size", "1024"],
            ["matrix", "--payload-sizes", "1024,1024"],
            ["matrix", "--run-id", "../escape"],
        ]
        for argv in bad:
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                options(argv)

    def test_cell_comparison_excludes_run_identity_but_keeps_workload(self):
        env = {key: None for key in ("cpuModel", "effectiveProcessorCount", "cpuQuota", "cpuset", "cpuAffinity", "memoryLimit", "runtimeOptions")}
        env["serializer"] = {"name": "typed JSON"}
        a = options(["single", "--scenario", "session-echo-only", "--run-id", "first", "--connections", "8"])
        b = options(["single", "--scenario", "session-echo-only", "--run-id", "second", "--connections", "8"])
        self.assertEqual(comparison(a, a.scenario, 1024, None, env)[1], comparison(b, b.scenario, 1024, None, env)[1])
        b.inflight = 2
        self.assertNotEqual(comparison(a, a.scenario, 1024, None, env)[1], comparison(b, b.scenario, 1024, None, env)[1])

    def test_merged_quantiles_are_weighted_by_integer_samples(self):
        merged = histogram_merge([histogram({0: 99}), histogram({13: 1})])
        metrics, reasons = {}, {}
        export_latency(merged, "latency", "latencyMs", metrics, reasons)
        self.assertEqual(metrics["latency.p99Ms"], .1)
        self.assertEqual(merged["count"], "100")
        self.assertEqual(merged["sumNs"], "1033900000")
        self.assertEqual(metrics["latency.maxMs"], 1024)

    def test_large_nearest_rank_never_loses_integer_precision(self):
        value = histogram({0: (MAX_U64 * 99 + 99) // 100}, overflow=MAX_U64 - (MAX_U64 * 99 + 99) // 100)
        metrics, reasons = {}, {}
        export_latency(histogram_merge([value]), "latency", "latencyMs", metrics, reasons)
        self.assertEqual(metrics["latency.p99Ms"], .1)

    def test_overflow_percentile_is_null_and_mean_uses_exact_sum(self):
        value = histogram_merge([histogram({0: 1}, overflow=1)])
        metrics, reasons = {}, {}
        export_latency(value, "latency", "latencyMs", metrics, reasons)
        self.assertEqual(metrics["latency.p50Ms"], .1)
        self.assertIsNone(metrics["latency.p95Ms"])
        self.assertEqual(reasons["/metrics/latency.p95Ms"]["lowerBoundMs"], 1024)
        self.assertEqual(metrics["latency.meanMs"], 1000.05)

    def test_mismatched_histogram_and_counter_overflow_are_failures(self):
        value = histogram({0: 1})
        value["bounds"][0] = .2
        with self.assertRaisesRegex(ValueError, "SchemaMismatch"):
            histogram_merge([value])
        with self.assertRaisesRegex(ValueError, "overflow"):
            histogram_merge([histogram({0: MAX_U64}), histogram({0: 1})])
        for value in (1, "01", "+1", "-1", str(MAX_U64 + 1)):
            with self.subTest(value=value), self.assertRaises(ValueError):
                u64(value)

    def test_existing_result_file_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.json"
            write_json(path, {"value": "first"})
            with self.assertRaises(FileExistsError):
                write_json(path, {"value": "second"})
            self.assertEqual(json.loads(path.read_text()), {"value": "first"})

    def test_failed_warmup_preserves_public_error_counts_without_measured_metrics(self):
        config = {"runId": "r", "cellId": "c", "configHash": "h", "scenario": "channel-echo-only",
                  "workload": {"payloadSize": 4096}, "topology": "routemesh"}
        source = {"schemaVersion": 2, **{key: config[key] for key in ("runId", "cellId", "configHash")},
                  "resetSeq": "0", "phase": "complete", "metrics": {
                      "messages.sent": "32", "messages.completed": "16", "messages.timeout": "16",
                      "errors.byKind": {"DeadlineExceeded": "16"}, "errors.harness": {}, "errors.language": {}},
                  "histograms": {}, "nullReasons": {}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_json(root / "server-channel-0.json", source)
            result = aggregate(root, config, [], ["server-channel-0.json"], [])
            self.assertEqual(result["status"], "failed")
            self.assertFalse(result["baselineEligible"])
            self.assertIsNone(result["metrics"]["messages.completed"])
            failure = next(issue for issue in result["reasons"] if issue["code"] == "PreMeasurementFailure")
            self.assertEqual(failure["resetSeq"], "0")
            self.assertEqual(failure["errorCounts"]["errors.byKind"], {"DeadlineExceeded": "16"})
            summary = json.loads((root / "summary.json").read_text())
            self.assertIn(failure, summary["reasons"])

    def test_cs_aggregation_sums_owner_rates_and_separates_receiver_messages(self):
        config = {"runId": "r", "cellId": "c", "configHash": "h", "scenario": "session-echo-only",
                  "workload": {"payloadSize": 1024}, "topology": None}
        def original(role, instance, successes, seconds, request_count, reply_count):
            metrics = {"messages." + key: str(successes if key in ("sent", "completed") else 0)
                       for key in ("sent", "completed", "settleCompleted", "failed", "timeout", "cancelled", "unresolved")}
            metrics.update({"connections.requested": "1", "connections.connected": "1", "connections.failed": "0",
                            "errors.byKind": {}, "errors.harness": {}, "errors.language": {},
                            "throughput.messagesPerSec": (request_count + reply_count) / seconds,
                            "throughput.megabytesPerSec": (request_count + reply_count) * 1024 / seconds / 1048576})
            for direction, count in (("request", request_count), ("reply", reply_count), ("send", 0), ("event", 0)):
                metrics["applicationMessages." + direction] = str(count)
                metrics["applicationPayloadBytes." + direction] = str(count * 1024)
            return {"schemaVersion": 2, **{key: config[key] for key in ("runId", "cellId", "configHash")},
                    "role": role, "roleInstance": instance, "resetSeq": "1", "phase": "complete",
                    "window": {"startTicks": "0", "endTicks": str(int(seconds * 1e9)), "measuredSeconds": seconds},
                    "metrics": metrics, "histograms": {"latencyMs": histogram({0: successes}) if successes else histogram({}),
                                                       "settleLatencyMs": histogram({})},
                    "nullReasons": {}, "provenance": {"pid": instance + 100}, "clock": {}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_json(root / "client-0.json", original("client", 0, 10, 1, 10, 0))
            write_json(root / "client-1.json", original("client", 1, 90, 9, 90, 0))
            write_json(root / "server-session-0.json", original("session", 2, 0, 10, 0, 100))
            result = aggregate(root, config, ["client-0.json", "client-1.json"], ["server-session-0.json"], [])
            self.assertEqual(result["status"], "valid")
            self.assertEqual(result["metrics"]["messages.completed"], "100")
            self.assertEqual(result["metrics"]["throughput.kops"], .02)
            self.assertEqual(result["metrics"]["throughput.messagesPerSec"], 30)
            self.assertEqual(result["metrics"]["applicationMessages.request"], "100")
            self.assertEqual(result["metrics"]["applicationMessages.reply"], "100")
            self.assertEqual(result["histograms"]["latencyMs"]["count"], "100")
            self.assertIsNone(result["measuredSeconds"])
            self.assertEqual(result["nullReasons"]["/measuredSeconds"]["code"], "MULTIPLE_OWNERS")


if __name__ == "__main__":
    unittest.main()
