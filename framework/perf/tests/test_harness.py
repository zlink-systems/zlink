#!/usr/bin/env python3
"""Focused coordinator contract tests: CLI consumers, identity and histogram aggregation."""
import contextlib
import errno
import hashlib
import io
import json
from pathlib import Path
import tempfile
import time
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runner"))

from launchers import declared_framework_version, launcher
from results import BOUNDS, MAX_U64, _validate_null_reasons, aggregate, export_latency, histogram_merge, u64, write_json
from roles import plan_roles
from runner import agreed_core_version, agreed_framework_version, build, comparison, options, role_executables, run_exit_code
from scenarios import BY_NAME, ROLE_KINDS, SCENARIOS, expand
from store import RunStore

COMMON = ["--language", "dotnet", "--perf-dir", "/tmp/perf"]


OVERFLOW_NS = int((BOUNDS[-1] + 1000) * 1e6)


def histogram(samples, overflow=0):
    counts = ["0"] * len(BOUNDS)
    for bucket, count in samples.items():
        counts[bucket] = str(count)
    return {"unit": "ms", "ticksUnit": "ns", "bounds": BOUNDS.copy(), "counts": counts,
            "overflow": str(overflow), "count": str(sum(samples.values()) + overflow),
            "sumNs": str(sum(int(BOUNDS[b] * 1e6) * n for b, n in samples.items()) + overflow * OVERFLOW_NS),
            "maxNs": str(OVERFLOW_NS) if overflow else str(int(BOUNDS[max(samples)] * 1e6)) if samples else None,
            "percentileMethod": "nearest-rank-bucket-upper-bound-capped-by-max"}


