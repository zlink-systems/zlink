import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest


MODULE_PATH = pathlib.Path (__file__).resolve ().parents[1] / "ws_roundtrip_gate.py"
SPEC = importlib.util.spec_from_file_location ("c_ws_roundtrip_gate", MODULE_PATH)
GATE = importlib.util.module_from_spec (SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module (GATE)


CELLS = {
    ("MULTI_DEALER_DEALER", "tcp", 1024): 1000.0,
    ("MULTI_DEALER_DEALER", "ws", 1024): 900.0,
    ("MULTI_DEALER_DEALER", "wss", 1024): 850.0,
    ("MULTI_DEALER_ROUTER_SENDSEND", "tcp", 1024): 500.0,
    ("MULTI_DEALER_ROUTER_SENDSEND", "ws", 1024): 405.0,
    ("MULTI_DEALER_ROUTER_SENDSEND", "wss", 1024): 382.5,
    ("MULTI_DEALER_DEALER", "tcp", 65536): 100.0,
    ("MULTI_DEALER_DEALER", "ws", 65536): 70.0,
    ("MULTI_DEALER_DEALER", "wss", 65536): 65.0,
    ("MULTI_DEALER_ROUTER_SENDSEND", "tcp", 65536): 50.0,
    ("MULTI_DEALER_ROUTER_SENDSEND", "ws", 65536): 35.0,
    ("MULTI_DEALER_ROUTER_SENDSEND", "wss", 65536): 32.5,
}


def measured_report (cells=None, *, metadata=None, duplicate=None):
    values = dict (CELLS if cells is None else cells)
    lines = [
        "META,os,Linux test-host",
        "META,cpu,test-cpu",
        "META,cores,8",
        "META,build,Release",
        "META,core_revision,abc123",
        "META,timestamp,2026-09-08T12:00:00+09:00",
        "META,load_avg,0.10 0.20 0.30",
        "META,runs,1",
        "META,clients,100",
    ]
    for (pattern, transport, size), throughput in values.items ():
        lines.extend (
          [
              f"RESULT,current,{pattern},{transport},{size},bandwidth,{throughput * size / 1000:.3f}",
              f"RESULT,current,{pattern},{transport},{size},latency,1.000",
              f"RESULT,current,{pattern},{transport},{size},latency_p95,2.000",
              f"RESULT,current,{pattern},{transport},{size},latency_p99,3.000",
              f"RESULT,current,{pattern},{transport},{size},throughput,{throughput:.3f}",
          ])
    if duplicate is not None:
        pattern, transport, size = duplicate
        lines.append (
          f"RESULT,current,{pattern},{transport},{size},throughput,1000.000")
    result_count = sum (line.startswith ("RESULT,") for line in lines)
    completion = {
        "success": len (values),
        "unsupported": 0,
        "skip": 0,
        "fail": 0,
        "status": "complete",
        "expected_result_lines": result_count,
        "actual_result_lines": result_count,
    }
    completion.update (metadata or {})
    lines.extend (
      [
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


class WsRoundtripGateTests (unittest.TestCase):
    def run_gate (self, reports):
        contents = reports if isinstance (reports, str) else reports
        if isinstance (contents, str):
            contents = [contents]
        with tempfile.TemporaryDirectory () as directory:
            root = pathlib.Path (directory)
            paths = []
            for index, content in enumerate (contents):
                path = root / f"report-{index}.txt"
                path.write_text (content, encoding="utf-8")
                paths.append (path)
            output = io.StringIO ()
            with contextlib.redirect_stdout (output):
                exit_code = GATE.main ([str (path) for path in paths])
        return exit_code, output.getvalue ()

    def test_measured_report_shape_passes_and_prints_each_transport_q (self):
        exit_code, output = self.run_gate (measured_report ())
        self.assertEqual (exit_code, 0)
        self.assertIn ("| 1024 | 0.900000 | 0.900000 |", output)
        self.assertIn ("| 65536 | 1.000000 | 1.000000 |", output)
        self.assertIn ("ws Q64/Q1 = 1.111111", output)
        self.assertIn ("wss Q64/Q1 = 1.111111", output)
        self.assertIn ("Final: PASS", output)

    def test_host_scale_cancels_from_q (self):
        scaled = {key: value * 37.5 for key, value in CELLS.items ()}
        exit_code, output = self.run_gate (measured_report (scaled))
        self.assertEqual (exit_code, 0)
        self.assertIn ("ws Q64/Q1 = 1.111111", output)
        self.assertIn ("wss Q64/Q1 = 1.111111", output)

    def test_rejects_large_ws_roundtrip_collapse (self):
        collapsed = dict (CELLS)
        collapsed[("MULTI_DEALER_ROUTER_SENDSEND", "ws", 65536)] = 20.0
        exit_code, output = self.run_gate (measured_report (collapsed))
        self.assertEqual (exit_code, 1)
        self.assertIn ("ws Q64/Q1 = 0.634921", output)
        self.assertIn ("wss Q64/Q1 = 1.111111", output)
        self.assertIn ("Final: FAIL", output)

    def test_accepts_multiple_nonoverlapping_reports (self):
        first = dict (list (CELLS.items ())[:4])
        second = dict (list (CELLS.items ())[4:])
        exit_code, output = self.run_gate (
          [measured_report (first), measured_report (second)])
        self.assertEqual (exit_code, 0)
        self.assertIn ("Final: PASS", output)

    def test_rejects_multiple_reports_from_different_load_or_run (self):
        first = dict (list (CELLS.items ())[:4])
        second = dict (list (CELLS.items ())[4:])
        mismatched = measured_report (second).replace (
          "META,load_avg,0.10 0.20 0.30", "META,load_avg,4.00 3.00 2.00")
        exit_code, output = self.run_gate (
          [measured_report (first), mismatched])
        self.assertEqual (exit_code, 1)
        self.assertIn ("comparison provenance load_avg", output)

    def test_rejects_missing_required_cell (self):
        incomplete = dict (CELLS)
        del incomplete[("MULTI_DEALER_ROUTER_SENDSEND", "ws", 65536)]
        exit_code, output = self.run_gate (measured_report (incomplete))
        self.assertEqual (exit_code, 1)
        self.assertIn ("missing required cell", output)

    def test_rejects_missing_required_wss_cell (self):
        incomplete = dict (CELLS)
        del incomplete[("MULTI_DEALER_ROUTER_SENDSEND", "wss", 65536)]
        exit_code, output = self.run_gate (measured_report (incomplete))
        self.assertEqual (exit_code, 1)
        self.assertIn ("wss/65536/throughput", output)

    def test_rejects_large_wss_roundtrip_collapse (self):
        collapsed = dict (CELLS)
        collapsed[("MULTI_DEALER_ROUTER_SENDSEND", "wss", 65536)] = 20.0
        exit_code, output = self.run_gate (measured_report (collapsed))
        self.assertEqual (exit_code, 1)
        self.assertIn ("ws Q64/Q1 = 1.111111", output)
        self.assertIn ("wss Q64/Q1 = 0.683761", output)
        self.assertIn ("Final: FAIL", output)

    def test_rejects_duplicate_cell (self):
        duplicate = ("MULTI_DEALER_DEALER", "tcp", 1024)
        exit_code, output = self.run_gate (
          measured_report (duplicate=duplicate))
        self.assertEqual (exit_code, 1)
        self.assertIn ("duplicate cell", output)

    def test_rejects_invalid_nonpositive_or_nonfinite_value (self):
        for bad_value in ("0", "-1", "nan", "inf"):
            with self.subTest (bad_value=bad_value):
                text = measured_report ().replace (
                  "RESULT,current,MULTI_DEALER_DEALER,tcp,1024,throughput,1000.000",
                  f"RESULT,current,MULTI_DEALER_DEALER,tcp,1024,throughput,{bad_value}")
                exit_code, output = self.run_gate (text)
                self.assertEqual (exit_code, 1)
                self.assertIn ("finite and positive", output)

    def test_rejects_nonzero_error_fail_skip_or_unsupported (self):
        for field in ("error", "fail", "skip", "unsupported"):
            with self.subTest (field=field):
                metadata = {field: 1}
                text = measured_report (metadata=metadata)
                if field == "error":
                    text = text.replace (
                      "- actual_result_lines:", "- error: 1\n- actual_result_lines:")
                exit_code, output = self.run_gate (text)
                self.assertEqual (exit_code, 1)
                self.assertIn (f"nonzero {field}", output)

    def test_rejects_incomplete_completion_counts (self):
        text = measured_report (metadata={"actual_result_lines": 1})
        exit_code, output = self.run_gate (text)
        self.assertEqual (exit_code, 1)
        self.assertIn ("expected_result_lines", output)


if __name__ == "__main__":
    unittest.main ()
