from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ReadmeAlignmentTests(unittest.TestCase):
    def test_python_readme_exists_and_references_binding_policy(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("bindings/README.md", readme)
        self.assertIn("core/include/zlink.h", readme)
        self.assertIn("multipart-only", readme)
        self.assertIn("capability matrix", readme)
        self.assertIn("SendFlags", readme)
        self.assertIn("RecvFlags", readme)
        self.assertIn("ContextOptions", readme)
        self.assertIn("ref_count", readme)
        self.assertIn("StreamPacket", readme)
        self.assertIn("ReplyToken", readme)
        self.assertIn("CompletionKind", readme)
        self.assertIn("WRITABLE", readme)
        self.assertIn("POLLCOMPLETION", readme)
        self.assertIn("pull-only", readme)
        self.assertIn("receive_subscription_event", readme)
        self.assertIn(
            "monitor_open(events=..., monitor_hwm_bytes=...)", readme
        )
        self.assertIn("MonitorEventMask", readme)
        self.assertIn("asynchronous context manager cleanup", readme)
        self.assertIn("Timer", readme)
        self.assertIn("Stopwatch", readme)
        self.assertIn("AtomicCounter", readme)
        self.assertNotIn("attach_discovery", readme)
        self.assertNotIn("MemberPeerEntry", readme)
        self.assertNotIn("RegistryQueryClient", readme)
        self.assertIn("XPubSocket", readme)
        self.assertIn("tests/run_tests.sh", readme)

        pending_start = readme.index("ZLINK_OPT_PENDING_MAX_MSGS")
        pending_contract = readme[pending_start : pending_start + 320]
        self.assertIn("ZLINK_OPT_PENDING_MAX_BYTES", pending_contract)
        self.assertIn("ABI", pending_contract)
        self.assertIn("ignored", pending_contract)
        self.assertNotIn("REQUEST-only", pending_contract)

    def test_test_runner_script_exists_in_tests_directory(self):
        script = ROOT / "tests" / "run_tests.sh"
        self.assertTrue(script.exists())

    def test_perf_readme_references_perf_policy_and_actual_transport_matrix(self):
        readme = (ROOT / "perf" / "README.md").read_text(encoding="utf-8")
        self.assertIn("bindings/README.md", readme)
        self.assertIn("doc/perf/PERF_POLICY.md", readme)
        self.assertIn("doc/perf/PERF_SINGLE_TEST_POLICY.md", readme)
        self.assertIn("doc/perf/PERF_MULTI_TEST_POLICY.md", readme)
        self.assertIn("`PAIR`: `tcp`", readme)
        self.assertIn("`PUBSUB`: `tcp`", readme)
        self.assertIn("MULTI_STREAM", readme)
        self.assertIn("RESULT,current", readme)
        self.assertIn("## Effective Options (start)", readme)
        self.assertIn("--msg-sizes", readme)
        self.assertIn("perf_<lang>_<suite>_<platform>", readme)

    def test_multi_perf_readme_documents_result_names_and_recv_only_surface(self):
        readme = (ROOT / "perf" / "multi" / "README.md").read_text(
            encoding="utf-8"
        )
        self.assertIn("MULTI_PUBSUB", readme)
        self.assertIn("MULTI_DEALER_ROUTER", readme)
        self.assertIn("MULTI_ROUTER_ROUTER", readme)
        self.assertIn("recv path only", readme)
        self.assertIn("STREAM", readme)
        self.assertIn("results/multi/report/perf_<lang>_<suite>_<platform>", readme)
