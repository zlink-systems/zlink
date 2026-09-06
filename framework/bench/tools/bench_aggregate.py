#!/usr/bin/env python3
"""Aggregate with-grpc bench runs into one report.

The language harnesses measure. This tool owns everything derived from what they
measured: unit normalization, medians, G5 reproducibility, the spec 7.2 ratios,
and the decision about whether a ratio may be published at all.

    bench_aggregate.py --lang dotnet \\
        --run zlink-work/fwb-02/gated2/c-router-1 \\
        --run zlink-work/fwb-02/gated2/dotnet-router-1 ...

Runs given in one invocation form one comparison. Each run contributes only the
implementations it measured, so the C reference runs and the language runs are
passed together and the ratio ``zlink-<lang> / zlink-c`` spans both.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from benchagg.analysis import (  # noqa: E402
    build_rows,
    judge_language,
    language_verdict,
    ordered_keys,
)
from benchagg.model import PAYLOAD_SIZES  # noqa: E402
from benchagg.readers import ReportError, read_runs  # noqa: E402
from benchagg.render import (  # noqa: E402
    render_contaminated,
    render_diagnostics_table,
    render_g5_note,
    render_g5_table,
    render_judgement_table,
    render_median_table,
    render_result_lines,
    render_spec4_table,
)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", action="append", default=[], help="a run directory")
    parser.add_argument("--runs-glob", action="append", default=[], help="glob of run directories")
    parser.add_argument("--lang", default="dotnet", help="language under judgement")
    parser.add_argument("--baseline", default="zlink-c", help="denominator of formula 1")
    parser.add_argument(
        "--payload-sizes",
        default=",".join(str(size) for size in PAYLOAD_SIZES),
        help="comma separated payload sizes",
    )
    parser.add_argument(
        "--min-runs",
        type=int,
        default=3,
        help="runs a row needs before G5 can pass it (plan 6 fixes 3)",
    )
    parser.add_argument("--json-out", help="write the normalized cells and verdicts here")
    parser.add_argument(
        "--format",
        choices=("full", "spec4", "result", "judgement"),
        default="full",
        help="which sections to print",
    )
    return parser.parse_args(argv)


def collect_run_dirs(args) -> list[str]:
    dirs: list[str] = []
    for pattern in args.runs_glob:
        dirs.extend(sorted(path for path in glob.glob(pattern) if os.path.isdir(path)))
    for path in args.run:
        if not os.path.isdir(path):
            raise ReportError(f"{path}: not a run directory")
        dirs.append(path)
    seen, unique = set(), []
    for path in dirs:
        key = os.path.abspath(path)
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def build(args):
    run_dirs = collect_run_dirs(args)
    if not run_dirs:
        raise ReportError("no run directories given")
    run_set = read_runs(run_dirs)
    rows = build_rows(run_set, min_runs_for_g5=args.min_runs)
    sizes = tuple(int(part) for part in args.payload_sizes.split(","))
    judgements = judge_language(rows, args.lang, args.baseline, sizes)
    return run_set, rows, sizes, judgements


def emit(args, run_set, rows, sizes, judgements) -> str:
    out: list[str] = []
    if args.format in ("full", "spec4"):
        out.append("## Report table (spec 4)\n")
        out.append(render_spec4_table(rows, sizes))
    if args.format in ("full", "result"):
        out.append("## RESULT lines (spec 4; throughput in completions per second)\n")
        out.append(render_result_lines(rows, sizes))
        out.append("")
    if args.format == "full":
        out.append("\n## Medians across runs\n")
        out.append(render_median_table(rows, sizes))
        out.append("\n## Diagnostics (FB-008, FB-017, G6, G8)\n")
        out.append(render_diagnostics_table(rows, sizes))
        out.append("\n## G5 reproducibility\n")
        out.append(render_g5_note())
        out.append("")
        out.append(render_g5_table(rows, sizes))
        out.append("\n## Contaminated cells (FB-008, excluded from tables and judgement)\n")
        out.append(render_contaminated(run_set.contaminated()))
    if args.format in ("full", "judgement"):
        out.append("\n## Judgement (spec 7.2, FB-005, FB-011)\n")
        out.append(render_judgement_table(judgements))
        status, reason = language_verdict(judgements)
        out.append(f"\n**{args.lang}: {status}** — {reason}")
    if args.format == "full" and run_set.notes:
        out.append("\n## Notes\n")
        out.extend(f"- {note}" for note in run_set.notes)
    return "\n".join(out)


def as_json(args, run_set, rows, sizes, judgements) -> dict:
    return {
        "schema": "with-grpc-aggregate-v1",
        "language": args.lang,
        "baseline": args.baseline,
        "runs": run_set.runs,
        "notes": run_set.notes,
        "rows": [
            {
                "implementation": key.implementation,
                "pattern": key.pattern,
                "payload_size": key.payload_size,
                "runs": rows[key].run_count,
                "median": rows[key].values,
                "g5_spread_percent": rows[key].spread_percent,
                "g5_status": rows[key].g5_status,
                "peak_in_flight": rows[key].peak_in_flight,
                "peak_in_flight_min": rows[key].peak_in_flight_min,
                "request_window": rows[key].request_window,
                "abandoned": rows[key].abandoned,
                "in_flight_depth": rows[key].in_flight_depth,
                "client_saturated": rows[key].client_saturated,
                "drain_bound_hit": rows[key].drain_bound_hit,
                "send_throughput_server_counted": rows[key].send_server_counted,
                "excluded_runs": rows[key].excluded_runs,
            }
            for key in ordered_keys(rows, sizes)
        ],
        "contaminated": [
            {"run": cell.run, "cell": str(cell.key), "reason": cell.contamination_reason}
            for cell in run_set.contaminated()
        ],
        "judgements": [
            {
                "formula": judgement.formula,
                "payload_size": judgement.payload_size,
                "value": judgement.value,
                "status": judgement.status,
                "verdict": judgement.verdict,
                "reason": judgement.reason,
            }
            for judgement in judgements
        ],
        "language_verdict": dict(zip(("status", "reason"), language_verdict(judgements))),
    }


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        run_set, rows, sizes, judgements = build(args)
    except ReportError as error:
        print(f"bench_aggregate: {error}", file=sys.stderr)
        return 2
    print(emit(args, run_set, rows, sizes, judgements))
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as handle:
            json.dump(as_json(args, run_set, rows, sizes, judgements), handle, indent=2)
            handle.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
