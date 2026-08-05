#!/usr/bin/env python3
"""Compatibility runner for dotnet perf.

Accepts the legacy run_comparison.py invocation shape and dispatches to the
local single or multi shell runner inside bindings/dotnet/perf.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Iterable, List, Set


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_RESULTS_DIR = SCRIPT_DIR / "results"


def _split_patterns(raw: str) -> List[str]:
    return [p.strip().upper() for p in raw.split(",") if p.strip()]


def _detect_suite(patterns: Iterable[str]) -> str:
    pats = list(patterns)
    if not pats:
        return "single"
    is_multi = [p.startswith("MULTI_") for p in pats]
    if all(is_multi):
        return "multi"
    if any(is_multi):
        raise ValueError("cannot mix single and MULTI_* patterns")
    return "single"


def _snapshot_files(path: Path) -> Set[str]:
    if not path.exists():
        return set()
    return {p.name for p in path.iterdir() if p.is_file()}


def _pick_latest_new(path: Path, before: Set[str]) -> Path | None:
    if not path.exists():
        return None
    candidates = [p for p in path.iterdir() if p.is_file() and p.name not in before]
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="dotnet perf compatibility runner")
    ap.add_argument("pattern")
    ap.add_argument("--build-dir", default="")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--pin-cpu", action="store_true")
    ap.add_argument("--duration", type=int, default=0)
    ap.add_argument("--results-dir", default="")
    ap.add_argument("--results-tag", default="")
    ap.add_argument("--result-file", default="")
    args, unknown = ap.parse_known_args(argv)

    patterns = _split_patterns(args.pattern)
    if not patterns:
        print("Error: pattern is required", file=sys.stderr)
        return 1

    try:
        suite = _detect_suite(patterns)
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    results_dir = Path(args.results_dir) if args.results_dir else DEFAULT_RESULTS_DIR
    report_dir = results_dir / suite / "report"
    tmp_dir = results_dir / suite / "tmp"
    before_report = _snapshot_files(report_dir)
    before_tmp = _snapshot_files(tmp_dir)

    runner = SCRIPT_DIR / ("single/run_benchmarks.sh" if suite == "single" else "multi/run_benchmarks.sh")
    cmd: List[str] = [
        str(runner),
        "--pattern",
        ",".join(patterns),
        "--results-dir",
        str(results_dir),
    ]

    if args.results_tag:
        cmd.extend(["--results-tag", args.results_tag])
    if args.duration > 0:
        cmd.extend(["--duration", str(args.duration)])

    cmd.extend(unknown)
    rc = subprocess.call(cmd)

    if args.result_file:
        out_path = Path(args.result_file)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        chosen = _pick_latest_new(report_dir, before_report)
        if chosen is None:
            chosen = _pick_latest_new(tmp_dir, before_tmp)
        if chosen is not None and chosen.exists():
            shutil.copy2(chosen, out_path)
        elif not out_path.exists():
            out_path.write_text("", encoding="utf-8")

    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
