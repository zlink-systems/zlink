#!/usr/bin/env python3
"""Gate 64 KiB WS-family latency and server RSS against matching TCP cells."""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import pathlib
import sys
from typing import Dict, List, Mapping, Sequence, Tuple


SCRIPT_DIR = pathlib.Path (__file__).resolve ().parent
WS_ROUNDTRIP_GATE_PATH = SCRIPT_DIR / "ws_roundtrip_gate.py"

REQUIRED_PATTERN = "MULTI_DEALER_ROUTER_SENDSEND"
OPTIONAL_PATTERN = "MULTI_ROUTER_ROUTER_SENDSEND"
PATTERNS = (REQUIRED_PATTERN, OPTIONAL_PATTERN)
TRANSPORTS = ("tcp", "ws", "wss", "tls")
SIZE = 65536
LATENCY_METRIC = "latency"
RATIO_MAX = 3.0


def _load_ws_roundtrip_gate ():
    spec = importlib.util.spec_from_file_location (
      "c_perf_ws_roundtrip_gate_for_latency_rss", WS_ROUNDTRIP_GATE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError (f"could not load WS report validation: {WS_ROUNDTRIP_GATE_PATH}")
    module = importlib.util.module_from_spec (spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module (module)
    return module


WS_GATE = _load_ws_roundtrip_gate ()


def _latency_key (pattern: str, transport: str):
    return (pattern, transport, SIZE, LATENCY_METRIC)


def _is_positive_finite (value) -> bool:
    return (isinstance (value, (int, float)) and not isinstance (value, bool)
            and math.isfinite (value) and value > 0)


def load_rss_cells (
    paths: Sequence[pathlib.Path],
) -> Tuple[Dict[Tuple[str, str, int], float], List[str]]:
    """Read sampler sidecars without altering benchmark reports.

    Each sidecar is ``{"cells": [{"pattern": ..., "transport": ...,
    "size": 65536, "server_peak_rss_kib": ...}]}``.  A cell may occur once
    across all sidecars so a mixed run cannot silently select an arbitrary RSS.
    """
    cells: Dict[Tuple[str, str, int], float] = {}
    sources: Dict[Tuple[str, str, int], pathlib.Path] = {}
    errors: List[str] = []
    for path in paths:
        try:
            decoded = json.loads (path.read_text (encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            errors.append (f"{path}: cannot read RSS sidecar: {exc}")
            continue
        entries = decoded.get ("cells") if isinstance (decoded, dict) else None
        if not isinstance (entries, list):
            errors.append (f"{path}: RSS sidecar requires a top-level cells array")
            continue
        for index, entry in enumerate (entries):
            prefix = f"{path}: cells[{index}]"
            if not isinstance (entry, dict):
                errors.append (f"{prefix}: must be an object")
                continue
            pattern = entry.get ("pattern")
            transport = entry.get ("transport")
            size = entry.get ("size")
            rss = entry.get ("server_peak_rss_kib")
            if not isinstance (pattern, str) or not pattern:
                errors.append (f"{prefix}: pattern must be a nonempty string")
                continue
            if not isinstance (transport, str) or not transport:
                errors.append (f"{prefix}: transport must be a nonempty string")
                continue
            if not isinstance (size, int) or isinstance (size, bool):
                errors.append (f"{prefix}: size must be an integer")
                continue
            if not _is_positive_finite (rss):
                errors.append (f"{prefix}: server_peak_rss_kib must be finite and positive")
                continue
            key = (pattern, transport, size)
            if key in cells:
                errors.append (
                  f"duplicate RSS cell {pattern}/{transport}/{size} across "
                  f"{sources[key]} and {path}")
                continue
            cells[key] = float (rss)
            sources[key] = path
    return cells, errors


def _pattern_is_present (cells: Mapping[WS_GATE.REGRESSION_GATE.CellKey, float],
                         pattern: str) -> bool:
    return any (key[0] == pattern for key in cells)


def compute_ratios (
    report_cells: Mapping[WS_GATE.REGRESSION_GATE.CellKey, float],
    rss_cells: Mapping[Tuple[str, str, int], float],
) -> Tuple[Dict[Tuple[str, str], Tuple[float, float]], List[str]]:
    """Return (latency_ratio, RSS_ratio) by (pattern, comparison transport)."""
    errors: List[str] = []
    ratios: Dict[Tuple[str, str], Tuple[float, float]] = {}
    for pattern in PATTERNS:
        present = _pattern_is_present (report_cells, pattern)
        if pattern == OPTIONAL_PATTERN and not present:
            continue
        for transport in TRANSPORTS:
            latency_key = _latency_key (pattern, transport)
            latency = report_cells.get (latency_key)
            if latency is None:
                errors.append (
                  f"missing required latency cell: "
                  f"{WS_GATE.REGRESSION_GATE.format_key (latency_key)}")
            elif not _is_positive_finite (latency):
                errors.append (
                  f"latency cell must be finite and positive: "
                  f"{WS_GATE.REGRESSION_GATE.format_key (latency_key)}={latency!r}")
            rss_key = (pattern, transport, SIZE)
            rss = rss_cells.get (rss_key)
            if rss is None:
                errors.append (f"missing required RSS cell: {pattern}/{transport}/{SIZE}")
            elif not _is_positive_finite (rss):
                errors.append (
                  f"RSS cell must be finite and positive: {pattern}/{transport}/{SIZE}={rss!r}")
        if errors:
            continue
        tcp_latency = report_cells[_latency_key (pattern, "tcp")]
        tcp_rss = rss_cells[(pattern, "tcp", SIZE)]
        for transport in TRANSPORTS[1:]:
            latency_ratio = report_cells[_latency_key (pattern, transport)] / tcp_latency
            rss_ratio = rss_cells[(pattern, transport, SIZE)] / tcp_rss
            if not math.isfinite (latency_ratio) or not math.isfinite (rss_ratio):
                errors.append (f"{pattern}/{transport}: nonfinite comparison ratio")
                continue
            ratios[(pattern, transport)] = (latency_ratio, rss_ratio)
    return ratios, errors


def parse_args (argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser (
      description=(
        "Gate 64 KiB WS/WSS/TLS mean latency and server peak RSS against "
        "matching complete C multi TCP report cells."))
    parser.add_argument ("reports", nargs="+", type=pathlib.Path, metavar="REPORT")
    parser.add_argument (
      "--rss-sidecar", required=True, action="append", type=pathlib.Path,
      metavar="JSON", help="JSON with cells[{pattern,transport,size,server_peak_rss_kib}]")
    return parser.parse_args (argv)


def main (argv: Sequence[str] | None = None) -> int:
    args = parse_args (argv)
    report_cells, errors = WS_GATE.load_cells (args.reports)
    rss_cells, rss_errors = load_rss_cells (args.rss_sidecar)
    errors.extend (rss_errors)
    ratios, ratio_errors = compute_ratios (report_cells, rss_cells)
    errors.extend (ratio_errors)

    if errors:
        print ("WS/WSS/TLS latency/RSS gate: FAIL")
        for error in errors:
            print (f"- {error}")
        print (f"Final: FAIL (reports={len (args.reports)}, errors={len (errors)})")
        return 1

    print ("WS/WSS/TLS 64 KiB gate (mean latency ratio <= 3.00; server peak RSS ratio <= 3.00)")
    print ("| Pattern | Transport | Latency/TCP | RSS/TCP |")
    print ("|---|---|---:|---:|")
    for pattern, transport in sorted (ratios):
        latency_ratio, rss_ratio = ratios[(pattern, transport)]
        print (f"| {pattern} | {transport} | {latency_ratio:.6f} | {rss_ratio:.6f} |")
    status = "PASS" if all (
      latency_ratio <= RATIO_MAX and rss_ratio <= RATIO_MAX
      for latency_ratio, rss_ratio in ratios.values ()) else "FAIL"
    print (f"Final: {status} (reports={len (args.reports)}, errors=0)")
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit (main ())
