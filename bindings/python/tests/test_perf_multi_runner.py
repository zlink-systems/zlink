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
from perf_multi_common import apply_multi_auto_hwm_msg_unit
from run_benchmarks import _normalize_pattern, _result_pattern


class PerfMultiRunnerTests(unittest.TestCase):
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

    def test_auto_hwm_message_unit_recalculates_after_assignment(self):
        calls = []

        class Options:
            auto_hwm_msg_unit_bytes = 0

        class Context:
            options = Options()

            def recalculate_auto_hwm(self):
                calls.append(self.options.auto_hwm_msg_unit_bytes)

        context = Context()
        apply_multi_auto_hwm_msg_unit(context, 4096)

        self.assertEqual(context.options.auto_hwm_msg_unit_bytes, 4096)
        self.assertEqual(calls, [4096])

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
