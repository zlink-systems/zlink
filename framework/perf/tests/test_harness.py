#!/usr/bin/env python3
"""Focused coordinator contract tests: CLI consumers, identity and histogram aggregation."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runner"))

from launchers import launcher
from results import BOUNDS, MAX_U64, aggregate, export_latency, histogram_merge, u64, write_json
from roles import plan_roles
from runner import agreed_core_version, agreed_framework_version, comparison, options, role_executables
from scenarios import BY_NAME, ROLE_KINDS, SCENARIOS, expand

COMMON = ["--language", "dotnet", "--perf-dir", "/tmp/perf"]


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
            ["single", "--scenario", "actor-no-bind-request-echo", "--connect-concurrency", "0"],
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
                options(argv + COMMON)

    def test_options_are_judged_by_the_scenario_table_in_both_directions(self):
        # (scenario, option arguments): every consumed option is accepted, every option a scenario does not consume is rejected.
        accepted = [
            ("session-echo-only", ["--connections", "8", "--connect-concurrency", "2", "--client-count", "2", "--mode", "request"]),
            ("cs-local-session-actor-echo", ["--connections", "8", "--terminal", "ordinary"]),
            ("channel-echo-only", ["--logical-streams", "8", "--channel-topology", "clientserver", "--connect-concurrency", "2"]),
            ("s2s-channel-to-spot-request-echo", ["--spot-count", "4", "--logical-streams", "8"]),
            ("s2s-channel-to-spot-send-send-echo", ["--mode", "send-send", "--channel-topology", "routemesh"]),
            ("s2s-spot-to-channel-request-echo", ["--terminal", "yield", "--spot-count", "1"]),
            ("spot-worker-offload-echo", ["--worker-task-millis", "3", "--worker-pool-size", "2", "--terminal", "ordinary", "--mode", "worker-offload"]),
            ("pubsub-fanout-echo", ["--subscriber-count", "3", "--mode", "publish", "--inflight", "4"]),
            ("actor-no-bind-request-echo", ["--logical-streams", "8", "--connect-concurrency", "4"]),
        ]
        for scenario, extra in accepted:
            with self.subTest(scenario=scenario, extra=extra):
                options(["single", "--scenario", scenario, *extra, *COMMON])
        rejected = [
            ("session-echo-only", ["--worker-task-millis", "5"]),
            ("cs-remote-session-actor-echo", ["--spot-count", "1"]),
            ("cs-remote-session-actor-echo", ["--terminal", "yield"]),
            ("s2s-channel-to-spot-request-echo", ["--connections", "1"]),
            ("s2s-channel-to-spot-request-echo", ["--subscriber-count", "2"]),
            ("s2s-channel-to-spot-request-echo", ["--channel-topology", "routemesh"]),  # no Channel role: topology is na
            ("s2s-channel-to-spot-send-send-echo", ["--channel-topology", "clientserver"]),
            ("s2s-spot-to-channel-request-echo", ["--mode", "send-send"]),
            ("spot-no-await-echo", ["--worker-pool-size", "2"]),
            ("spot-no-await-echo", ["--terminal", "yield"]),
            ("spot-worker-offload-echo", ["--mode", "request"]),
            ("actor-no-bind-request-echo", ["--spot-count", "1"]),
            ("actor-no-bind-request-echo", ["--client-count", "2"]),
            ("pubsub-fanout-echo", ["--spot-count", "1"]),
            ("pubsub-fanout-echo", ["--mode", "request"]),
            ("channel-echo-only", ["--subscriber-count", "1"]),
            ("session-echo-only", ["--workload-config", "/tmp/w.json"]),
            ("session-echo-only", ["--endpoint-config", "/tmp/e.json"]),
        ]
        for scenario, extra in rejected:
            with self.subTest(scenario=scenario, extra=extra), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                options(["single", "--scenario", scenario, *extra, *COMMON])
        # spot-local-echo is a comparison-table name (§11.3), not a scenario; matrix axes are not overridden by options.
        for argv in (["single", "--scenario", "spot-local-echo"], ["matrix", "--terminal", "yield"],
                     ["matrix", "--scenario", "s2s-spot-to-channel-request-echo", "--spot-count", "1"],
                     ["matrix", "--channel-topology", "routemesh"], ["matrix", "--client-count", "2"]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                options(argv + COMMON)

    def test_matrix_cells_match_the_specification_counts(self):
        args = options(["matrix", *COMMON])
        cells = expand(args, True)
        per_payload = {payload: [c for c in cells if c.payload == payload] for payload in (1024, 4096)}
        # §5: 9 single-cell scenarios, §10.5 ordinary/yield x SpotId 1/16, §10.8 ordinary/yield, §11 two topologies + session.
        self.assertEqual({p: len(c) for p, c in per_payload.items()}, {1024: 18, 4096: 18})
        for cells_of_payload in per_payload.values():
            count = lambda name: sum(c.scenario.name == name for c in cells_of_payload)
            self.assertEqual(count("s2s-spot-to-channel-request-echo"), 4)
            self.assertEqual(count("spot-worker-offload-echo"), 2)
            self.assertEqual(count("channel-echo-only"), 2)
            self.assertEqual({(c.terminal, c.spot_count) for c in cells_of_payload if c.scenario.name == "s2s-spot-to-channel-request-echo"},
                             {("ordinary", 1), ("ordinary", 16), ("yield", 1), ("yield", 16)})
            self.assertEqual({c.terminal for c in cells_of_payload if c.scenario.name == "spot-worker-offload-echo"}, {"ordinary", "yield"})
        self.assertEqual({c.scenario.name for c in cells}, {s.name for s in SCENARIOS})
        self.assertEqual(len(SCENARIOS), 13)  # §10's 11 scenarios and §11's two baselines; spot-local-echo is only a reference
        self.assertEqual(BY_NAME["spot-no-await-echo"].references, ("spot-local-echo",))

    def test_variant_and_config_hash_follow_the_identity_format(self):
        env = {key: None for key in ("cpuModel", "effectiveProcessorCount", "cpuQuota", "cpuset", "cpuAffinity", "memoryLimit", "runtimeOptions")}
        env["serializer"] = {"name": "typed JSON"}
        for argv in (["--scenario", "s2s-spot-to-channel-request-echo", "--terminal", "yield", "--spot-count", "1"],
                     ["--scenario", "pubsub-fanout-echo", "--subscriber-count", "3"],
                     ["--scenario", "channel-echo-only", "--channel-topology", "clientserver"],
                     ["--scenario", "session-echo-only"]):
            args = options(["single", *argv, *COMMON])
            cell = expand(args, False)[0]
            config_hash = hashlib.sha256(comparison(args, cell, env)[1].encode()).hexdigest()
            self.assertRegex(cell.variant(config_hash), r"^(request|send-send|no-await|worker-offload|publish)-(ordinary|yield)-(routemesh|clientserver|na)-s(\d+|na)-n(\d+|na)-[0-9a-f]{64}$")
            self.assertEqual(cell.cell_id(config_hash), f"{cell.scenario.name}/{cell.payload}/{cell.variant(config_hash)}")
        # Not applicable parts are `na`; the hash changes with a comparison input such as the subscriber count.
        a = options(["single", "--scenario", "pubsub-fanout-echo", "--subscriber-count", "3", *COMMON])
        b = options(["single", "--scenario", "pubsub-fanout-echo", "--subscriber-count", "4", *COMMON])
        self.assertNotEqual(comparison(a, expand(a, False)[0], env)[1], comparison(b, expand(b, False)[0], env)[1])
        self.assertIn("-na-sna-n3-", expand(a, False)[0].variant("x"))

    def test_only_send_send_object_client_sources_skip_the_remote_target_wait(self):
        common = {"runId": "r", "cellId": "c", "configHash": "a" * 64, "workload": {}, "worker": None, "store": None,
                  "diagnostics": lambda name: None, "provenance": {}}
        skipping = set()
        for scenario in SCENARIOS:
            args = options(["single", "--scenario", scenario.name, *(["--subscriber-count", "3"] if scenario.uses(count="subscribers") else []), *COMMON])
            cell = expand(args, False)[0]
            port = iter(range(20000, 30000))
            for role in plan_roles(cell, {"spot_count": cell.spot_count, "connections": 4, "logical_streams": 4},
                                  common, launcher("dotnet").stream_scheme, lambda: next(port)):
                if not role.config["awaitRemoteTargets"]:
                    skipping.add((scenario.name, role.config["role"]))
        # Bad direction: the receivers, the request sources and the server-side send/send source (10.6) still wait.
        self.assertEqual(skipping, {("s2s-channel-to-spot-send-send-echo", "channel"), ("actor-no-bind-send-send-echo", "actor-caller")})

    def test_every_role_kind_gets_config_and_manifest_from_the_tables(self):
        self.assertEqual(len(ROLE_KINDS), 8)
        seen = set()
        for scenario in SCENARIOS:
            args = options(["single", "--scenario", scenario.name, *(["--subscriber-count", "3"] if scenario.uses(count="subscribers") else []), *COMMON])
            cell = expand(args, False)[0]
            port = iter(range(20000, 30000))
            common = {"runId": "r", "cellId": "c", "configHash": "a" * 64, "workload": {}, "worker": None, "store": None,
                      "diagnostics": lambda name: None, "provenance": {}}
            planned = plan_roles(cell, {"spot_count": cell.spot_count, "connections": 4, "logical_streams": 4},
                                 common, launcher("dotnet").stream_scheme, lambda: next(port))
            expected = sum(3 if role.count else 1 for role in scenario.roles)
            self.assertEqual(len(planned), expected)
            self.assertEqual(sum(role.source for role in planned), 0 if scenario.driver == "clients" else 1)
            for role in planned:
                seen.add(role.config["role"])
                self.assertEqual(role.manifest["metrics"]["baseUrl"], role.config["metricsUrl"])
                self.assertEqual(role.manifest["streamEndpoint"], role.config["transportEndpoints"].get("stream"))
                self.assertEqual(role.config["spotIds"] == [], not any(r.objects == "spot" and r.kind == role.config["role"] for r in scenario.roles))
                self.assertEqual(len(role.ports), len(set(role.ports)))
        self.assertEqual(seen, set(ROLE_KINDS))
        self.assertEqual(set(role_executables(options(["matrix", *COMMON]))), {"Client", *ROLE_KINDS.values()})

    def test_language_and_perf_dir_are_required_and_unknown_language_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            options(["single", "--scenario", "session-echo-only"])
        with self.assertRaises(ValueError):
            launcher("cobol")
        self.assertEqual(launcher("dotnet").command(Path("/p"), "Client")[0], "dotnet")

    def test_launcher_owns_each_languages_stream_scheme_and_loaded_artifact_markers(self):
        expected_schemes = {"dotnet": "tcp", "java": "tcp", "kotlin": "tcp", "node": "ws", "cpp": "tcp"}
        for language, scheme in expected_schemes.items():
            with self.subTest(language=language):
                selected = launcher(language)
                self.assertEqual(selected.stream_scheme, scheme)
                self.assertTrue(selected.loaded_artifact_markers)
        self.assertIn(".node", launcher("node").loaded_artifact_markers)
        self.assertIn("libzlink.so", launcher("java").loaded_artifact_markers)
        self.assertIn(".jar", launcher("java").loaded_artifact_markers)
        self.assertIn("libzlink.so", launcher("kotlin").loaded_artifact_markers)
        self.assertIn(".jar", launcher("kotlin").loaded_artifact_markers)
        self.assertIn("libzlink_framework.so", launcher("cpp").loaded_artifact_markers)

    def test_planned_stream_endpoint_uses_the_launcher_scheme(self):
        args = options(["single", "--scenario", "session-echo-only", *COMMON])
        cell = expand(args, False)[0]
        common = {"runId": "r", "cellId": "c", "configHash": "a" * 64, "workload": {}, "worker": None,
                  "store": None, "diagnostics": lambda name: None, "provenance": {}}
        expected = {"dotnet": "tcp", "java": "tcp", "kotlin": "tcp", "node": "ws", "cpp": "tcp"}
        for language, scheme in expected.items():
            port = iter(range(20000, 30000))
            planned = plan_roles(cell, {"spot_count": cell.spot_count, "connections": 4, "logical_streams": 4},
                                 common, launcher(language).stream_scheme, lambda: next(port))
            stream_roles = [role for role in planned if role.config["transportEndpoints"].get("stream")]
            self.assertTrue(stream_roles)
            for role in stream_roles:
                endpoint = role.config["transportEndpoints"]["stream"]
                self.assertTrue(endpoint.startswith(scheme + "://"))
                self.assertEqual(role.manifest["streamEndpoint"], endpoint)

    def test_declared_framework_version_walks_to_shared_language_version(self):
        from launchers import _declared_framework_version

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "java").mkdir()
            (root / "java" / "VERSION").write_text("ZLINK_FRAMEWORK_VERSION=1.2.3\n")
            kotlin_perf = root / "java" / "perf" / "kotlin"
            kotlin_perf.mkdir(parents=True)
            self.assertEqual(_declared_framework_version(kotlin_perf), "1.2.3")

    def test_cell_comparison_excludes_run_identity_but_keeps_workload(self):
        env = {key: None for key in ("cpuModel", "effectiveProcessorCount", "cpuQuota", "cpuset", "cpuAffinity", "memoryLimit", "runtimeOptions")}
        env["serializer"] = {"name": "typed JSON"}
        a = options(["single", "--scenario", "session-echo-only", "--run-id", "first", "--connections", "8", *COMMON])
        b = options(["single", "--scenario", "session-echo-only", "--run-id", "second", "--connections", "8", *COMMON])
        cell_a, cell_b = expand(a, False)[0], expand(b, False)[0]
        self.assertEqual(comparison(a, cell_a, env)[1], comparison(b, cell_b, env)[1])
        b.inflight = 2
        self.assertNotEqual(comparison(a, cell_a, env)[1], comparison(b, cell_b, env)[1])

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
        config = {"language": "dotnet", "runId": "r", "cellId": "c", "configHash": "h", "scenario": "channel-echo-only",
                  "workload": {"payloadSize": 4096}, "topology": "routemesh"}
        source = {"schemaVersion": 2, **{key: config[key] for key in ("runId", "cellId", "configHash")},
                  "resetSeq": "0", "phase": "complete", "metrics": {
                      "messages.sent": "32", "messages.completed": "16", "messages.timeout": "16",
                      "errors.byKind": {"DeadlineExceeded": "16"}, "errors.harness": {}, "errors.language": {}},
                  "histograms": {}, "nullReasons": {}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_json(root / "server-channel-0.json", source)
            result = aggregate(root, config, [], ["server-channel-0.json"], [], ["server-channel-0.json"])
            self.assertEqual(result["status"], "failed")
            self.assertFalse(result["baselineEligible"])
            self.assertIsNone(result["metrics"]["messages.completed"])
            failure = next(issue for issue in result["reasons"] if issue["code"] == "PreMeasurementFailure")
            self.assertEqual(failure["resetSeq"], "0")
            self.assertEqual(failure["errorCounts"]["errors.byKind"], {"DeadlineExceeded": "16"})
            summary = json.loads((root / "summary.json").read_text())
            self.assertIn(failure, summary["reasons"])

    def test_cs_aggregation_sums_owner_rates_and_separates_receiver_messages(self):
        config = {"language": "dotnet", "runId": "r", "cellId": "c", "configHash": "h", "scenario": "session-echo-only",
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
        for has_lower_bound in (True, False):
            with self.subTest(has_lower_bound=has_lower_bound), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                client_zero = original("client", 0, 10, 1, 10, 0)
                client_zero["metrics"]["sourceProbe"] = None
                source_reason = {"code": "PUBLIC_OBSERVATION_UNSUPPORTED",
                                 "reason": "Source metric observation is unsupported.",
                                 "owner": "test/source"}
                if has_lower_bound:
                    source_reason["lowerBoundMs"] = None
                client_zero["nullReasons"] = {"/metrics/sourceProbe": source_reason}
                write_json(root / "client-0.json", client_zero)
                write_json(root / "client-1.json", original("client", 1, 90, 9, 90, 0))
                write_json(root / "server-session-0.json", original("session", 2, 0, 10, 0, 100))
                result = aggregate(root, config, ["client-0.json", "client-1.json"], ["server-session-0.json"], [], ["client-0.json", "client-1.json"])
                self.assertEqual(result["status"], "valid" if has_lower_bound else "invalid")
                if has_lower_bound:
                    self.assertEqual(result["metrics"]["messages.completed"], "100")
                    self.assertEqual(result["metrics"]["throughput.kops"], .02)
                    self.assertEqual(result["metrics"]["throughput.messagesPerSec"], 30)
                    self.assertEqual(result["metrics"]["applicationMessages.request"], "100")
                    self.assertEqual(result["metrics"]["applicationMessages.reply"], "100")
                    self.assertEqual(result["histograms"]["latencyMs"]["count"], "100")
                    self.assertIsNone(result["measuredSeconds"])
                    self.assertEqual(result["nullReasons"]["/measuredSeconds"]["code"], "MULTIPLE_OWNERS")
                    self.assertEqual(result["nullReasons"]["/metrics/sourceProbe"]["owner"], "test/source")
                    self.assertIsNone(result["nullReasons"]["/metrics/sourceProbe"]["lowerBoundMs"])
                else:
                    mismatch = next(issue for issue in result["reasons"] if issue["code"] == "SchemaMismatch")
                    self.assertEqual(mismatch["sourceFile"], "client-0.json")
                    self.assertIn("lowerBoundMs", mismatch["message"])
                for output in (root / "result.json", root / "summary.json"):
                    saved = json.loads(output.read_text())
                    self.assertTrue(all("lowerBoundMs" in reason for reason in saved["nullReasons"].values()))



class CoreVersionAgreementTest(unittest.TestCase):
    def test_all_roles_report_the_same_version(self):
        self.assertEqual(agreed_core_version({"a.json": "1.10.0", "b.json": "1.10.0"}, None), "1.10.0")

    def test_a_role_with_a_different_version_fails(self):
        with self.assertRaises(RuntimeError):
            agreed_core_version({"a.json": "1.10.0", "b.json": "1.9.0"}, None)

    def test_an_unreported_version_fails(self):
        with self.assertRaises(RuntimeError):
            agreed_core_version({"a.json": "1.10.0", "b.json": None}, None)

    def test_a_later_cell_must_match_the_version_already_recorded(self):
        with self.assertRaises(RuntimeError):
            agreed_core_version({"a.json": "1.9.0"}, "1.10.0")


class FrameworkVersionAgreementTest(unittest.TestCase):
    def test_observed_framework_version_matches_the_declaration(self):
        self.assertEqual(agreed_framework_version("0.26.0", "0.26.0"), "0.26.0")

    def test_a_different_or_missing_framework_version_fails(self):
        for observed, declared in (("0.25.0", "0.26.0"), (None, "0.26.0"), ("0.26.0", None)):
            with self.subTest(observed=observed, declared=declared), self.assertRaises(RuntimeError):
                agreed_framework_version(observed, declared)


if __name__ == "__main__":
    unittest.main()
