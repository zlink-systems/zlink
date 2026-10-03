"""Contract checks for the shared server-driven runner steps."""

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


COMMON = Path(__file__).resolve().parents[2] / "runner_common.sh"


class RunnerCommonTest(unittest.TestCase):
    def test_node_cell_without_submitted_still_checks_target_count(self):
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory) / "results.json"
            cell = {"implementation": "grpc-node", "pattern": "request-serial",
                    "completed": 3, "errors": 0, "abandoned": 0,
                    "target_stats": {"received": 3, "errors": 0}}
            result.write_text(json.dumps({"cells": [cell]}), encoding="utf-8")
            command = ["bash", "-c", 'source "$1"; verify_request_counts "$2"',
                       "_", str(COMMON), str(result)]
            self.assertEqual(0, subprocess.run(command, capture_output=True).returncode)
            cell["target_stats"]["received"] = 2
            result.write_text(json.dumps({"cells": [cell]}), encoding="utf-8")
            self.assertNotEqual(0, subprocess.run(command, capture_output=True).returncode)

    def test_merge_preserves_active_boundary_and_accounts_for_rejections(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = root / "results.json"
            target = root / "target.json"
            result.write_text(json.dumps({"cells": [{
                "pattern": "send-saturation", "submitted": 13, "completed": 13,
                "errors": 0, "abandoned": 0, "server_received_at_close": 7,
                "source_drain_ms": 4,
            }]}), encoding="utf-8")
            target.write_text(json.dumps({"received": 11, "errors": 0, "rejected": 2}),
                              encoding="utf-8")
            subprocess.run(["bash", "-c", 'source "$1"; merge_target_stats "$2" "$3" 6 false; '
                            'verify_request_counts "$2"', "_", str(COMMON), str(result), str(target)],
                           check=True, capture_output=True, text=True)
            cell = json.loads(result.read_text(encoding="utf-8"))["cells"][0]
            self.assertEqual(7, cell["server_received_at_close"])
            self.assertEqual(11, cell["target_stats"]["received"])
            self.assertEqual(10, cell["drain_ms"])

    def test_java_send_boundary_is_independent_of_settled_target_count(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = root / "results.json"
            target = root / "target.json"
            result.write_text(json.dumps({"cells": [{
                "implementation": "zlink-java", "pattern": "send-saturation",
                "submitted": 11, "completed": 11, "errors": 0, "abandoned": 0,
                "server_received_at_close": 7,
            }]}), encoding="utf-8")
            target.write_text(json.dumps({"received": 11, "errors": 0}), encoding="utf-8")
            subprocess.run(["bash", "-c", 'source "$1"; merge_target_stats "$2" "$3" 1 false; '
                            'verify_request_counts "$2"', "_", str(COMMON), str(result), str(target)],
                           check=True, capture_output=True, text=True)
            cell = json.loads(result.read_text(encoding="utf-8"))["cells"][0]
            self.assertEqual(7, cell["server_received_at_close"])
            self.assertEqual(11, cell["target_stats"]["received"])

    def test_settle_bound_records_snapshot_and_continues(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "target.json"
            script = '''
source "$1"
curl() {
  if [[ "$*" == *source*/bench/stats* ]]; then
    printf '%s\\n' '{"completed":0,"currentInFlight":0}'
  else
    printf '%s\\n' '{"received":1,"errors":0}'
  fi
}
settle_and_capture source target "$2" received 0
[[ "$SETTLE_BOUND_HIT" == true && -s "$2" ]]
'''
            subprocess.run(["bash", "-c", script, "_", str(COMMON), str(target)],
                           check=True, capture_output=True, text=True)
            self.assertEqual(1, json.loads(target.read_text(encoding="utf-8"))["received"])

    def test_bound_excludes_only_hit_cell_and_measures_the_next_cell(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = root / "results.json"
            target = root / "target.json"
            next_result = root / "next-results.json"
            result.write_text(json.dumps({"cells": [{
                "implementation": "grpc-java", "pattern": "send-saturation",
                "submitted": 1, "completed": 1, "errors": 0, "abandoned": 0,
                "server_received_at_close": 1,
            }]}), encoding="utf-8")
            script = '''
source "$1"
DURATION_SECONDS=1
active_result_path="$2"
active_count=0
trigger_phase() {
  if [[ "$6" == active ]]; then
    active_count=$((active_count + 1))
    printf '%s\\n' '{"cells":[{"implementation":"grpc-java","pattern":"send-saturation","submitted":1,"completed":1,"errors":0,"abandoned":0,"server_received_at_close":1}]}' >"$active_result_path"
  fi
  return 0
}
wait_for_idle() { return 0; }
calls=0
settle_and_capture() {
  calls=$((calls + 1))
  SETTLE_MS=1
  if ((calls == 2)); then SETTLE_BOUND_HIT=true; else SETTLE_BOUND_HIT=false; fi
  if [[ "$3" != /dev/null ]]; then
    printf '%s\\n' '{"received":1,"errors":0}' >"$3"
  fi
}
bench_run_cell trigger source target run first grpc-java send-saturation 1024 1000 "$2" "$3"
active_result_path="$4"
bench_run_cell trigger source target run second grpc-java send-saturation 1024 1000 "$4" "$3"
[[ "$active_count" == 2 ]]
'''
            subprocess.run(["bash", "-c", script, "_", str(COMMON), str(result),
                            str(target), str(next_result)], check=True, capture_output=True, text=True)
            first = json.loads(result.read_text(encoding="utf-8"))["cells"][0]
            second = json.loads(next_result.read_text(encoding="utf-8"))["cells"][0]
            self.assertTrue(first["drain_bound_hit"])
            self.assertFalse(second["drain_bound_hit"])
            self.assertNotIn("contaminated", second)
            self.assertEqual(1, second["target_stats"]["received"])

    def test_warmup_bound_excludes_current_cell(self):
        with tempfile.TemporaryDirectory() as directory:
            result = Path(directory) / "results.json"
            script = '''
source "$1"
trigger_phase() { [[ "$6" == warmup ]]; }
wait_for_idle() { return 0; }
settle_and_capture() { SETTLE_BOUND_HIT=true; SETTLE_MS=1; }
bench_run_cell trigger source target run cell grpc-java request-serial 1024 1000 "$2" /dev/null
'''
            subprocess.run(["bash", "-c", script, "_", str(COMMON), str(result)],
                           check=True, capture_output=True, text=True)
            cell = json.loads(result.read_text(encoding="utf-8"))["cells"][0]
            self.assertTrue(cell["contaminated"])
            self.assertNotIn("drain_bound_hit", cell)


if __name__ == "__main__":
    unittest.main()
