import asyncio
import os
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
PERF_DIR = ROOT / "perf"
sys.path.insert(0, str(PERF_DIR))
sys.path.insert(0, str(PERF_DIR / "multi"))

from perf_metrics import (
    active_message_latency_ns,
    LatencySampler,
    new_payload,
    result_metrics,
    stamp_payload,
)
from perf_multi_common import (
    PERF_MULTI_AUX_POLL_WAIT_MS,
    PYTHON_MULTI_DEFAULT_IO_THREADS,
    RELAY_SCHEDULER_QUANTUM,
    RelaySchedulerQuantum,
    STOP_TOKEN,
    benchmark_endpoint,
    make_relay_send_done_callback,
    received_has_stop_token,
    scoped_relay_eager_task_factory,
    send_routed,
    track_relay_send_task,
)
from perf_multi_dealer_dealer_server import (
    dealer_dealer_active_poll_timeout_ms,
)
from perf_multi_reqrep_client import submit_managed_request
from perf_multi_reqrep_server import submit_reqrep_reply
from perf_multi_stream_server import classify_control_line, wait_connection_ready_count
from run_benchmarks import (
    CANONICAL_STREAM_CLIENT_BIN,
    DEFAULT_PATTERNS,
    POLICY_TRANSPORTS,
    STREAM_CLIENT_BIN,
    _build_options,
    _clients_for_pattern,
    _ensure_stream_client,
    _normalize_pattern,
    _options_clients_display,
    _parse_patterns,
    _release_stream_start,
    _require_binding_runtime,
    _result_pattern,
    _run_pattern_captured,
    _validate_python_build_options,
    parse_args,
    pattern_direction,
)

import zlink


