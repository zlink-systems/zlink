import asyncio
import subprocess
import sys
import unittest
from pathlib import Path


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
from perf_multi_common import benchmark_endpoint, send_routed
from perf_multi_reqrep_client import request_with_admission_retry
from run_benchmarks import (
    DEFAULT_PATTERNS,
    POLICY_TRANSPORTS,
    _normalize_pattern,
    _parse_patterns,
    _result_pattern,
    pattern_direction,
)

import zlink


class PerfMultiRunnerTests(unittest.TestCase):
    def test_request_rebuilds_same_logical_multipart_after_backpressure(self):
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
                if len(self.owner.attempts) == 1:
                    raise zlink.SubmitError(
                        zlink.SubmitResult.BACKPRESSURED, 11
                    )
                return reply

        class Socket:
            def __init__(self):
                self.attempts = []

            def request(self):
                return RequestOperation(self)

        socket = Socket()
        admitted = []
        result = asyncio.run(
            request_with_admission_retry(
                socket,
                (b"payload", b""),
                timeout_s=0.2,
                on_admitted=lambda: admitted.append(True),
            )
        )
        self.assertIs(result, reply)
        self.assertEqual(admitted, [True])
        self.assertEqual(
            socket.attempts,
            [((b"payload", b""), 0.2), ((b"payload", b""), 0.2)],
        )

    def test_routed_send_rebuilds_after_immediate_admission_backpressure(self):
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
        self.assertTrue(asyncio.run(send_routed(socket, b"payload")))
        self.assertEqual(
            socket.attempts,
            [(b"payload", b""), (b"payload", b"")],
        )

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
