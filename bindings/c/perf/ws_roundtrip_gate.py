#!/usr/bin/env python3
"""Gate WS and WSS round-trip performance against matching TCP paths."""

from __future__ import annotations

import argparse
import importlib.util
import math
import pathlib
import sys
from typing import Dict, List, Mapping, Sequence, Tuple


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
REGRESSION_GATE_PATH = SCRIPT_DIR / "perf_regression_gate.py"

PATTERNS = ("MULTI_DEALER_DEALER", "MULTI_DEALER_ROUTER_SENDSEND")
COMPARISON_TRANSPORTS = ("ws", "wss")
TRANSPORTS = ("tcp",) + COMPARISON_TRANSPORTS
SIZES = (1024, 65536)
METRIC = "throughput"
Q64_OVER_Q1_MIN = 0.80
PROVENANCE_FIELDS = (
    "os",
    "cpu",
    "cores",
    "build",
    "core_revision",
    "timestamp",
    "load_avg",
    "runs",
    "clients",
)


def _load_regression_gate ():
    spec = importlib.util.spec_from_file_location (
      "c_perf_regression_gate_for_ws_roundtrip", REGRESSION_GATE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError (f"could not load parser: {REGRESSION_GATE_PATH}")
    module = importlib.util.module_from_spec (spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module (module)
    return module


REGRESSION_GATE = _load_regression_gate ()


def _report_provenance (
    path: pathlib.Path,
) -> Tuple[Dict[str, str], List[str]]:
    values: Dict[str, str] = {}
    errors: List[str] = []
    try:
        lines = path.read_text (encoding="utf-8").splitlines ()
    except OSError as exc:
        return values, [f"{path}: could not read provenance: {exc}"]

    for line_number, raw_line in enumerate (lines, start=1):
        if not raw_line.startswith ("META,"):
            continue
        parts = raw_line.split (",", 2)
        if len (parts) != 3:
            errors.append (f"{path}: line {line_number}: malformed META line")
            continue
        key, value = parts[1].strip (), parts[2].strip ()
        if key in values and values[key] != value:
            errors.append (
              f"{path}: conflicting META value for {key}: "
              f"{values[key]!r} != {value!r}")
        values[key] = value
    return values, errors


def _completion_errors (result) -> List[str]:
    path = result.path
    errors: List[str] = []
    if result.unsupported_count is None:
        errors.append (f"{path}: missing completion field: unsupported")
    elif result.unsupported_count != 0:
        errors.append (f"{path}: nonzero unsupported: {result.unsupported_count}")
    if result.skip_count != 0:
        errors.append (f"{path}: nonzero skip: {result.skip_count}")
    if result.fail_count != 0:
        errors.append (f"{path}: nonzero fail: {result.fail_count}")
    if result.expected_result_lines is None:
        errors.append (f"{path}: missing completion field: expected_result_lines")
    if result.actual_result_lines is None:
        errors.append (f"{path}: missing completion field: actual_result_lines")

    if result.completion_status != "complete":
        errors.append (
          f"{path}: report completion status is {result.completion_status or '<missing>'}")

    if (result.expected_result_lines is not None
        and result.actual_result_lines is not None):
        if result.expected_result_lines != result.actual_result_lines:
            errors.append (
              f"{path}: expected_result_lines {result.expected_result_lines} != "
              f"actual_result_lines {result.actual_result_lines}")
        if result.actual_result_lines != result.result_line_count:
            errors.append (
              f"{path}: actual_result_lines {result.actual_result_lines} != "
              f"RESULT lines {result.result_line_count}")

    if result.error_count is not None and result.error_count != 0:
        errors.append (f"{path}: nonzero error count: {result.error_count}")

    for key, value in result.cells.items ():
        if not math.isfinite (value) or value <= 0:
            errors.append (
              f"{path}: RESULT value must be finite and positive: "
              f"{REGRESSION_GATE.format_key (key)}={value!r}")
    return errors


def load_cells (
    paths: Sequence[pathlib.Path],
) -> Tuple[Dict[REGRESSION_GATE.CellKey, float], List[str]]:
    cells: Dict[REGRESSION_GATE.CellKey, float] = {}
    errors: List[str] = []
    sources: Dict[REGRESSION_GATE.CellKey, pathlib.Path] = {}
    reference_provenance: Dict[str, str] | None = None
    reference_path: pathlib.Path | None = None
    for path in paths:
        provenance, provenance_errors = _report_provenance (path)
        errors.extend (provenance_errors)
        if len (paths) > 1:
            for field in PROVENANCE_FIELDS:
                if not provenance.get (field):
                    errors.append (
                      f"{path}: missing comparison provenance META field: {field}")
            if reference_provenance is None:
                reference_provenance = provenance
                reference_path = path
            else:
                for field in PROVENANCE_FIELDS:
                    if (field in provenance and field in reference_provenance
                        and provenance[field] != reference_provenance[field]):
                        errors.append (
                          f"{path}: comparison provenance {field}={provenance[field]!r} "
                          f"does not match {reference_path}: "
                          f"{reference_provenance[field]!r}")
        report = REGRESSION_GATE.parse_report (path)
        errors.extend (REGRESSION_GATE.report_errors (str (path), report))
        errors.extend (_completion_errors (report))
        for key in report.duplicate_cells:
            errors.append (f"{path}: duplicate cell {REGRESSION_GATE.format_key (key)}")
        for key, value in report.cells.items ():
            if key in cells:
                errors.append (
                  f"duplicate cell {REGRESSION_GATE.format_key (key)} across "
                  f"{sources[key]} and {path}")
            else:
                cells[key] = value
                sources[key] = path
    return cells, errors


def _required_key (
    pattern: str, transport: str, size: int
) -> REGRESSION_GATE.CellKey:
    return (pattern, transport, size, METRIC)


def compute_q (
    cells: Mapping[REGRESSION_GATE.CellKey, float],
) -> Tuple[Dict[Tuple[str, int], float], List[str]]:
    errors: List[str] = []
    q_values: Dict[Tuple[str, int], float] = {}
    for size in SIZES:
        values: Dict[Tuple[str, str], float] = {}
        for pattern in PATTERNS:
            for transport in TRANSPORTS:
                key = _required_key (pattern, transport, size)
                value = cells.get (key)
                if value is None:
                    errors.append (
                      f"missing required cell: {REGRESSION_GATE.format_key (key)}")
                elif not math.isfinite (value) or value <= 0:
                    errors.append (
                      f"required cell must be finite and positive: "
                      f"{REGRESSION_GATE.format_key (key)}={value!r}")
                else:
                    values[(pattern, transport)] = value
        if len (values) != len (PATTERNS) * len (TRANSPORTS):
            continue
        for transport in COMPARISON_TRANSPORTS:
            one_way_ratio = (
              values[(PATTERNS[0], transport)]
              / values[(PATTERNS[0], "tcp")])
            roundtrip_ratio = (
              values[(PATTERNS[1], transport)]
              / values[(PATTERNS[1], "tcp")])
            q = roundtrip_ratio / one_way_ratio
            if not math.isfinite (q) or q <= 0:
                errors.append (
                  f"{transport} Q({size}) must be finite and positive: {q!r}")
            else:
                q_values[(transport, size)] = q
    for transport in COMPARISON_TRANSPORTS:
        if all ((transport, size) in q_values for size in SIZES):
            ratio = q_values[(transport, 65536)] / q_values[(transport, 1024)]
            if not math.isfinite (ratio) or ratio <= 0:
                errors.append (
                  f"{transport} Q64/Q1 must be finite and positive: {ratio!r}")
    return q_values, errors


def parse_args (argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser (
      description=(
        "Gate WS and WSS round-trip throughput against matching TCP paths in "
        "one or more complete C multi reports."))
    parser.add_argument ("reports", nargs="+", type=pathlib.Path, metavar="REPORT")
    return parser.parse_args (argv)


def main (argv: Sequence[str] | None = None) -> int:
    args = parse_args (argv)
    cells, errors = load_cells (args.reports)
    q_values, q_errors = compute_q (cells)
    errors.extend (q_errors)

    if errors:
        print ("WS/WSS round-trip gate: FAIL")
        for error in errors:
            print (f"- {error}")
        print (f"Final: FAIL (reports={len (args.reports)}, errors={len (errors)})")
        return 1

    q_ratios = {
      transport: q_values[(transport, 65536)] / q_values[(transport, 1024)]
      for transport in COMPARISON_TRANSPORTS
    }
    print ("WS/WSS round-trip gate (each Q64/Q1 >= 0.80)")
    print ("| Size | Q(ws) | Q(wss) |")
    print ("|---:|---:|---:|")
    for size in SIZES:
        print (f"| {size} | {q_values[('ws', size)]:.6f} | "
               f"{q_values[('wss', size)]:.6f} |")
    status = "PASS" if all (
      ratio >= Q64_OVER_Q1_MIN for ratio in q_ratios.values ()) else "FAIL"
    for transport in COMPARISON_TRANSPORTS:
        print (f"{transport} Q64/Q1 = {q_ratios[transport]:.6f} "
               f"(threshold >= {Q64_OVER_Q1_MIN:.2f})")
    print (f"Final: {status} (reports={len (args.reports)}, errors=0)")
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit (main ())