class HarnessTests(unittest.TestCase):
    def test_store_waits_for_published_endpoint_and_internal_ping(self):
        def docker_result(*command, **_kwargs):
            if command[0] == "run":
                return "a" * 64
            if command[0] == "port":
                return "127.0.0.1:54321"
            if command[0] == "image":
                return json.dumps({"Id": "image-id", "RepoDigests": ["image-digest"]})
            return "PONG" if command[-1] == "ping" else "Redis version"

        with tempfile.TemporaryDirectory() as output, \
                patch("store.docker", side_effect=docker_result), \
                patch("store.time.sleep") as wait, \
                patch("socket.socket") as socket_factory:
            connection = socket_factory.return_value.__enter__.return_value
            connection.connect_ex.side_effect = [errno.ECONNREFUSED, 0]
            info = RunStore("store-readiness-contract", Path(output)).acquire()
            self.assertEqual(info["endpoint"], "127.0.0.1:54321")
            self.assertEqual(connection.connect_ex.call_count, 2)
            connection.settimeout.assert_called_with(5)
            wait.assert_called_once_with(0.5)

        with tempfile.TemporaryDirectory() as output, \
                patch("store.docker", side_effect=docker_result), \
                patch("store.time.sleep") as wait, \
                patch("socket.socket") as socket_factory:
            connection = socket_factory.return_value.__enter__.return_value
            connection.connect_ex.return_value = 0
            info = RunStore("store-ready-contract", Path(output)).acquire()
            self.assertEqual(info["endpoint"], "127.0.0.1:54321")
            wait.assert_not_called()

        with tempfile.TemporaryDirectory() as output, \
                patch("store.docker", side_effect=docker_result), \
                patch("store.time.monotonic", side_effect=[0, 60]), \
                patch("store.time.sleep") as wait, \
                patch("socket.socket") as socket_factory:
            connection = socket_factory.return_value.__enter__.return_value
            connection.connect_ex.return_value = errno.ECONNREFUSED
            with self.assertRaisesRegex(RuntimeError, "did not become ready within 60s"):
                RunStore("store-unreachable-contract", Path(output)).acquire()
            wait.assert_not_called()

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
            ["single", "--scenario", "session-echo-only", "--inflight", "1"],
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
            ("pubsub-fanout-echo", ["--subscriber-count", "3", "--mode", "publish"]),
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
        common = {"runId": "r", "cellId": "c", "configHash": "a" * 64, "language": "dotnet",
                  "workload": {}, "worker": None, "store": None,
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
            common = {"runId": "r", "cellId": "c", "configHash": "a" * 64, "language": "dotnet",
                      "workload": {}, "worker": None, "store": None,
                      "diagnostics": lambda name: None, "provenance": {}}
            planned = plan_roles(cell, {"spot_count": cell.spot_count, "connections": 4, "logical_streams": 4},
                                 common, launcher("dotnet").stream_scheme, lambda: next(port))
            expected = sum(3 if role.count else 1 for role in scenario.roles)
            self.assertEqual(len(planned), expected)
            self.assertEqual(sum(role.source for role in planned), 0 if scenario.driver == "clients" else 1)
            for role in planned:
                seen.add(role.config["role"])
                self.assertEqual(role.config["language"], "dotnet")
                self.assertEqual(role.manifest["metrics"]["baseUrl"], role.config["metricsUrl"])
                self.assertEqual(role.manifest["streamEndpoint"], role.config["transportEndpoints"].get("stream"))
                self.assertEqual(role.config["spotIds"] == [], not any(r.objects == "spot" and r.kind == role.config["role"] for r in scenario.roles))
                self.assertEqual(len(role.ports), len(set(role.ports)))
            if scenario.name == "s2s-spot-to-channel-send-send-echo":
                role_by_kind = {role.config["role"]: role for role in planned}
                self.assertEqual(role_by_kind["channel"].config["spotIds"], role_by_kind["spot"].config["spotIds"])
                self.assertEqual(len(role_by_kind["channel"].config["spotIds"]), cell.spot_count)
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
        common = {"runId": "r", "cellId": "c", "configHash": "a" * 64, "language": "dotnet",
                  "workload": {}, "worker": None, "store": None, "diagnostics": lambda name: None, "provenance": {}}
        expected = {"dotnet": "tcp", "java": "tcp", "kotlin": "tcp", "node": "ws", "cpp": "tcp"}
        for language, scheme in expected.items():
            port = iter(range(20000, 30000))
            common["language"] = language
            planned = plan_roles(cell, {"spot_count": cell.spot_count, "connections": 4, "logical_streams": 4},
                                 common, launcher(language).stream_scheme, lambda: next(port))
            stream_roles = [role for role in planned if role.config["transportEndpoints"].get("stream")]
            self.assertTrue(stream_roles)
            for role in stream_roles:
                self.assertEqual(role.config["language"], language)
                endpoint = role.config["transportEndpoints"]["stream"]
                self.assertTrue(endpoint.startswith(scheme + "://"))
                self.assertEqual(role.manifest["streamEndpoint"], endpoint)

    def test_cell_comparison_excludes_run_identity_but_keeps_workload(self):
        env = {key: None for key in ("cpuModel", "effectiveProcessorCount", "cpuQuota", "cpuset", "cpuAffinity", "memoryLimit", "runtimeOptions")}
        env["serializer"] = {"name": "typed JSON"}
        a = options(["single", "--scenario", "session-echo-only", "--run-id", "first", "--connections", "8", *COMMON])
        b = options(["single", "--scenario", "session-echo-only", "--run-id", "second", "--connections", "8", *COMMON])
        cell_a, cell_b = expand(a, False)[0], expand(b, False)[0]
        self.assertEqual(comparison(a, cell_a, env)[1], comparison(b, cell_b, env)[1])
        self.assertEqual(comparison(a, cell_a, env)[0]["packageSource"], "published")
        b.connections = 9
        self.assertNotEqual(comparison(a, cell_a, env)[1], comparison(b, cell_b, env)[1])

    def test_load_config_has_no_window_and_publisher_records_no_drop(self):
        env = {key: None for key in ("cpuModel", "effectiveProcessorCount", "cpuQuota", "cpuset", "cpuAffinity", "memoryLimit", "runtimeOptions")}
        env["serializer"] = {"name": "typed JSON"}
        args = options(["single", "--scenario", "pubsub-fanout-echo", *COMMON])
        cell = expand(args, False)[0]
        comparable = comparison(args, cell, env)[0]
        workload = comparable["workload"]
        self.assertNotIn("inflight", workload)
        self.assertEqual(comparable["publisherChannel"], {"noDrop": True})
        common = {"runId": "r", "cellId": "c", "configHash": "a" * 64, "language": "cpp",
                  "workload": workload, "worker": None, "store": None, "diagnostics": lambda name: None, "provenance": {}}
        ports = iter(range(20000, 30000))
        roles = plan_roles(cell, {"subscriber_count": cell.subscriber_count, "logical_streams": 4},
                           common, "tcp", lambda: next(ports))
        publisher = next(role.config for role in roles if role.config["role"] == "publisher")
        self.assertIs(publisher["provenance"]["fanout"]["noDrop"], True)

    def test_workload_owns_timeouts_and_worker_has_no_queue_limit(self):
        env = {key: None for key in ("cpuModel", "effectiveProcessorCount", "cpuQuota", "cpuset", "cpuAffinity", "memoryLimit", "runtimeOptions")}
        env["serializer"] = {"name": "typed JSON"}
        args = options(["single", "--scenario", "session-echo-only", *COMMON])
        self.assertEqual(args.package_source, "published")
        local_args = options(["single", "--scenario", "session-echo-only", "--package-source", "local", *COMMON])
        self.assertEqual(local_args.package_source, "local")
        workload = comparison(args, expand(args, False)[0], env)[0]["workload"]
        self.assertEqual(workload["driverTimeoutMs"], 2000)
        self.assertEqual(workload["setupTimeoutMs"], 30000)
        self.assertEqual(workload["adminTimeoutMs"], 5000)
        self.assertNotIn("settleTimeoutMs", workload)
        worker_args = options(["single", "--scenario", "spot-worker-offload-echo", *COMMON])
        worker = comparison(worker_args, expand(worker_args, False)[0], env)[0]["worker"]
        self.assertNotIn("maxQueueLength", worker)

    def test_merged_quantiles_are_weighted_by_integer_samples(self):
        merged = histogram_merge([histogram({0: 99}), histogram({13: 1})])
        metrics, reasons = {}, {}
        export_latency(merged, "latency", "latencyMs", metrics, reasons)
        self.assertEqual(metrics["latency.p99Ms"], .01)
        self.assertEqual(merged["count"], "100")
        self.assertEqual(merged["sumNs"], "1190000")
        self.assertEqual(metrics["latency.maxMs"], .2)

    def test_percentile_is_capped_at_the_observed_maximum(self):
        value = histogram({1: 1})
        value["maxNs"] = "11000"
        value["sumNs"] = "11000"
        metrics, reasons = {}, {}
        export_latency(histogram_merge([value]), "latency", "latencyMs", metrics, reasons)
        self.assertEqual(metrics["latency.p50Ms"], .011)
        self.assertLessEqual(metrics["latency.p50Ms"], metrics["latency.maxMs"])

    def test_large_nearest_rank_never_loses_integer_precision(self):
        value = histogram({0: (MAX_U64 * 99 + 99) // 100}, overflow=MAX_U64 - (MAX_U64 * 99 + 99) // 100)
        metrics, reasons = {}, {}
        export_latency(histogram_merge([value]), "latency", "latencyMs", metrics, reasons)
        self.assertEqual(metrics["latency.p99Ms"], .01)

    def test_overflow_percentile_is_null_and_mean_uses_exact_sum(self):
        value = histogram_merge([histogram({0: 1}, overflow=1)])
        metrics, reasons = {}, {}
        export_latency(value, "latency", "latencyMs", metrics, reasons)
        self.assertEqual(metrics["latency.p50Ms"], .01)
        self.assertIsNone(metrics["latency.p95Ms"])
        self.assertEqual(reasons["/metrics/latency.p95Ms"]["lowerBoundMs"], BOUNDS[-1])
        self.assertEqual(metrics["latency.meanMs"], (0.01 + OVERFLOW_NS / 1e6) / 2)

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
        def original(role, instance, successes, seconds, request_count, reply_count, inflight=0, failed=0):
            metrics = {"messages." + key: str(value) for key, value in {
                "sent": successes + inflight + failed, "completed": successes, "failed": failed,
                "timeout": 0, "cancelled": 0, "inflightAtEnd": inflight}.items()}
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
                    "metrics": metrics, "histograms": {"latencyMs": histogram({0: successes}) if successes else histogram({})},
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

    def test_inflight_at_end_reconciles_without_an_echo_failure(self):
        config = {"language": "dotnet", "runId": "r", "cellId": "c", "configHash": "h", "scenario": "session-echo-only",
                  "workload": {"payloadSize": 1024}, "topology": None}
        metrics = {"messages." + key: str(value) for key, value in {
            "sent": 5, "completed": 3, "failed": 0, "timeout": 0, "cancelled": 0, "inflightAtEnd": 2}.items()}
        metrics.update({"errors.byKind": {}, "errors.harness": {}, "errors.language": {},
                        "throughput.messagesPerSec": 3, "throughput.megabytesPerSec": 0,
                        "connections.requested": "1", "connections.connected": "1", "connections.failed": "0"})
        for direction in ("request", "send", "reply", "event"):
            metrics["applicationMessages." + direction] = "0"
            metrics["applicationPayloadBytes." + direction] = "0"
        original = {"schemaVersion": 2, **{key: config[key] for key in ("runId", "cellId", "configHash")},
                    "role": "client", "roleInstance": 0, "resetSeq": "1", "phase": "complete",
                    "window": {"startTicks": "0", "endTicks": "2000000000", "measuredSeconds": 2.0},
                    "metrics": metrics, "histograms": {"latencyMs": histogram({0: 3})}, "nullReasons": {},
                    "provenance": {"pid": 100}, "clock": {}}
        server = {**original, "role": "session", "roleInstance": 0,
                  "metrics": {**metrics, "messages.sent": "0", "messages.completed": "0", "messages.inflightAtEnd": "0",
                              "throughput.messagesPerSec": 0}}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_json(root / "client-0.json", original)
            write_json(root / "server-session-0.json", server)
            config["packageSource"] = "local"
            result = aggregate(root, config, ["client-0.json"], ["server-session-0.json"], [], ["client-0.json"])
        self.assertEqual(result["status"], "valid")
        self.assertFalse(result["baselineEligible"])
        self.assertEqual(result["metrics"]["messages.sent"], "5")
        self.assertEqual(result["metrics"]["messages.completed"], "3")
        self.assertEqual(result["metrics"]["messages.inflightAtEnd"], "2")
        self.assertFalse(any(issue["code"] == "EchoOutcomeFailure" for issue in result["reasons"]))



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


class NullReasonValidationTests(unittest.TestCase):
    def test_code_must_be_allowed_and_pointer_must_resolve_to_null(self):
        reason = {"code": "NOT_APPLICABLE", "reason": "Not used.", "owner": "test", "lowerBoundMs": None}
        valid = {"metrics": {"sample": None}, "nullReasons": {"/metrics/sample": reason}}
        _validate_null_reasons("client.json", valid)
        cases = (
            {"metrics": {"sample": None}, "nullReasons": {"/metrics/sample": {**reason, "code": "SETTLE_INCOMPLETE"}}},
            {"metrics": {"sample": None}, "nullReasons": {"/metrics/sample": {**reason, "code": ["NOT_APPLICABLE"]}}},
            {"metrics": {"sample": 1}, "nullReasons": {"/metrics/sample": reason}},
        )
        for original in cases:
            with self.subTest(original=original), self.assertRaisesRegex(ValueError, "SchemaMismatch"):
                _validate_null_reasons("client.json", original)


class FrameworkVersionAgreementTest(unittest.TestCase):
    def test_packages_json_declares_all_supported_framework_versions(self):
        packages = json.loads((Path(__file__).resolve().parents[1] / "schema/packages.json").read_text())
        self.assertEqual(set(packages), {"dotnet", "cpp", "java", "kotlin", "node"})
        self.assertEqual({declared_framework_version(language) for language in packages}, {"0.26.0"})
        self.assertEqual(agreed_framework_version("0.26.0", declared_framework_version("dotnet")), "0.26.0")

    def test_observed_package_version_mismatch_fails(self):
        with self.assertRaises(RuntimeError):
            agreed_framework_version("0.25.0", declared_framework_version("dotnet"))

    def test_runner_passes_declared_version_to_the_build_environment(self):
        from types import SimpleNamespace

        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(output=Path(directory), language="dotnet", perf_dir=Path("/tmp/perf"),
                                   package_source="local")
            with patch("runner.subprocess.run", return_value=SimpleNamespace(returncode=0)) as run:
                build(args, ["Client"])
        self.assertEqual(run.call_args.kwargs["env"]["ZLINK_PERF_FRAMEWORK_VERSION"], "0.26.0")
        self.assertEqual(run.call_args.kwargs["env"]["ZLINK_PERF_PACKAGE_SOURCE"], "local")

    def test_jvm_launchers_build_through_the_java_perf_build_script(self):
        perf = Path("/repo/framework/languages/java/perf")
        for language, perf_dir in (("java", perf), ("kotlin", perf / "kotlin")):
            with self.subTest(language=language):
                command = launcher(language).build(perf_dir, "Client")
                self.assertEqual(command[4:6], ["bash", str(perf / "scripts/build_role.sh")])
                self.assertEqual(command[6], str(perf))

    def test_missing_or_unreported_framework_version_fails(self):
        for observed, declared in ((None, "0.26.0"), ("0.26.0", None)):
            with self.subTest(observed=observed, declared=declared), self.assertRaises(RuntimeError):
                agreed_framework_version(observed, declared)


class ClientControlTests(unittest.TestCase):
    def test_command_reply_skips_a_late_prepared_report(self):
        import io
        import os
        from types import SimpleNamespace
        from runner import ClientControl

        read_end, write_end = os.pipe()
        os.write(write_end, b'{"type": "prepared", "ok": true, "snapshot": {}}\n{"ok": true, "response": {"phase": "complete"}}\n')
        os.close(write_end)
        with tempfile.TemporaryDirectory() as directory, os.fdopen(read_end, "rb") as stdout:
            control = ClientControl(SimpleNamespace(stdout=stdout, stdin=io.BytesIO()), Path(directory) / "control.log")
            self.assertEqual(control.call("stats", None, 1), {"phase": "complete"})
            control.log.close()


class RoleResetTests(unittest.TestCase):
    def test_trigger_or_reset_repeats_while_the_role_reports_undrained_work(self):
        from runner import AdminConflict, post_until_accepted

        url = "http://127.0.0.1:1/perf/reset"
        workload = {"adminTimeoutMs": 1000}
        ack = {"ok": True, "runId": "run", "cellId": "cell", "resetSeq": "1"}
        with patch("runner.post_json", side_effect=[AdminConflict("Admin HTTP 409: not drained"), ack]) as post:
            self.assertEqual(post_until_accepted(url, {}, workload, time.monotonic() + 5), ack)
        self.assertEqual(post.call_count, 2)
        with patch("runner.post_json", side_effect=AdminConflict("Admin HTTP 409: not drained")), \
                self.assertRaises(AdminConflict):
            post_until_accepted(url, {}, workload, time.monotonic() - 1)


class RunExitCodeTests(unittest.TestCase):
    def test_redis_removal_failure_makes_the_run_fail(self):
        from types import SimpleNamespace

        valid_results = [{"status": "valid"}]
        self.assertEqual(run_exit_code(valid_results, SimpleNamespace(removal={"removed": True})), 0)
        self.assertEqual(run_exit_code(valid_results, SimpleNamespace(removal={"removed": False, "error": "still running"})), 1)
        self.assertEqual(run_exit_code([{"status": "failed"}], SimpleNamespace(removal=None)), 1)


if __name__ == "__main__":
    unittest.main()
