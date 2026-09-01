import importlib.util
import contextlib
import io
import os
import pathlib
import sys
import threading
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[2] / "run_comparison.py"
SPEC = importlib.util.spec_from_file_location("multi_run_comparison", MODULE_PATH)
RC = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = RC
SPEC.loader.exec_module(RC)

CPP_MODULE_PATH = MODULE_PATH.parents[2] / "cpp" / "perf" / "run_comparison.py"
CPP_SPEC = importlib.util.spec_from_file_location(
    "cpp_multi_run_comparison", CPP_MODULE_PATH
)
CPP_RC = importlib.util.module_from_spec(CPP_SPEC)
assert CPP_SPEC and CPP_SPEC.loader
sys.modules[CPP_SPEC.name] = CPP_RC
CPP_SPEC.loader.exec_module(CPP_RC)

RUNNER_MODULES = (("c", RC), ("cpp", CPP_RC))


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
    def test_stream_server_start_ready_token_parser(self):
        self.assertEqual(RC.parse_server_start_ready_size("SERVER_START_READY,1024\n"), 1024)
        self.assertIsNone(RC.parse_server_start_ready_size("SERVER_START_READY,0\n"))
        self.assertIsNone(RC.parse_server_start_ready_size("PHASE_ACTIVE,1024\n"))

    def test_stream_start_barrier_waits_for_server_ack(self):
        class FakeProcess:
            def __init__(self):
                self.stdin = io.StringIO()

        args = RC.build_stream_shared_client_args(
            "tcp", "STREAM", "1024", 1, 100, 4
        )
        start_gate_index = args.index("--start-gate")
        self.assertEqual(args[start_gate_index + 1], "1")

        server = FakeProcess()
        client = FakeProcess()
        pending_ready_sizes = set()
        requested_sizes = set()

        ready_size = RC.parse_client_ready_size("CLIENT_READY,1024\n")
        pending_ready_sizes.add(ready_size)
        self.assertTrue(
            RC.request_stream_server_start(server, ready_size, requested_sizes)
        )
        self.assertEqual(server.stdin.getvalue(), "START,1024\n")
        self.assertEqual(client.stdin.getvalue(), "")

        self.assertFalse(
            RC.forward_stream_start_ack(
                client,
                "AUTO_HWM_DETAIL,pattern=STREAM\n",
                pending_ready_sizes,
                requested_sizes,
            )
        )
        self.assertEqual(client.stdin.getvalue(), "")

        self.assertTrue(
            RC.forward_stream_start_ack(
                client,
                "SERVER_START_READY,1024\n",
                pending_ready_sizes,
                requested_sizes,
            )
        )
        self.assertEqual(client.stdin.getvalue(), "START,1024\n")
        self.assertEqual(pending_ready_sizes, set())
        self.assertEqual(requested_sizes, set())

    def test_stream_start_request_publishes_state_before_write_and_keeps_ack(self):
        class FakeProcess:
            def __init__(self, stdin):
                self.stdin = stdin

        class BlockingStdin:
            def __init__(self):
                self.write_entered = threading.Event()
                self.release_write = threading.Event()
                self.value = ""

            def write(self, value):
                self.value += value
                self.write_entered.set()
                if not self.release_write.wait(timeout=1.0):
                    raise TimeoutError("test did not release START write")
                return len(value)

            def flush(self):
                return None

        for runner_name, runner in RUNNER_MODULES:
            with self.subTest(runner=runner_name):
                server_stdin = BlockingStdin()
                server = FakeProcess(server_stdin)
                client_stdin = io.StringIO()
                client = FakeProcess(client_stdin)
                pending_ready_sizes = {1024}
                requested_sizes = set()
                requested_start_times = {}
                state_lock = threading.Lock()
                request_result = []
                ack_result = []
                ack_started = threading.Event()

                request_thread = threading.Thread(
                    target=lambda: request_result.append(
                        runner.request_stream_server_start(
                            server,
                            1024,
                            requested_sizes,
                            requested_start_times,
                            state_lock,
                        )
                    )
                )
                request_thread.start()
                self.assertTrue(server_stdin.write_entered.wait(timeout=1.0))
                self.assertEqual(requested_sizes, {1024})
                self.assertIn(1024, requested_start_times)
                state_lock_was_free = state_lock.acquire(blocking=False)
                if state_lock_was_free:
                    state_lock.release()
                self.assertFalse(state_lock_was_free)

                def forward_ack():
                    ack_started.set()
                    ack_result.append(
                        runner.forward_stream_start_ack(
                            client,
                            "SERVER_START_READY,1024\n",
                            pending_ready_sizes,
                            requested_sizes,
                            requested_start_times,
                            state_lock,
                        )
                    )

                ack_thread = threading.Thread(target=forward_ack)
                ack_thread.start()
                self.assertTrue(ack_started.wait(timeout=1.0))
                server_stdin.release_write.set()
                request_thread.join(timeout=1.0)
                ack_thread.join(timeout=1.0)

                self.assertFalse(request_thread.is_alive())
                self.assertFalse(ack_thread.is_alive())
                self.assertEqual(request_result, [True])
                self.assertEqual(ack_result, [True])
                self.assertEqual(server_stdin.value, "START,1024\n")
                self.assertEqual(client_stdin.getvalue(), "START,1024\n")
                self.assertEqual(pending_ready_sizes, set())
                self.assertEqual(requested_sizes, set())
                self.assertEqual(requested_start_times, {})

    def test_stream_start_request_rolls_back_published_state_on_flush_failure(self):
        class FailingStdin:
            def __init__(self, requested_sizes, requested_start_times):
                self.requested_sizes = requested_sizes
                self.requested_start_times = requested_start_times
                self.state_was_published = False

            def write(self, value):
                self.state_was_published = (
                    value == "START,1024\n"
                    and 1024 in self.requested_sizes
                    and 1024 in self.requested_start_times
                )
                return len(value)

            def flush(self):
                raise OSError("simulated flush failure")

        class FakeProcess:
            def __init__(self, stdin):
                self.stdin = stdin

        for runner_name, runner in RUNNER_MODULES:
            with self.subTest(runner=runner_name):
                requested_sizes = {2048}
                requested_start_times = {2048: 1.0}
                server_stdin = FailingStdin(
                    requested_sizes, requested_start_times
                )

                self.assertFalse(
                    runner.request_stream_server_start(
                        FakeProcess(server_stdin),
                        1024,
                        requested_sizes,
                        requested_start_times,
                        threading.Lock(),
                    )
                )
                self.assertTrue(server_stdin.state_was_published)
                self.assertEqual(requested_sizes, {2048})
                self.assertEqual(requested_start_times, {2048: 1.0})

    def test_stream_start_barrier_stops_client_when_server_exits(self):
        class FakeServer:
            def poll(self):
                return 1

        class FakeClient:
            def __init__(self):
                self.stdin = io.StringIO()

        client = FakeClient()
        self.assertTrue(
            RC.stop_stream_client_if_server_exited(
                FakeServer(), client, {1024}
            )
        )
        self.assertEqual(client.stdin.getvalue(), "STOP\n")
        self.assertFalse(
            RC.stop_stream_client_if_server_exited(
                FakeServer(), client, set()
            )
        )

    def test_requested_transport_order_is_preserved(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_env_transports = RC._env_transports
        try:
            RC.ALLOW_MULTI = True
            RC._env_transports = ["wss", "tcp", "wss"]
            self.assertEqual(
                RC.select_transports("DEALER_ROUTER_REQREP"),
                ["wss", "tcp"],
            )
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC._env_transports = old_env_transports

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
                RC.resolve_split_required_binaries("ROUTER_ROUTER_REQREP"),
                [
                    "comp_src_router_router_reqrep_server",
                    "comp_src_router_router_reqrep_matched_client",
                ],
            )
            self.assertEqual(
                RC.resolve_split_required_binaries("ROUTER_ROUTER_SENDSEND"),
                [
                    "comp_src_router_router_sendsend_server",
                    "comp_src_router_router_sendsend_matched_client",
                ],
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
            self.assertEqual(RC.pattern_default_clients("STREAM"), 100)
            self.assertEqual(RC.pattern_default_hwm("DEALER_DEALER"), 0)
            self.assertEqual(RC.pattern_default_hwm("STREAM"), 0)
            self.assertEqual(RC.pattern_default_io_threads("DEALER_DEALER"), 4)
            self.assertEqual(RC.pattern_default_io_threads("STREAM"), 4)
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            os.environ.clear()
            os.environ.update(old_env)

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

    def test_multi_size_failure_continues_without_merging_failed_metrics(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_split = RC.run_sizes_test_split
        calls = []
        result_callbacks = []
        size_callbacks = []
        callback_events = []
        try:
            RC.ALLOW_MULTI = True

            def fake_split(server_name, client_name, lib_name, transport, sizes,
                           pattern_name, result_line_callback=None, **kwargs):
                size = sizes[0]
                calls.append(size)
                parsed = {
                    f"tcp|{size}|{metric_name}": float(size)
                    for metric_name, _ in tier1_metrics(1.0)
                }
                if size == 65536:
                    return {
                        "status": "fail",
                        "parsed": parsed,
                        "timed_out": False,
                        "returncode": 1,
                        "reason": "server_non_zero_exit_1",
                        "warnings": [],
                    }
                return {
                    "status": "success",
                    "parsed": parsed,
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
                [65536, 131072],
                "DEALER_ROUTER_REQREP",
                result_line_callback=lambda tr, size, metric, value: (
                    result_callbacks.append((tr, size, metric, value)),
                    callback_events.append(("metric", size, metric)),
                ),
                size_result_callback=lambda tr, size, item: (
                    size_callbacks.append((tr, size, item["status"])),
                    callback_events.append(("terminal", size, item["status"])),
                ),
            )

            self.assertEqual(calls, [65536, 131072])
            self.assertEqual(outcome["status"], "fail")
            self.assertEqual(
                outcome["reason"],
                "server_non_zero_exit_1_size_65536",
            )
            self.assertEqual(
                outcome["size_failures"],
                {
                    65536: {
                        "reason": "server_non_zero_exit_1",
                        "timed_out": False,
                        "returncode": 1,
                    }
                },
            )
            self.assertNotIn("tcp|65536|throughput", outcome["parsed"])
            self.assertEqual(outcome["parsed"]["tcp|131072|throughput"], 131072.0)
            self.assertEqual(
                {size for _tr, size, _metric, _value in result_callbacks},
                {131072},
            )
            self.assertEqual(
                size_callbacks,
                [
                    ("tcp", 65536, "fail"),
                    ("tcp", 131072, "success"),
                ],
            )
            self.assertEqual(callback_events[0], ("terminal", 65536, "fail"))
            self.assertEqual(callback_events[-1], ("terminal", 131072, "success"))
            self.assertTrue(
                all(event[0] == "metric" for event in callback_events[1:-1])
            )
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test_split = old_split

    def test_split_runner_releases_reqrep_client_after_server_exit(self):
        old_popen = RC.subprocess.Popen
        old_run_command = RC.run_command_with_metrics
        events = []

        class RecordingStdin:
            def __init__(self, name, owner):
                self.name = name
                self.owner = owner
                self.closed = False

            def write(self, text):
                token = text.strip()
                events.append(f"{self.name}:{token}")
                if self.name == "client" and token == "STOP":
                    self.owner.returncode = 0
                return len(text)

            def flush(self):
                return None

            def close(self):
                self.closed = True

        class FakeServerProcess:
            def __init__(self):
                self.returncode = None
                self.stdin = RecordingStdin("server", self)
                self.stdout = io.StringIO("READY,tcp://127.0.0.1:5555\n")
                self.stderr = io.StringIO("")

            def poll(self):
                return self.returncode

            def wait(self, timeout=None):
                events.append("server:exit")
                self.returncode = 0
                return 0

            def terminate(self):
                self.returncode = -15

            def kill(self):
                self.returncode = -9

        class FakeClientProcess:
            def __init__(self):
                self.returncode = None
                self.stdin = RecordingStdin("client", self)

            def poll(self):
                return self.returncode

        result_lines = "".join(
            "RESULT,current,MULTI_DEALER_ROUTER_REQREP,tcp,65536,"
            f"{metric_name},1.0\n"
            for metric_name, _ in tier1_metrics(1.0)
        )

        def fake_popen(*args, **kwargs):
            return FakeServerProcess()

        def fake_run_command(cmd, env, timeout_sec, on_stdout_line=None,
                             on_stderr_line=None, on_sample=None,
                             on_process_start=None):
            client = FakeClientProcess()
            if on_process_start is not None:
                on_process_start(client)
            if on_stdout_line is not None:
                for line in result_lines.splitlines(True):
                    on_stdout_line(line)
                on_stdout_line("CLIENT_DONE,65536\n")
            self.assertEqual(client.returncode, 0)
            return {
                "returncode": 0,
                "stdout": result_lines + "CLIENT_DONE,65536\n",
                "stderr": "",
                "timed_out": False,
            }

        try:
            RC.subprocess.Popen = fake_popen
            RC.run_command_with_metrics = fake_run_command
            outcome = RC.run_sizes_test_split(
                "server",
                "client",
                "current",
                "tcp",
                [65536],
                "DEALER_ROUTER_REQREP",
            )

            self.assertEqual(outcome["status"], "success")
            self.assertLess(events.index("server:STOP"), events.index("server:exit"))
            self.assertLess(events.index("server:exit"), events.index("client:STOP"))
        finally:
            RC.subprocess.Popen = old_popen
            RC.run_command_with_metrics = old_run_command

    def test_split_runner_does_not_wait_for_dealer_done_callback(self):
        old_popen = RC.subprocess.Popen
        old_run_command = RC.run_command_with_metrics
        events = []
        server_ref = []

        class RecordingStdin:
            def __init__(self, name):
                self.name = name

            def write(self, text):
                events.append(f"{self.name}:{text.strip()}")
                return len(text)

            def flush(self):
                return None

            def close(self):
                return None

        class FakeServerProcess:
            def __init__(self):
                self.returncode = None
                self.stdin = RecordingStdin("server")
                self.stdout = io.StringIO("READY,tcp://127.0.0.1:5555\n")
                self.stderr = io.StringIO("")

            def poll(self):
                return self.returncode

            def wait(self, timeout=None):
                events.append("server:exit")
                self.returncode = 0
                return 0

            def terminate(self):
                self.returncode = -15

            def kill(self):
                self.returncode = -9

        class FakeClientProcess:
            def __init__(self):
                self.returncode = None
                self.stdin = RecordingStdin("client")

            def poll(self):
                return self.returncode

        result_lines = "".join(
            "RESULT,current,MULTI_DEALER_DEALER,tcp,65536,"
            f"{metric_name},1.0\n"
            for metric_name, _ in tier1_metrics(1.0)
        )

        def fake_popen(*args, **kwargs):
            server = FakeServerProcess()
            server_ref.append(server)
            return server

        def fake_run_command(cmd, env, timeout_sec, on_stdout_line=None,
                             on_stderr_line=None, on_sample=None,
                             on_process_start=None):
            client = FakeClientProcess()
            if on_process_start is not None:
                on_process_start(client)
            if on_stdout_line is not None:
                on_stdout_line("CLIENT_DONE,65536\n")
            events.append("client:callback-return")
            self.assertIn("server:STOP", events)
            self.assertNotIn("server:exit", events)
            self.assertNotIn("client:STOP", events)
            client.returncode = 0
            server_ref[0].returncode = 0
            return {
                "returncode": 0,
                "stdout": result_lines + "CLIENT_DONE,65536\n",
                "stderr": "",
                "timed_out": False,
            }

        try:
            RC.subprocess.Popen = fake_popen
            RC.run_command_with_metrics = fake_run_command
            outcome = RC.run_sizes_test_split(
                "server",
                "client",
                "current",
                "tcp",
                [65536],
                "DEALER_DEALER",
            )

            self.assertEqual(outcome["status"], "success")
            self.assertNotIn("server:exit", events)
            self.assertNotIn("client:STOP", events)
        finally:
            RC.subprocess.Popen = old_popen
            RC.run_command_with_metrics = old_run_command

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
            self.assertIn("0.123 Kmsg/s", output)
            self.assertIn("1.500 ms", output)
            self.assertNotIn("999.000 ms", output)
        finally:
            RC.ALLOW_MULTI = old_allow_multi
            RC.run_sizes_test = old_run_sizes_test
            RC.MSG_SIZES = old_msg_sizes
            os.environ.clear()
            os.environ.update(old_env)

    def test_multi_collect_data_attributes_failure_to_exact_size(self):
        old_allow_multi = RC.ALLOW_MULTI
        old_run_sizes_test = RC.run_sizes_test
        old_msg_sizes = RC.MSG_SIZES
        old_env = os.environ.copy()
        try:
            RC.ALLOW_MULTI = True
            RC.MSG_SIZES = [65536, 131072]
            os.environ["PERF_RUN_COOLDOWN_MS"] = "0"
            os.environ["PERF_TRANSPORT_TRANSITION_MS"] = "0"

            def fake_run_sizes_test(binary_name, lib_name, transport, sizes,
                                    pattern_name, result_line_callback=None,
                                    size_start_callback=None,
                                    size_result_callback=None):
                parsed = {
                    f"tcp|131072|{metric_name}": value
                    for metric_name, value in tier1_metrics(10.0)
                }
                failed_size_outcome = {
                    "status": "fail",
                    "parsed": {},
                    "timed_out": False,
                    "returncode": 1,
                    "reason": "server_non_zero_exit_1",
                    "warnings": [],
                }
                successful_size_outcome = {
                    "status": "success",
                    "parsed": parsed,
                    "timed_out": False,
                    "returncode": 0,
                    "reason": "",
                    "warnings": [],
                }
                if size_start_callback is not None:
                    size_start_callback(transport, 65536)
                if size_result_callback is not None:
                    size_result_callback(transport, 65536, failed_size_outcome)
                if size_start_callback is not None:
                    size_start_callback(transport, 131072)
                if size_result_callback is not None:
                    size_result_callback(transport, 131072, successful_size_outcome)
                return {
                    "status": "fail",
                    "parsed": parsed,
                    "timed_out": False,
                    "returncode": 1,
                    "reason": "server_non_zero_exit_1_size_65536",
                    "warnings": [],
                    "size_failures": {
                        65536: {
                            "reason": "server_non_zero_exit_1",
                            "timed_out": False,
                            "returncode": 1,
                        }
                    },
                }

            RC.run_sizes_test = fake_run_sizes_test
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                final_stats, failures = RC.collect_data(
                    "ignored",
                    "current",
                    "DEALER_ROUTER_REQREP",
                    1,
                    transports=["tcp"],
                    table_lines=[],
                )

            self.assertEqual(
                failures,
                [
                    (
                        "DEALER_ROUTER_REQREP",
                        "current",
                        "tcp",
                        65536,
                        "server_non_zero_exit_1",
                    )
                ],
            )
            self.assertEqual(final_stats["tcp|65536|throughput"], 0)
            self.assertEqual(final_stats["tcp|131072|throughput"], 10.0)
            output = stdout.getvalue()
            self.assertIn("| 65536B", output)
            self.assertIn("| 131072B", output)
            self.assertLess(
                output.index("| 65536B"),
                output.index("Testing tcp | 131072B:"),
            )
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
