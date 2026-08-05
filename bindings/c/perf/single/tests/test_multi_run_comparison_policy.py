import importlib.util
import contextlib
import io
import os
import pathlib
import sys
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[2] / "run_comparison.py"
SPEC = importlib.util.spec_from_file_location("multi_run_comparison", MODULE_PATH)
RC = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = RC
SPEC.loader.exec_module(RC)


def multi_args():
    return {
        "num_runs": 1,
        "recv_mode": "recv",
        "server_ready_timeout_ms": 10000,
        "server_shutdown_timeout_ms": 5000,
        "server_bind_port": 0,
        "transport_transition_ms": 3000,
        "pattern_transition_ms": 3000,
    }


def tier1_metrics(value):
    return (
        ("throughput", value),
        ("bandwidth", value),
        ("latency", value),
        (RC.LATENCY_P95_METRIC, value),
        (RC.LATENCY_P99_METRIC, value),
    )


def reset_auto_hwm_detail_state():
    RC._AUTO_HWM_DETAIL_SEEN.clear()
    RC._AUTO_HWM_DETAIL_ROWS.clear()
    RC._AUTO_HWM_DETAIL_TABLE_SEEN.clear()


def auto_hwm_detail_line(pattern, transport, component, msg_size, **fields):
    values = {
        "pattern": pattern,
        "transport": transport,
        "component": component,
        "socket_type": fields.pop("socket_type", "dealer"),
        "msg_size": str(msg_size),
        "source": fields.pop("source", "socket_snapshot"),
        "role": fields.pop("role", "peer_queue"),
        "unit_budget_bytes": fields.pop("unit_budget_bytes", "524288"),
        "effective_message_bytes": fields.pop(
            "effective_message_bytes", str(msg_size)
        ),
        "sndhwm": fields.pop("sndhwm", "512"),
        "rcvhwm": fields.pop("rcvhwm", "512"),
        "effective_sndbuf": fields.pop("effective_sndbuf", "-1"),
        "effective_rcvbuf": fields.pop("effective_rcvbuf", "-1"),
    }
    values.update({key: str(value) for key, value in fields.items()})
    return "AUTO_HWM_DETAIL," + ",".join(
        f"{key}={value}" for key, value in values.items()
    )


