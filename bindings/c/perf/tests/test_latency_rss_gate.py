import contextlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest


MODULE_PATH = pathlib.Path (__file__).resolve ().parents[1] / "latency_rss_gate.py"
SPEC = importlib.util.spec_from_file_location ("c_latency_rss_gate", MODULE_PATH)
GATE = importlib.util.module_from_spec (SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module (GATE)


PATTERNS = ("MULTI_DEALER_ROUTER_SENDSEND", "MULTI_ROUTER_ROUTER_SENDSEND")
TRANSPORTS = ("tcp", "ws", "wss", "tls")


def measured_report (cells=None, *, metadata=None, duplicate=False):
    values = cells or {
      (pattern, transport): 1.0 if transport == "tcp" else 2.0
      for pattern in PATTERNS for transport in TRANSPORTS
    }
    lines = []
    for (pattern, transport), latency in values.items ():
        lines.extend ([
          f"RESULT,current,{pattern},{transport},65536,bandwidth,1.0",
          f"RESULT,current,{pattern},{transport},65536,latency,{latency}",
          f"RESULT,current,{pattern},{transport},65536,latency_p95,{latency}",
          f"RESULT,current,{pattern},{transport},65536,latency_p99,{latency}",
          f"RESULT,current,{pattern},{transport},65536,throughput,1.0",
        ])
    if duplicate:
        lines.append (
          "RESULT,current,MULTI_DEALER_ROUTER_SENDSEND,tcp,65536,latency,1.0")
    result_lines = len (lines)
    completion = {
      "success": len (values), "unsupported": 0, "skip": 0, "fail": 0,
      "status": "complete", "expected_result_lines": result_lines,
      "actual_result_lines": result_lines,
    }
    completion.update (metadata or {})
    lines.extend ([
      "## Completion",
      f"- success: {completion['success']}",
      f"- unsupported: {completion['unsupported']}",
      f"- skip: {completion['skip']}",
      f"- fail: {completion['fail']}",
      f"- status: {completion['status']}",
      f"- expected_result_lines: {completion['expected_result_lines']}",
      f"- actual_result_lines: {completion['actual_result_lines']}",
    ])
    return "\n".join (lines) + "\n"


def rss_sidecar (cells=None):
    values = cells or {
      (pattern, transport): 100.0 if transport == "tcp" else 200.0
      for pattern in PATTERNS for transport in TRANSPORTS
    }
    return {"cells": [
      {"pattern": pattern, "transport": transport, "size": 65536,
       "server_peak_rss_kib": rss}
      for (pattern, transport), rss in values.items ()
    ]}


class LatencyRssGateTests (unittest.TestCase):
    def run_gate (self, report, sidecar):
        with tempfile.TemporaryDirectory () as directory:
            root = pathlib.Path (directory)
            report_path = root / "report.txt"
            sidecar_path = root / "rss.json"
            report_path.write_text (report, encoding="utf-8")
            sidecar_path.write_text (json.dumps (sidecar), encoding="utf-8")
            output = io.StringIO ()
            with contextlib.redirect_stdout (output):
                code = GATE.main ([str (report_path), "--rss-sidecar", str (sidecar_path)])
        return code, output.getvalue ()

    def test_complete_sendsend_and_reqrep_pass (self):
        code, output = self.run_gate (measured_report (), rss_sidecar ())
        self.assertEqual (code, 0)
        self.assertIn ("MULTI_DEALER_ROUTER_SENDSEND | tls | 2.000000 | 2.000000", output)
        self.assertIn ("MULTI_ROUTER_ROUTER_SENDSEND | ws | 2.000000 | 2.000000", output)
        self.assertIn ("Final: PASS", output)

    def test_reqrep_is_optional_only_when_entire_pattern_is_absent (self):
        pattern = "MULTI_DEALER_ROUTER_SENDSEND"
        report = measured_report ({(pattern, transport): 1.0 for transport in TRANSPORTS})
        sidecar = rss_sidecar ({(pattern, transport): 100.0 for transport in TRANSPORTS})
        code, output = self.run_gate (report, sidecar)
        self.assertEqual (code, 0)
        self.assertNotIn ("MULTI_ROUTER_ROUTER_SENDSEND", output)

    def test_rejects_latency_or_rss_ratio_over_limit (self):
        latency_cells = {(pattern, transport): 1.0 for pattern in PATTERNS for transport in TRANSPORTS}
        latency_cells[("MULTI_DEALER_ROUTER_SENDSEND", "wss")] = 3.01
        code, output = self.run_gate (measured_report (latency_cells), rss_sidecar ())
        self.assertEqual (code, 1)
        self.assertIn ("wss | 3.010000 | 2.000000", output)

        rss_cells = {(pattern, transport): 100.0 for pattern in PATTERNS for transport in TRANSPORTS}
        rss_cells[("MULTI_DEALER_ROUTER_SENDSEND", "tls")] = 301.0
        code, output = self.run_gate (measured_report (), rss_sidecar (rss_cells))
        self.assertEqual (code, 1)
        self.assertIn ("tls | 2.000000 | 3.010000", output)

    def test_rejects_missing_or_duplicate_rss_cells (self):
        sidecar = rss_sidecar ()
        sidecar["cells"].pop ()
        code, output = self.run_gate (measured_report (), sidecar)
        self.assertEqual (code, 1)
        self.assertIn ("missing required RSS cell", output)

        sidecar = rss_sidecar ()
        sidecar["cells"].append (dict (sidecar["cells"][0]))
        code, output = self.run_gate (measured_report (), sidecar)
        self.assertEqual (code, 1)
        self.assertIn ("duplicate RSS cell", output)

    def test_rejects_partial_optional_pattern_and_invalid_report (self):
        values = {("MULTI_DEALER_ROUTER_SENDSEND", transport): 1.0 for transport in TRANSPORTS}
        values[("MULTI_ROUTER_ROUTER_SENDSEND", "tcp")] = 1.0
        code, output = self.run_gate (measured_report (values), rss_sidecar ())
        self.assertEqual (code, 1)
        self.assertIn ("missing required latency cell", output)

        code, output = self.run_gate (measured_report (duplicate=True), rss_sidecar ())
        self.assertEqual (code, 1)
        self.assertIn ("duplicate cell", output)

    def test_rejects_incomplete_or_invalid_sidecar(self):
        code, output = self.run_gate (
          measured_report (metadata={"status": "partial"}), {"cells": []})
        self.assertEqual (code, 1)
        self.assertIn ("completion status is partial", output)

        code, output = self.run_gate (measured_report (), {"cells": "bad"})
        self.assertEqual (code, 1)
        self.assertIn ("requires a top-level cells array", output)


if __name__ == "__main__":
    unittest.main ()