class PerfMultiRunnerTests(unittest.TestCase):
    def test_runner_does_not_treat_partial_results_from_failed_case_as_success(self):
        partial = (
            "RESULT,current,MULTI_PUBSUB,tcp,1024,throughput,1.000\n"
            "server shutdown timed out"
        )
        with mock.patch("run_benchmarks._run_pattern", side_effect=SystemExit(partial)):
            output, failed = _run_pattern_captured(
                object(), {}, "PUBSUB", "tcp", "1024", "8"
            )
        self.assertTrue(failed)
        self.assertEqual(output, partial)

    def test_multi_clients_default_to_one_hundred_for_every_pattern(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(_clients_for_pattern("DEALER_DEALER", None), "100")
            self.assertEqual(_clients_for_pattern("STREAM", None), "100")
            clients = _options_clients_display(list(DEFAULT_PATTERNS), None)
            self.assertEqual(clients, "100 (stream=100)")
            options = _build_options(
                parse_args([]),
                list(DEFAULT_PATTERNS),
                list(POLICY_TRANSPORTS),
                [],
                clients,
                {"PERF_MULTI_MONITOR_HWM": "100000"},
            )
            self.assertEqual(options["default_clients"], "100")
            self.assertEqual(options["default_stream_clients"], "100")

    def test_python_runner_rejects_build_options_it_does_not_own(self):
        for argv, option in (
            (["--build-dir", "/tmp/python-perf"], "--build-dir"),
            (["--clean-build"], "--clean-build"),
        ):
            with self.subTest(option=option):
                with self.assertRaisesRegex(SystemExit, option):
                    _validate_python_build_options(parse_args(argv))

    def test_python_runner_accepts_reuse_as_validation_only(self):
        args = parse_args(["--reuse-build"])
        _validate_python_build_options(args)
        self.assertTrue(args.reuse_build)

    def test_python_runner_requires_source_tree_inplace_binding(self):
        _require_binding_runtime()

    def test_python_runner_reuse_never_builds_missing_stream_client(self):
        with mock.patch("run_benchmarks._is_executable", return_value=False):
            with mock.patch("run_benchmarks.subprocess.run") as build:
                with self.assertRaisesRegex(SystemExit, "--reuse-build"):
                    _ensure_stream_client(reuse_build=True)
        build.assert_not_called()

    def test_python_runner_prefers_canonical_stream_client(self):
        executable = {CANONICAL_STREAM_CLIENT_BIN, STREAM_CLIENT_BIN}
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch(
                "run_benchmarks._is_executable",
                side_effect=lambda path: path in executable,
            ):
                with mock.patch("run_benchmarks._stream_client_is_fresh", return_value=True):
                    selected = _ensure_stream_client()
        self.assertEqual(selected, CANONICAL_STREAM_CLIENT_BIN)

    def test_python_runner_rebuilds_stale_canonical_stream_client(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch("run_benchmarks._stream_client_is_fresh", return_value=False):
                with mock.patch(
                    "run_benchmarks._rebuild_canonical_stream_client",
                    return_value=CANONICAL_STREAM_CLIENT_BIN,
                ) as rebuild_canonical:
                    with mock.patch(
                        "run_benchmarks._rebuild_fallback_stream_client"
                    ) as rebuild_fallback:
                        selected = _ensure_stream_client()
        self.assertEqual(selected, CANONICAL_STREAM_CLIENT_BIN)
        rebuild_canonical.assert_called_once_with()
        rebuild_fallback.assert_not_called()

    def test_python_runner_reuse_accepts_canonical_without_freshness_rebuild(self):
        executable = {CANONICAL_STREAM_CLIENT_BIN, STREAM_CLIENT_BIN}
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch(
                "run_benchmarks._is_executable",
                side_effect=lambda path: path in executable,
            ):
                with mock.patch("run_benchmarks._stream_client_is_fresh") as freshness:
                    selected = _ensure_stream_client(reuse_build=True)
        self.assertEqual(selected, CANONICAL_STREAM_CLIENT_BIN)
        freshness.assert_not_called()

    def test_stream_start_barrier_waits_for_server_ack_before_client_start(self):
        events = []

        class ControlInput:
            def __init__(self, owner):
                self.owner = owner

            def write(self, value):
                events.append(f"{self.owner}:write:{value.strip()}")

            def flush(self):
                events.append(f"{self.owner}:flush")

        server = mock.Mock(stdin=ControlInput("server"))
        client = mock.Mock(stdin=ControlInput("client"))

        def wait_for_line(proc, *_args, **_kwargs):
            if proc is client:
                events.append("wait:client-ready")
                return "CLIENT_READY,1024"
            events.append("wait:server-start-ready")
            return "SERVER_START_READY,1024"

        with mock.patch("run_benchmarks._wait_for_control_line", side_effect=wait_for_line):
            _release_stream_start(
                server,
                client,
                "1024",
                timeout_s=1,
                stdout_chunks=[],
            )

        self.assertEqual(
            events,
            [
                "wait:client-ready",
                "server:write:START,1024",
                "server:flush",
                "wait:server-start-ready",
                "client:write:START,1024",
                "client:flush",
            ],
        )

    def test_stream_server_control_requires_exact_start_size(self):
        self.assertEqual(classify_control_line("START,1024", 1024), "start")
        self.assertIsNone(classify_control_line("START,256", 1024))
        self.assertEqual(classify_control_line("STOP", 1024), "stop")

    def test_stream_server_waits_for_target_connection_count(self):
        monitor = object()
        with mock.patch(
            "perf_multi_stream_server.wait_monitor_event"
        ) as wait_event:
            wait_connection_ready_count(monitor, 3, 1000)
        self.assertEqual(wait_event.call_count, 3)

    def test_python_multi_defaults_to_one_io_thread(self):
        self.assertEqual(PYTHON_MULTI_DEFAULT_IO_THREADS, 1)

    def test_routed_sendsend_clients_recalculate_hwm_before_active_phase(self):
        multi_dir = PERF_DIR / "multi"
        for filename in (
            "perf_multi_dealer_router_client.py",
            "perf_multi_router_router_client.py",
        ):
            with self.subTest(filename=filename):
                source = (multi_dir / filename).read_text(encoding="utf-8")
                ready_wait = source.index("for monitor in monitors:")
                recalculate = source.index("ctx.recalculate_auto_hwm()")
                active_phase = source.index("active_deadline =")
                self.assertLess(ready_wait, recalculate)
                self.assertLess(recalculate, active_phase)

    def test_reqrep_client_owns_completion_dispatch_until_drain(self):
        source = (
            PERF_DIR / "multi" / "perf_multi_reqrep_client.py"
        ).read_text(encoding="utf-8")
        registration = source.index("zlink.PollEventFlag.POLLCOMPLETION")
        active_progress = source.index(
            "completion_poller.wait(completion_events, 0)", registration
        )
        drain = source.index("drain_deadline =", active_progress)
        drain_progress = source.index(
            "completion_poller.wait(completion_events, 0)",
            active_progress + 1,
        )
        removal = source.index("completion_poller.remove_socket(sock)")
        socket_close = source.index("sock.close()", removal)

        self.assertLess(registration, active_progress)
        self.assertLess(active_progress, drain)
        self.assertLess(drain, drain_progress)
        self.assertLess(drain_progress, removal)
        self.assertLess(removal, socket_close)
        self.assertNotIn("await asyncio.wait(", source)

    def test_dealer_dealer_stop_token_has_raw_message_shape(self):
        class Part:
            def __init__(self, data):
                self.data = data

        class Received:
            parts = [Part(STOP_TOKEN)]

            def __iter__(self):
                return iter(self.parts)

        self.assertTrue(received_has_stop_token(Received()))

        Received.parts = [Part(STOP_TOKEN), Part(b"")]
        self.assertFalse(received_has_stop_token(Received()))

    def test_multi_aux_poll_wait_matches_canonical_interval(self):
        self.assertEqual(PERF_MULTI_AUX_POLL_WAIT_MS, 100)

    def test_relay_scheduler_quantum_is_a_yield_interval(self):
        self.assertEqual(RELAY_SCHEDULER_QUANTUM, 32)
        scheduler = RelaySchedulerQuantum()

        self.assertFalse(any(scheduler.received() for _ in range(31)))
        self.assertTrue(scheduler.received())
        self.assertFalse(scheduler.received())
        scheduler.reset()
        self.assertFalse(any(scheduler.received() for _ in range(31)))
        self.assertTrue(scheduler.received())

    def test_relay_eager_task_factory_restores_previous_factory(self):
        previous_factory = object()
        eager_factory = object()

        class Loop:
            def __init__(self):
                self.factory = previous_factory

            def get_task_factory(self):
                return self.factory

            def set_task_factory(self, factory):
                self.factory = factory

        loop = Loop()
        with mock.patch.object(
            asyncio, "eager_task_factory", eager_factory, create=True
        ):
            with self.assertRaisesRegex(RuntimeError, "restore"):
                with scoped_relay_eager_task_factory(loop) as eager_enabled:
                    self.assertTrue(eager_enabled)
                    self.assertIs(loop.factory, eager_factory)
                    raise RuntimeError("restore")

        self.assertIs(loop.factory, previous_factory)

    def test_relay_task_factory_falls_back_without_python_312_eager_api(self):
        previous_factory = object()

        class Loop:
            def __init__(self):
                self.factory = previous_factory

            def get_task_factory(self):
                return self.factory

            def set_task_factory(self, factory):
                self.factory = factory

        loop = Loop()
        with mock.patch.object(asyncio, "eager_task_factory", None, create=True):
            with scoped_relay_eager_task_factory(loop) as eager_enabled:
                self.assertFalse(eager_enabled)
                self.assertIs(loop.factory, previous_factory)

        self.assertIs(loop.factory, previous_factory)

    @unittest.skipUnless(
        hasattr(asyncio, "eager_task_factory"), "requires Python 3.12 eager tasks"
    )
    def test_relay_eager_tasks_classify_done_pending_and_error_paths(self):
        async def scenario():
            pending_tasks = set()
            send_errors = []
            on_send_done = make_relay_send_done_callback(
                pending_tasks, send_errors
            )

            async def complete_inline():
                return None

            gate = asyncio.Event()

            async def complete_later():
                await gate.wait()

            async def ignored_error():
                raise zlink.SubmitError(zlink.SubmitResult.NOT_FOUND, 2)

            expected_error = RuntimeError("relay failure")

            async def visible_error():
                raise expected_error

            with scoped_relay_eager_task_factory() as eager_enabled:
                self.assertTrue(eager_enabled)

                inline_task = asyncio.create_task(complete_inline())
                self.assertTrue(inline_task.done())
                self.assertFalse(
                    track_relay_send_task(
                        inline_task, pending_tasks, on_send_done
                    )
                )

                ignored_task = asyncio.create_task(ignored_error())
                self.assertTrue(ignored_task.done())
                self.assertFalse(
                    track_relay_send_task(
                        ignored_task, pending_tasks, on_send_done
                    )
                )

                error_task = asyncio.create_task(visible_error())
                self.assertTrue(error_task.done())
                self.assertFalse(
                    track_relay_send_task(
                        error_task, pending_tasks, on_send_done
                    )
                )

                pending_task = asyncio.create_task(complete_later())
                self.assertFalse(pending_task.done())
                self.assertTrue(
                    track_relay_send_task(
                        pending_task, pending_tasks, on_send_done
                    )
                )
                self.assertEqual(pending_tasks, {pending_task})
                gate.set()
                await pending_task
                await asyncio.sleep(0)

            self.assertFalse(pending_tasks)
            self.assertEqual(send_errors, [expected_error])

        asyncio.run(scenario())

    def test_dealer_dealer_poll_becomes_bounded_only_after_stop_token(self):
        active_deadline = 12.5

        self.assertEqual(
            dealer_dealer_active_poll_timeout_ms(
                False, active_deadline, now=12.0
            ),
            -1,
        )
        self.assertEqual(
            dealer_dealer_active_poll_timeout_ms(
                True, active_deadline, now=12.0
            ),
            500,
        )

    def test_reqrep_reply_drains_vanished_route_without_retry(self):
        class ReplyOperation:
            def __init__(self, owner):
                self.owner = owner

            def messages(self, *parts):
                self.owner.parts = parts
                return self

            def submit(self):
                self.owner.attempts += 1
                raise zlink.SubmitError(
                    zlink.SubmitResult.NOT_CONNECTED, 107
                )

        class Request:
            def __init__(self):
                self.attempts = 0
                self.parts = ()

            def reply(self):
                return ReplyOperation(self)

        request = Request()
        self.assertFalse(submit_reqrep_reply(request, (b"payload", b"")))
        self.assertEqual(request.attempts, 1)
        self.assertEqual(request.parts, (b"payload", b""))

    def test_reqrep_reply_does_not_invent_backpressure_retry(self):
        class ReplyOperation:
            def __init__(self, owner):
                self.owner = owner

            def messages(self, *parts):
                return self

            def submit(self):
                self.owner.attempts += 1
                raise zlink.SubmitError(
                    zlink.SubmitResult.BACKPRESSURED, 110
                )

        class Request:
            def __init__(self):
                self.attempts = 0

            def reply(self):
                return ReplyOperation(self)

        request = Request()
        with self.assertRaises(zlink.SubmitError):
            submit_reqrep_reply(request, (b"payload", b""))
        self.assertEqual(request.attempts, 1)

    def test_request_submit_uses_binding_managed_terminal_once(self):
        reply = object()

        class RequestOperation:
            def __init__(self, owner):
                self.owner = owner
                self.parts = ()

            def messages(self, *parts):
                self.parts = parts
                return self

            def timeout(self, timeout_s):
                self.timeout_s = timeout_s
                return self

            async def submit(self):
                self.owner.attempts.append((self.parts, self.timeout_s))
                return reply

        class Socket:
            def __init__(self):
                self.attempts = []

            def request(self):
                return RequestOperation(self)

        socket = Socket()
        logical_parts = (b"payload", b"")
        result = asyncio.run(
            submit_managed_request(socket, logical_parts, timeout_s=0.2)
        )
        self.assertIs(result, reply)
        self.assertEqual(
            socket.attempts,
            [((b"payload", b""), 0.2)],
        )

    def test_request_submit_does_not_add_external_backpressure_retry(self):
        class RequestOperation:
            def messages(self, *_parts):
                return self

            def timeout(self, _timeout_s):
                return self

            async def submit(self):
                raise zlink.SubmitError(zlink.SubmitResult.BACKPRESSURED, 11)

        class Socket:
            def request(self):
                return RequestOperation()

        with self.assertRaises(zlink.SubmitError):
            asyncio.run(
                submit_managed_request(
                    Socket(), (b"payload", b""), timeout_s=0.2
                )
            )

    def test_routed_send_propagates_backpressure_without_external_retry(self):
        class SendOperation:
            def __init__(self, owner):
                self.owner = owner
                self.parts = []

            def messages(self, *parts):
                self.parts.extend(parts)
                return self

            def message(self, part):
                self.parts.append(part)
                return self

            async def submit(self):
                self.owner.attempts.append(tuple(self.parts))
                if len(self.owner.attempts) == 1:
                    raise zlink.SubmitError(
                        zlink.SubmitResult.BACKPRESSURED, 11
                    )

        class Socket:
            def __init__(self):
                self.attempts = []

            def send(self):
                return SendOperation(self)

        socket = Socket()
        with self.assertRaises(zlink.SubmitError) as raised:
            asyncio.run(send_routed(socket, b"payload"))
        self.assertEqual(raised.exception.result, zlink.SubmitResult.BACKPRESSURED)
        self.assertEqual(socket.attempts, [(b"payload", b"")])

    def test_inline_routed_admission_yields_to_concurrent_progress(self):
        events = []

        class SendOperation:
            def messages(self, *parts):
                return self

            async def submit(self):
                events.append("submit")

        class Socket:
            def send(self):
                return SendOperation()

        async def scenario():
            async def send_once():
                await send_routed(Socket(), b"payload")
                events.append("send-done")

            async def receive_progress():
                events.append("receive-progress")

            await asyncio.gather(send_once(), receive_progress())

        asyncio.run(scenario())
        self.assertEqual(events, ["submit", "receive-progress", "send-done"])

    def test_one_shot_routed_reply_can_skip_duplicate_success_yield(self):
        events = []

        class SendOperation:
            def messages(self, *parts):
                return self

            async def submit(self):
                events.append("submit")

        class Socket:
            def send(self):
                return SendOperation()

        async def scenario():
            async def send_once():
                await send_routed(
                    Socket(), b"payload", _yield_after_submit=False
                )
                events.append("send-done")

            async def receive_progress():
                events.append("receive-progress")

            await asyncio.gather(send_once(), receive_progress())

        asyncio.run(scenario())
        self.assertEqual(events, ["submit", "send-done", "receive-progress"])

    def test_one_shot_routed_reply_propagates_backpressure_without_retry_yield(self):
        events = []

        class SendOperation:
            def __init__(self, owner):
                self.owner = owner

            def messages(self, *parts):
                return self

            async def submit(self):
                self.owner.attempts += 1
                events.append(f"submit-{self.owner.attempts}")
                if self.owner.attempts == 1:
                    raise zlink.SubmitError(
                        zlink.SubmitResult.BACKPRESSURED, 11
                    )

        class Socket:
            def __init__(self):
                self.attempts = 0

            def send(self):
                return SendOperation(self)

        socket = Socket()
        with self.assertRaises(zlink.SubmitError) as raised:
            asyncio.run(
                send_routed(socket, b"payload", _yield_after_submit=False)
            )
        self.assertEqual(raised.exception.result, zlink.SubmitResult.BACKPRESSURED)
        self.assertEqual(socket.attempts, 1)
        self.assertEqual(events, ["submit-1"])

    def test_active_message_latency_validates_and_decodes_once(self):
        payload = stamp_payload(new_payload(64), phase=1, run_id=7, seq=11)

        active, latency = active_message_latency_ns(
            payload,
            expected_msg_size=64,
            run_id=7,
        )

        self.assertTrue(active)
        self.assertIsNotNone(latency)
        self.assertEqual(
            active_message_latency_ns(payload, expected_msg_size=256, run_id=7),
            (False, None),
        )

    def test_canonical_routed_echo_names(self):
        self.assertEqual(
            _normalize_pattern("MULTI_DEALER_ROUTER_SENDSEND"), "DEALER_ROUTER"
        )
        self.assertEqual(
            _normalize_pattern("MULTI_ROUTER_ROUTER_SENDSEND"), "ROUTER_ROUTER"
        )
        self.assertEqual(
            _result_pattern("DEALER_ROUTER"), "MULTI_DEALER_ROUTER_SENDSEND"
        )
        self.assertEqual(
            _result_pattern("ROUTER_ROUTER"), "MULTI_ROUTER_ROUTER_SENDSEND"
        )

    def test_all_registers_the_same_seven_canonical_multi_patterns(self):
        self.assertEqual(
            DEFAULT_PATTERNS,
            (
                "DEALER_DEALER",
                "DEALER_ROUTER",
                "ROUTER_ROUTER",
                "DEALER_ROUTER_REQREP",
                "ROUTER_ROUTER_REQREP",
                "PUBSUB",
                "STREAM",
            ),
        )
        self.assertEqual(_parse_patterns("ALL"), list(DEFAULT_PATTERNS))
        self.assertEqual(
            _result_pattern("DEALER_ROUTER_REQREP"),
            "MULTI_DEALER_ROUTER_REQREP",
        )
        self.assertEqual(
            _result_pattern("ROUTER_ROUTER_REQREP"),
            "MULTI_ROUTER_ROUTER_REQREP",
        )
        self.assertEqual(
            pattern_direction("MULTI_DEALER_ROUTER_REQREP"), "request-reply"
        )
        self.assertIn("ipc", POLICY_TRANSPORTS["ROUTER_ROUTER_REQREP"])

    def test_router_router_supports_ipc_with_unique_bind_endpoint(self):
        self.assertIn("ipc", POLICY_TRANSPORTS["ROUTER_ROUTER"])
        first = benchmark_endpoint("ipc", "multi-router-router")
        second = benchmark_endpoint("ipc", "multi-router-router")
        self.assertRegex(
            first,
            r"^ipc:///tmp/zlink-python-perf-multi-router-router-\d+-\d+\.ipc$",
        )
        self.assertNotEqual(first, second)

    def test_latency_sampler_keeps_exact_mean_with_bounded_percentiles(self):
        sampler = LatencySampler(sample_cap=2)
        for value in (1_000_000, 2_000_000, 9_000_000):
            sampler.add(value)

        metrics = result_metrics(
            count=3,
            msg_size=64,
            elapsed_s=1.0,
            latency_sampler=sampler,
        )

        self.assertEqual(sampler.count, 3)
        self.assertEqual(len(sampler.samples), 2)
        self.assertAlmostEqual(metrics["latency"], 4.0)

    def test_latency_sampler_without_reservoir_uses_exact_mean_for_percentiles(self):
        sampler = LatencySampler(sample_cap=0)
        sampler.add(1_000_000)
        sampler.add(3_000_000)

        metrics = result_metrics(
            count=2,
            msg_size=64,
            elapsed_s=1.0,
            latency_sampler=sampler,
        )

        self.assertAlmostEqual(metrics["latency"], 2.0)
        self.assertAlmostEqual(metrics["latency_p95"], 2.0)
        self.assertAlmostEqual(metrics["latency_p99"], 2.0)

    def test_top_level_multi_wrapper_help(self):
        runner = (
            PERF_DIR / "multi" / "run_benchmarks.py"
            if sys.platform == "win32"
            else PERF_DIR / "run_benchmarks_multi.sh"
        )
        result = subprocess.run(
            [sys.executable, str(runner), "--help"]
            if sys.platform == "win32"
            else [str(runner), "--help"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0)
        self.assertRegex(result.stdout, r"usage: run_benchmarks_multi\.(sh|py)")
        self.assertIn("--clients", result.stdout)
        self.assertIn("--msg-sizes", result.stdout)

    def test_multi_runner_help(self):
        runner = (
            PERF_DIR / "multi" / "run_benchmarks.py"
            if sys.platform == "win32"
            else PERF_DIR / "multi" / "run_benchmarks.sh"
        )
        result = subprocess.run(
            [sys.executable, str(runner), "--help"]
            if sys.platform == "win32"
            else [str(runner), "--help"],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0)
        self.assertRegex(result.stdout, r"usage: run_benchmarks_multi\.(sh|py)")
        self.assertIn("--clients", result.stdout)
        self.assertIn("--results-dir", result.stdout)