class MultiRunComparisonPolicyTests(unittest.TestCase):
    def test_paired_gate_selects_process_per_peer_router_echo_clients(self):
        previous = os.environ.get("PERF_MULTI_MATCHED_BASELINE")
        try:
            os.environ["PERF_MULTI_MATCHED_BASELINE"] = "1"
            self.assertEqual(
                RC.resolve_binary_names("ROUTER_ROUTER_REQREP")["client"],
                "comp_src_router_router_reqrep_matched_client",
            )
            self.assertEqual(
                RC.resolve_binary_names("ROUTER_ROUTER_SENDSEND")["client"],
                "comp_src_router_router_sendsend_matched_client",
            )
            self.assertEqual(
                RC.resolve_binary_names("ROUTER_ROUTER_ONEWAY")["client"],
                "comp_src_router_router_oneway_client",
            )
        finally:
            if previous is None:
                os.environ.pop("PERF_MULTI_MATCHED_BASELINE", None)
            else:
                os.environ["PERF_MULTI_MATCHED_BASELINE"] = previous

    def test_matched_diagnostic_lines_are_preserved_without_becoming_results(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            RC.emit_benchmark_diag_line(
                "RESULT,current,MULTI_ROUTER_ROUTER_REQREP,tcp,64,throughput,1\n"
            )
            RC.emit_benchmark_diag_line(
                "MATCHED_DIAG,current,MULTI_ROUTER_ROUTER_REQREP,tcp,64,"
                "role=peers,processes=100,contexts=100,sockets=100\n"
            )
        self.assertEqual(
            output.getvalue(),
            (
                "MATCHED_DIAG,current,MULTI_ROUTER_ROUTER_REQREP,tcp,64,"
                "role=peers,processes=100,contexts=100,sockets=100\n"
            ),
        )

    def test_multi_required_result_metrics_are_tier1_only(self):
        self.assertEqual(RC.REQUIRED_RESULT_METRICS, tuple(name for name, _ in tier1_metrics(1.0)))
        self.assertEqual(RC.REQUIRED_RESULT_METRIC_COUNT, 5)

    def test_multi_default_msg_sizes_include_64b(self):
        self.assertEqual(
            RC.MSG_SIZES,
            [64, 256, 1024, 65536, 131072, 262144],
        )
        self.assertEqual(
            RC.STREAM_MSG_SIZES,
            [64, 256, 1024, 65536],
        )

    def test_result_filename_uses_current_mode_label(self):
        old_allow_multi = RC.ALLOW_MULTI
        try:
            RC.ALLOW_MULTI = True
            self.assertRegex(
                RC.build_result_filename("tag"),
                r"^perf_c_multi_[a-z]+_\d{8}_\d{6}_tag\.txt$",
            )
        finally:
            RC.ALLOW_MULTI = old_allow_multi

    def test_expand_pattern_aliases_ordered_preserves_request_order(self):
        self.assertEqual(
            RC.expand_pattern_aliases_ordered(
                ["DEALER_DEALER", "STREAMS"]
            ),
            ["DEALER_DEALER", "STREAM"],
        )

    def test_collect_unsupported_patterns_matches_current_recv_matrix(self):
        self.assertEqual(
            RC.collect_unsupported_patterns(
                ["PUBSUB", "STREAM"], "recv"
            ),
            [],
        )
        self.assertEqual(
            RC.collect_unsupported_patterns(
                ["DEALER_DEALER", "PUBSUB", "STREAM"],
                "recv",
            ),
            [],
        )

    def test_multi_connect_concurrency_header_uses_multi_env_name(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_env = os.environ.copy()
        try:
            RC.ALLOW_MULTI = True
            os.environ["PERF_MULTI_CONNECT_CONCURRENCY"] = "321"
            items = dict(
                RC.build_effective_option_items(multi_args(), ["DEALER_DEALER"])
            )
            self.assertEqual(items["connect_concurrency"], "321")
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_defaults_do_not_require_internal_default_envs(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_env = os.environ.copy()
        try:
            RC.ALLOW_MULTI = True
            for key in (
                "PERF_MULTI_DEFAULT_CLIENTS",
                "PERF_MULTI_DEFAULT_STREAM_CLIENTS",
                "PERF_MULTI_DEFAULT_HWM",
                "PERF_MULTI_DEFAULT_STREAM_HWM",
                "PERF_MULTI_STREAM_DEFAULT_IO_THREADS",
            ):
                os.environ.pop(key, None)
            self.assertEqual(RC.pattern_default_clients("DEALER_DEALER"), 100)
            self.assertEqual(RC.pattern_default_clients("ROUTER_ROUTER_REQREP"), 100)
            self.assertEqual(RC.pattern_default_clients("STREAM"), 10000)
            self.assertEqual(RC.pattern_default_hwm("DEALER_DEALER"), 0)
            self.assertEqual(RC.pattern_default_hwm("STREAM"), 0)
            self.assertEqual(RC.pattern_default_io_threads("DEALER_DEALER"), 4)
            self.assertEqual(RC.pattern_default_io_threads("STREAM"), 4)
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            os.environ.clear()
            os.environ.update(old_env)

    def test_router_router_oneway_is_direction_matched_one_way_baseline(self):
        old_allow_multi = RC.ALLOW_MULTI
        try:
            RC.ALLOW_MULTI = True
            self.assertEqual(
                RC.resolve_binary_names("ROUTER_ROUTER_ONEWAY"),
                {
                    "server": "comp_src_router_router_oneway_server",
                    "client": "comp_src_router_router_oneway_client",
                },
            )
            self.assertFalse(RC.is_echo_pattern("ROUTER_ROUTER_ONEWAY"))
            self.assertEqual(
                RC.compute_bandwidth_mb_s("ROUTER_ROUTER_ONEWAY", 64, 1000),
                0.064,
            )
            self.assertEqual(
                RC.pattern_default_clients("ROUTER_ROUTER_ONEWAY"),
                100,
            )
            self.assertEqual(
                RC.pattern_default_io_threads("ROUTER_ROUTER_ONEWAY"),
                1,
            )
        finally:
            RC.ALLOW_MULTI = old_allow_multi

    def test_multi_split_runner_isolates_each_size_case(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_split = RC.run_sizes_test_split
        old_env = os.environ.copy()
        calls = []
        try:
            RC.ALLOW_MULTI = True

            def fake_split(server_name, client_name, lib_name, transport, sizes,
                           pattern_name, result_line_callback=None, **kwargs):
                calls.append(
                    (server_name, client_name, lib_name, transport,
                     list(sizes), pattern_name)
                )
                return {
                    "status": "success",
                    "parsed": {},
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }

            RC.run_sizes_test_split = fake_split
            outcome = RC.run_sizes_test(
                "ignored",
                "current",
                "tcp",
                [64, 256, 1024],
                "PUBSUB",
            )

            self.assertEqual(outcome["status"], "success")
            self.assertEqual(
                calls,
                [
                    ("comp_src_pubsub_server", "comp_src_pubsub_client",
                     "current", "tcp", [64], "PUBSUB"),
                    ("comp_src_pubsub_server", "comp_src_pubsub_client",
                     "current", "tcp", [256], "PUBSUB"),
                    ("comp_src_pubsub_server", "comp_src_pubsub_client",
                     "current", "tcp", [1024], "PUBSUB"),
                ],
            )
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test_split = old_split
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_stream_runner_isolates_each_size_case(self):
        old_stream = RC.run_sizes_test_stream_shared
        calls = []
        try:
            def fake_stream(server_name, lib_name, transport, sizes,
                            pattern_name, result_line_callback=None):
                calls.append(
                    (server_name, lib_name, transport, list(sizes), pattern_name)
                )
                return {
                    "status": "success",
                    "parsed": {},
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }

            RC.run_sizes_test_stream_shared = fake_stream
            outcome = RC.run_sizes_test(
                "ignored",
                "current",
                "wss",
                [64, 256],
                "STREAM",
            )

            self.assertEqual(outcome["status"], "success")
            self.assertEqual(
                calls,
                [
                    ("comp_src_stream_server", "current", "wss", [64], "STREAM"),
                    ("comp_src_stream_server", "current", "wss", [256], "STREAM"),
                ],
            )
        finally:
            RC.run_sizes_test_stream_shared = old_stream

    def test_multi_stream_default_sizes_ignore_non_stream_multi_sizes(self):
        old_env = os.environ.copy()
        try:
            os.environ["PERF_MSG_SIZES"] = "64,256,1024,4096,65536,131072"
            spec = importlib.util.spec_from_file_location(
                "multi_run_comparison_stream_size_policy", MODULE_PATH
            )
            reloaded = importlib.util.module_from_spec(spec)
            assert spec and spec.loader
            spec.loader.exec_module(reloaded)
            self.assertEqual(reloaded.STREAM_MSG_SIZES, [64, 256, 1024, 65536])
        finally:
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_sizes_run_as_isolated_cases_without_transition_sleep(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_split = RC.run_sizes_test_split
        old_sleep = RC.time.sleep
        old_env = os.environ.copy()
        calls = []
        sleeps = []
        try:
            RC.ALLOW_MULTI = True

            def fake_split(server_name, client_name, lib_name, transport, sizes,
                           pattern_name, result_line_callback=None, **kwargs):
                calls.append(list(sizes))
                return {
                    "status": "success",
                    "parsed": {},
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }

            def fake_sleep(seconds):
                sleeps.append(seconds)

            RC.run_sizes_test_split = fake_split
            RC.time.sleep = fake_sleep

            outcome = RC.run_sizes_test(
                "ignored",
                "current",
                "tcp",
                [64, 256, 1024],
                "PUBSUB",
            )

            self.assertEqual(outcome["status"], "success")
            self.assertEqual(calls, [[64], [256], [1024]])
            self.assertEqual(sleeps, [])
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test_split = old_split
            RC.time.sleep = old_sleep
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_size_case_does_not_retry_before_failing(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_split = RC.run_sizes_test_split
        old_env = os.environ.copy()
        calls = []
        try:
            RC.ALLOW_MULTI = True

            def fake_split(server_name, client_name, lib_name, transport, sizes,
                           pattern_name, result_line_callback=None, **kwargs):
                size = sizes[0]
                calls.append(size)
                if len(calls) == 1:
                    return {
                        "status": "fail",
                        "parsed": {},
                        "timed_out": False,
                        "returncode": 1,
                        "reason": "client_ready",
                        "warnings": [],
                    }
                return {
                    "status": "success",
                    "parsed": {f"tcp|{size}|throughput": 1.0},
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }

            RC.run_sizes_test_split = fake_split

            outcome = RC.run_sizes_test(
                "ignored",
                "current",
                "tcp",
                [65536],
                "PUBSUB",
            )

            self.assertEqual(outcome["status"], "fail")
            self.assertEqual(outcome["reason"], "client_ready_size_65536")
            self.assertEqual(calls, [65536])
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test_split = old_split
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_transport_cooldown_runs_only_between_transports(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_run_sizes_test = RC.run_sizes_test
        old_sleep = RC.time.sleep
        old_env = os.environ.copy()
        calls = []
        sleeps = []
        try:
            RC.ALLOW_MULTI = True
            os.environ["PERF_RUN_COOLDOWN_MS"] = "0"
            os.environ["PERF_TRANSPORT_TRANSITION_MS"] = "23"

            def fake_run_sizes_test(binary_name, lib_name, transport, sizes,
                                    pattern_name, result_line_callback=None):
                calls.append((transport, list(sizes), pattern_name))
                parsed = {}
                if result_line_callback is not None:
                    for size in sizes:
                        for metric_name, value in tier1_metrics(1.0):
                            result_line_callback(transport, size, metric_name, value)
                            parsed[f"{transport}|{size}|{metric_name}"] = value
                return {
                    "status": "success",
                    "parsed": parsed,
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }

            def fake_sleep(seconds):
                sleeps.append(seconds)

            RC.run_sizes_test = fake_run_sizes_test
            RC.time.sleep = fake_sleep

            final_stats, failures = RC.collect_data(
                "ignored",
                "current",
                "PUBSUB",
                1,
                transports=["tcp", "tls", "ws"],
                table_lines=[],
            )

            self.assertEqual(failures, [])
            self.assertEqual(
                calls,
                [
                    ("tcp", RC.MSG_SIZES, "PUBSUB"),
                    ("tls", RC.MSG_SIZES, "PUBSUB"),
                    ("ws", RC.MSG_SIZES, "PUBSUB"),
                ],
            )
            self.assertEqual(sleeps, [0.023, 0.023])
            self.assertIn("tcp|256|throughput", final_stats)
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test = old_run_sizes_test
            RC.time.sleep = old_sleep
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_size_result_table_uses_parsed_result(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_run_sizes_test = RC.run_sizes_test
        old_msg_sizes = RC.MSG_SIZES
        old_env = os.environ.copy()
        callbacks = []
        start_callbacks = []
        result_callbacks = []
        try:
            RC.ALLOW_MULTI = True
            RC.MSG_SIZES = [65536]
            os.environ["PERF_RUN_COOLDOWN_MS"] = "0"
            os.environ["PERF_TRANSPORT_TRANSITION_MS"] = "0"

            def fake_run_sizes_test(binary_name, lib_name, transport, sizes,
                                    pattern_name, result_line_callback=None,
                                    size_start_callback=None,
                                    size_result_callback=None):
                callbacks.append(result_line_callback)
                if size_start_callback is not None:
                    size_start_callback(transport, sizes[0])
                    start_callbacks.append((transport, sizes[0]))
                if result_line_callback is not None:
                    for metric_name, value in tier1_metrics(999.0):
                        result_line_callback(transport, sizes[0], metric_name, value)
                outcome = {
                    "status": "success",
                    "parsed": {
                        "tcp|65536|throughput": 123.0,
                        "tcp|65536|bandwidth": 456.0,
                        "tcp|65536|latency": 1.5,
                        "tcp|65536|latency_p95": 2.5,
                        "tcp|65536|latency_p99": 3.5,
                    },
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }
                if size_result_callback is not None:
                    size_result_callback(transport, sizes[0], outcome)
                    result_callbacks.append((transport, sizes[0]))
                return outcome

            RC.run_sizes_test = fake_run_sizes_test

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                RC.collect_data(
                    "ignored",
                    "current",
                    "PUBSUB",
                    1,
                    transports=["tcp"],
                    table_lines=[],
                )

            output = stdout.getvalue()
            self.assertEqual(len(callbacks), 1)
            self.assertIsNotNone(callbacks[0])
            self.assertEqual(start_callbacks, [("tcp", 65536)])
            self.assertEqual(result_callbacks, [("tcp", 65536)])
            self.assertIn("Testing tcp | 65536B:", output)
            self.assertIn("999.000 ms", output)
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test = old_run_sizes_test
            RC.MSG_SIZES = old_msg_sizes
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_collect_data_reports_each_size_separately(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_run_sizes_test = RC.run_sizes_test
        old_env = os.environ.copy()
        try:
            RC.ALLOW_MULTI = True
            os.environ["PERF_RUN_COOLDOWN_MS"] = "0"
            os.environ["PERF_TRANSPORT_TRANSITION_MS"] = "0"

            def fake_run_sizes_test(binary_name, lib_name, transport, sizes,
                                    pattern_name, result_line_callback=None):
                for size in sizes:
                    if result_line_callback is not None:
                        for metric_name, value in tier1_metrics(1.0):
                            result_line_callback(transport, size, metric_name, value)
                return {
                    "status": "success",
                    "parsed": {
                        "tcp|64|throughput": 1.0,
                        "tcp|64|bandwidth": 1.0,
                        "tcp|64|latency": 1.0,
                        "tcp|64|latency_p95": 1.0,
                        "tcp|64|latency_p99": 1.0,
                        "tcp|256|throughput": 1.0,
                        "tcp|256|bandwidth": 1.0,
                        "tcp|256|latency": 1.0,
                        "tcp|256|latency_p95": 1.0,
                        "tcp|256|latency_p99": 1.0,
                    },
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }

            RC.run_sizes_test = fake_run_sizes_test

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                RC.collect_data(
                    "ignored",
                    "current",
                    "PUBSUB",
                    1,
                    transports=["tcp"],
                    table_lines=[],
                )

            output = stdout.getvalue()
            self.assertIn("Testing tcp | 64B:", output)
            self.assertIn("Testing tcp | 256B:", output)
            self.assertNotIn("Testing tcp | 64B,256B:", output)
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test = old_run_sizes_test
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_collect_data_omits_legacy_queue_metrics(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_run_sizes_test = RC.run_sizes_test
        old_env = os.environ.copy()
        try:
            RC.ALLOW_MULTI = True
            os.environ["PERF_RUN_COOLDOWN_MS"] = "0"
            os.environ["PERF_TRANSPORT_TRANSITION_MS"] = "0"

            def fake_run_sizes_test(binary_name, lib_name, transport, sizes,
                                    pattern_name, result_line_callback=None):
                for size in sizes:
                    if result_line_callback is not None:
                        for metric_name, value in tier1_metrics(1.0):
                            result_line_callback(transport, size, metric_name, value)
                return {
                    "status": "success",
                    "parsed": {
                        "tcp|64|throughput": 1.0,
                        "tcp|64|bandwidth": 1.0,
                        "tcp|64|latency": 1.0,
                        "tcp|64|latency_p95": 1.0,
                        "tcp|64|latency_p99": 1.0,
                    },
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }

            RC.run_sizes_test = fake_run_sizes_test

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                RC.collect_data(
                    "ignored",
                    "current",
                    "PUBSUB",
                    1,
                    transports=["tcp"],
                    table_lines=[],
                )

            output = stdout.getvalue()
            for metric_name in ("Q.Snd.Max", "Q.Rcv.Max", "Q.Rcv.End"):
                self.assertNotIn(metric_name, output)
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test = old_run_sizes_test
            os.environ.clear()
            os.environ.update(old_env)

    def test_auto_hwm_detail_collapses_transport_dimension(self):
        reset_auto_hwm_detail_state()
        try:
            for transport in ("tcp", "tls", "ws", "wss"):
                for component in ("client", "server"):
                    RC.emit_auto_hwm_detail_line(
                        auto_hwm_detail_line(
                            "DEALER_DEALER",
                            transport,
                            component,
                            64,
                            sndhwm="512",
                            rcvhwm="512",
                        )
                    )
                    RC.emit_auto_hwm_detail_line(
                        auto_hwm_detail_line(
                            "DEALER_DEALER",
                            transport,
                            component,
                            65536,
                            effective_message_bytes="65536",
                            sndhwm="8",
                            rcvhwm="8",
                        )
                    )

            lines = []
            self.assertTrue(
                RC.emit_auto_hwm_detail_table(lines.append, "DEALER_DEALER")
            )
            output = "\n".join(lines)
            self.assertIn("| Size(B) | Component | Type", output)
            self.assertNotIn("Transport", output)
            self.assertEqual(output.count("| 64      |"), 2)
            self.assertEqual(output.count("| 65536   |"), 2)
            self.assertIn("| 64      | client", output)
            self.assertIn("| 64      | server", output)
            self.assertIn("| 65536   | client", output)
            self.assertIn("| 65536   | server", output)
            self.assertIn("| 8      | 8", output)
        finally:
            reset_auto_hwm_detail_state()

    def test_auto_hwm_detail_prefers_expected_hwm_when_transport_samples_conflict(self):
        reset_auto_hwm_detail_state()
        try:
            RC.emit_auto_hwm_detail_line(
                auto_hwm_detail_line(
                    "DEALER_ROUTER",
                    "tcp",
                    "server",
                    131072,
                    socket_type="router",
                    effective_message_bytes="131072",
                    size_cap="512",
                    sndhwm="128",
                    rcvhwm="128",
                    effective_sndbuf="524288",
                    effective_rcvbuf="524288",
                )
            )
            RC.emit_auto_hwm_detail_line(
                auto_hwm_detail_line(
                    "DEALER_ROUTER",
                    "tls",
                    "server",
                    131072,
                    socket_type="router",
                    effective_message_bytes="131072",
                    size_cap="512",
                    sndhwm="4",
                    rcvhwm="4",
                    effective_sndbuf="524288",
                    effective_rcvbuf="524288",
                )
            )

            lines = []
            self.assertTrue(
                RC.emit_auto_hwm_detail_table(lines.append, "DEALER_ROUTER")
            )
            output = "\n".join(lines)
            self.assertEqual(output.count("| 131072  | server"), 1)
            self.assertIn("| 131072  | server    | router", output)
            self.assertIn("| 4      | 4", output)
            self.assertNotIn("| 128    | 128", output)
            self.assertNotIn("Transport", output)
        finally:
            reset_auto_hwm_detail_state()

    def test_stream_auto_hwm_detail_reports_stream_socket_type(self):
        reset_auto_hwm_detail_state()
        try:
            for size, hwm in ((64, 128), (1024, 64), (65536, 1)):
                RC.emit_auto_hwm_detail_line(
                    auto_hwm_detail_line(
                        "STREAM",
                        "tcp",
                        "server",
                        size,
                        socket_type="stream",
                        role="stream",
                        unit_budget_bytes="65536",
                        effective_message_bytes=str(size),
                        sndhwm=str(hwm),
                        rcvhwm=str(hwm),
                    )
                )

            lines = []
            self.assertTrue(RC.emit_auto_hwm_detail_table(lines.append, "STREAM"))
            output = "\n".join(lines)
            self.assertIn("| Size(B) | Component | Type", output)
            self.assertIn("| 64      | server    | stream", output)
            self.assertIn("| 1024    | server    | stream", output)
            self.assertIn("| 65536   | server    | stream", output)
            self.assertNotIn("unknown", output)
            self.assertNotIn("Transport", output)
        finally:
            reset_auto_hwm_detail_state()


if __name__ == "__main__":
    unittest.main()
