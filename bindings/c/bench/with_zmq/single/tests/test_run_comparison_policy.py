import importlib.util
import pathlib
import sys
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "run_comparison.py"
SPEC = importlib.util.spec_from_file_location("with_zmq_single_run_comparison", MODULE_PATH)
RC = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = RC
SPEC.loader.exec_module(RC)


class RunComparisonPolicyTests(unittest.TestCase):
    def test_stream_pattern_removed(self):
        with self.assertRaises(ValueError):
            RC.parse_pattern_arg("STREAM")

    def test_service_patterns_removed(self):
        with self.assertRaises(ValueError):
            RC.parse_pattern_arg("GATEWAY")
        with self.assertRaises(ValueError):
            RC.parse_pattern_arg("SPOT")

    def test_default_transports_remove_ws_family(self):
        self.assertIn("tcp", RC.DEFAULT_TRANSPORTS)
        self.assertIn("inproc", RC.DEFAULT_TRANSPORTS)
        self.assertNotIn("tls", RC.DEFAULT_TRANSPORTS)
        self.assertNotIn("ws", RC.DEFAULT_TRANSPORTS)
        self.assertNotIn("wss", RC.DEFAULT_TRANSPORTS)

    def test_validate_transports_rejects_ws_family(self):
        with self.assertRaises(ValueError):
            RC.validate_transports(["tcp", "tls"])
        with self.assertRaises(ValueError):
            RC.validate_transports(["ws"])
        with self.assertRaises(ValueError):
            RC.validate_transports(["wss"])

    def test_results_dir_policy(self):
        root = "/tmp/bench-results"
        self.assertEqual(RC.single_result_dir(root), "/tmp/bench-results/single/report")

    def test_comparison_table_format_kept_without_resource_columns(self):
        std = {
            "tcp|64|throughput": 1000.0,
            "tcp|64|latency": 10.0,
        }
        zlk = {
            "tcp|64|throughput": 1200.0,
            "tcp|64|latency": 8.0,
        }
        lines = RC.build_pattern_report_lines(std, zlk, ["tcp"], [64])
        joined = "\n".join(lines)

        self.assertTrue(any("Standard libzmq" in line for line in lines))
        self.assertTrue(any("Diff (%)" in line for line in lines))
        self.assertIn("Throughput", joined)
        self.assertIn("Latency", joined)

        lowered = joined.lower()
        self.assertNotIn("cpu", lowered)
        self.assertNotIn("mem", lowered)
        self.assertNotIn("queue", lowered)


if __name__ == "__main__":
    unittest.main()
