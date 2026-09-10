#!/usr/bin/env python3
"""Render a Markdown before/after comparison of with-grpc bench JSON results.

The two inputs are measurement directories.  A directory may itself be a run or
contain run directories.  The established ``benchagg`` reader owns the JSON
schema and A/B source-target merge; this command only compares its normalized
rows.  It deliberately reports evidence instead of making a release decision.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass
from typing import Iterable

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from benchagg.analysis import Row, build_rows  # noqa: E402
from benchagg.model import CellKey, RunSet  # noqa: E402
from benchagg.readers import ReportError, read_run  # noqa: E402


HIGHER_IS_BETTER = ("throughput_per_second", "bandwidth_mb_s")
LATENCY_METRICS = ("latency_mean_ms", "latency_p95_ms", "latency_p99_ms")
METRICS = HIGHER_IS_BETTER + LATENCY_METRICS
METRIC_LABELS = {
    "throughput_per_second": "Throughput",
    "bandwidth_mb_s": "Bandwidth",
    "latency_mean_ms": "Lat.Mean",
    "latency_p95_ms": "Lat.P95",
    "latency_p99_ms": "Lat.P99",
}


@dataclass
class InputIssue:
    side: str
    path: str
    detail: str


@dataclass
class Snapshot:
    label: str
    rows: dict[CellKey, Row]
    issues: list[InputIssue]
    run_dirs: list[str]


@dataclass
class MetricComparison:
    key: CellKey
    metric: str
    before: float | None
    after: float | None
    ratio: float | None
    change_percent: float | None
    rule: str
    evidence: str


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("before", help="baseline measurement directory")
    parser.add_argument("after", help="candidate measurement directory")
    return parser.parse_args(argv)


def _is_result_directory(path: str) -> bool:
    """Whether a directory has one of the result containers the reader accepts."""
    if os.path.isfile(os.path.join(path, "report.txt")):
        return True
    for name in os.listdir(path):
        if not name.endswith(".json"):
            continue
        if name == "cells.json":
            return True
        file_path = os.path.join(path, name)
        try:
            with open(file_path, encoding="utf-8") as handle:
                payload = json.load(handle)
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(payload, dict) and (
            payload.get("schema") == "with-grpc-cell-v1"
            or payload.get("role") in ("source", "target")
        ):
            return True
    return False


def discover_run_dirs(root: str) -> list[str]:
    """Find result directories without treating files as part of the schema."""
    if not os.path.isdir(root):
        raise ReportError(f"{root}: not a measurement directory")
    found: list[str] = []
    for current, dirs, _files in os.walk(root):
        dirs.sort()
        if _is_result_directory(current):
            found.append(current)
            dirs[:] = []
    return found


def load_snapshot(label: str, root: str) -> Snapshot:
    run_dirs = discover_run_dirs(root)
    issues: list[InputIssue] = []
    if not run_dirs:
        issues.append(InputIssue(label, root, "no supported JSON result document or report.txt found"))

    run_set = RunSet()
    for run_dir in run_dirs:
        try:
            cells, notes = read_run(run_dir, source=run_dir)
        except ReportError as error:
            issues.append(InputIssue(label, run_dir, str(error)))
            continue
        for cell in cells:
            run_set.add(cell)
        run_set.notes.extend(notes)
    # This command compares a pair of measurement sets.  It uses the existing
    # row median when a set has repeated runs, but G5 is not a release-comparison
    # criterion and must not turn a one-run comparison into a hidden failure.
    return Snapshot(label, build_rows(run_set, min_runs_for_g5=1), issues, run_dirs)


def _row_state(row: Row | None) -> str | None:
    """Return why a row cannot be used for a ratio, never silently filtering it."""
    if row is None:
        return "missing cell"
    reasons: list[str] = []
    if row.run_count == 0:
        reasons.append("no complete measurement")
    if row.incomplete_runs:
        detail = "; ".join(row.incomplete_reasons) or "unstated"
        reasons.append(f"incomplete run(s)={len(row.incomplete_runs)} ({detail})")
    if row.excluded_runs:
        reasons.append(f"contaminated run(s)={len(row.excluded_runs)}")
    if row.errors:
        reasons.append(f"errors={row.errors}")
    if row.abandoned:
        reasons.append(f"abandoned={row.abandoned}")
    if row.drain_bound_hit:
        reasons.append("drain bound hit")
    return "; ".join(reasons) if reasons else None


def _rule(metric: str) -> str:
    if metric in HIGHER_IS_BETTER:
        return "after/before >= 0.95 (5% tolerance)"
    return "after/before <= 1.05 (5% tolerance)"


def compare_metric(
    key: CellKey, metric: str, before_row: Row | None, after_row: Row | None
) -> MetricComparison:
    before = None if before_row is None else before_row.values.get(metric)
    after = None if after_row is None else after_row.values.get(metric)
    rule = _rule(metric)
    before_state = _row_state(before_row)
    after_state = _row_state(after_row)
    if before_state or after_state:
        states = []
        if before_state:
            states.append(f"before: {before_state}")
        if after_state:
            states.append(f"after: {after_state}")
        return MetricComparison(key, metric, before, after, None, None, rule, "not comparable — " + "; ".join(states))
    if before is None or after is None:
        missing = []
        if before is None:
            missing.append("before metric missing")
        if after is None:
            missing.append("after metric missing")
        return MetricComparison(key, metric, before, after, None, None, rule, "not comparable — " + "; ".join(missing))
    if not math.isfinite(before) or not math.isfinite(after):
        return MetricComparison(key, metric, before, after, None, None, rule, "not comparable — non-finite metric")
    if before <= 0:
        return MetricComparison(key, metric, before, after, None, None, rule, "not comparable — before value is zero or negative")

    ratio = after / before
    change_percent = (ratio - 1.0) * 100.0
    within_tolerance = ratio >= 0.95 if metric in HIGHER_IS_BETTER else ratio <= 1.05
    evidence = "within cell tolerance" if within_tolerance else "outside cell tolerance"
    return MetricComparison(key, metric, before, after, ratio, change_percent, rule, evidence)


def compare_snapshots(before: Snapshot, after: Snapshot) -> list[MetricComparison]:
    comparisons: list[MetricComparison] = []
    for key in sorted(set(before.rows) | set(after.rows)):
        for metric in METRICS:
            comparisons.append(compare_metric(key, metric, before.rows.get(key), after.rows.get(key)))
    return comparisons


def _metric_value(value: float | None, metric: str) -> str:
    if value is None:
        return "**missing**"
    if metric == "throughput_per_second":
        return f"{value / 1000.0:.3f} KOPS"
    if metric == "bandwidth_mb_s":
        return f"{value:.3f} MB/s"
    return f"{value:.3f} ms"


def _ratio(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.4f}x"


def _change(value: float | None) -> str:
    return "n/a" if value is None else f"{value:+.2f}%"


def _depth(row: Row | None) -> str:
    if row is None:
        return "**missing**"
    value = row.in_flight_depth
    return "n/a" if value is None else f"{value:.2f}"


def _diagnostics(row: Row | None) -> str:
    if row is None:
        return "**missing cell**"
    state = _row_state(row)
    if state:
        return f"**{state}**"
    return f"ok (runs={row.run_count})"


def render_summary_table(
    before: Snapshot, after: Snapshot, comparisons: Iterable[MetricComparison]
) -> str:
    indexed = {(item.key, item.metric): item for item in comparisons}
    keys = sorted(set(before.rows) | set(after.rows))
    lines = [
        "| Scenario | Payload | Throughput before → after | Ratio | Change | Mean latency before → after | Ratio | Change | P95 before → after | Ratio | Change | Depth before → after | Errors / abandoned / drain |",
        "|---|---:|---|---:|---:|---|---:|---:|---|---:|---:|---|---|",
    ]
    for key in keys:
        metrics = [indexed[(key, name)] for name in ("throughput_per_second", "latency_mean_ms", "latency_p95_ms")]
        values: list[str] = []
        for item in metrics:
            values.extend((
                f"{_metric_value(item.before, item.metric)} → {_metric_value(item.after, item.metric)}",
                _ratio(item.ratio),
                _change(item.change_percent),
            ))
        before_row, after_row = before.rows.get(key), after.rows.get(key)
        diagnostics = f"before: {_diagnostics(before_row)}<br>after: {_diagnostics(after_row)}"
        lines.append(
            f"| `{key.implementation}` / {key.pattern} | {key.payload_size} | "
            + " | ".join(values)
            + f" | {_depth(before_row)} → {_depth(after_row)} | {diagnostics} |"
        )
    return "\n".join(lines)


def render_cell_evidence(comparisons: Iterable[MetricComparison]) -> str:
    lines = [
        "| Scenario | Payload | Metric | Before | After | Ratio | Change | Rule | Evidence |",
        "|---|---:|---|---:|---:|---:|---:|---|---|",
    ]
    for item in comparisons:
        lines.append(
            f"| `{item.key.implementation}` / {item.key.pattern} | {item.key.payload_size} "
            f"| {METRIC_LABELS[item.metric]} | {_metric_value(item.before, item.metric)} "
            f"| {_metric_value(item.after, item.metric)} | {_ratio(item.ratio)} "
            f"| {_change(item.change_percent)} | {item.rule} | {item.evidence} |"
        )
    return "\n".join(lines)


def render_aggregate_evidence(comparisons: Iterable[MetricComparison]) -> str:
    grouped: dict[tuple[str, str, str], list[MetricComparison]] = {}
    for item in comparisons:
        group = (item.key.pattern, item.key.implementation, item.metric)
        grouped.setdefault(group, []).append(item)

    lines = [
        "| Pattern | Transport (`implementation` in JSON) | Metric | Size ratios | Geometric mean | Rule | Evidence |",
        "|---|---|---|---|---:|---|---|",
    ]
    for (pattern, implementation, metric), items in sorted(grouped.items()):
        items.sort(key=lambda item: item.key.payload_size)
        sizes = ", ".join(
            f"{item.key.payload_size}: {_ratio(item.ratio)}" for item in items
        )
        unavailable = [item for item in items if item.ratio is None or item.ratio <= 0]
        aggregate_rule = (
            "geomean(after/before) >= 1.0"
            if metric in HIGHER_IS_BETTER
            else "geomean(after/before) <= 1.0"
        )
        if unavailable:
            details = "; ".join(
                f"{item.key.payload_size}: {item.evidence}" for item in unavailable
            )
            geomean = "n/a"
            evidence = "not comparable — " + details
        else:
            ratios = [item.ratio for item in items]
            assert all(ratio is not None for ratio in ratios)
            geomean_value = math.exp(sum(math.log(float(ratio)) for ratio in ratios) / len(ratios))
            geomean = f"{geomean_value:.4f}x"
            meets_rule = geomean_value >= 1.0 if metric in HIGHER_IS_BETTER else geomean_value <= 1.0
            evidence = "meets geometric-mean rule" if meets_rule else "outside geometric-mean rule"
        lines.append(
            f"| {pattern} | `{implementation}` | {METRIC_LABELS[metric]} | {sizes} "
            f"| {geomean} | {aggregate_rule} | {evidence} |"
        )
    return "\n".join(lines)


def render_issues(issues: Iterable[InputIssue]) -> str:
    issues = list(issues)
    if not issues:
        return "No unreadable measurement directories."
    lines = ["| Side | Directory | State |", "|---|---|---|"]
    for issue in issues:
        lines.append(f"| {issue.side} | `{issue.path}` | **unreadable** — {issue.detail} |")
    return "\n".join(lines)


def render(before: Snapshot, after: Snapshot) -> str:
    comparisons = compare_snapshots(before, after)
    return "\n\n".join((
        "# with-grpc bench before/after comparison\n\n"
        "Ratios are `after / before`.  This report exposes the mechanical evidence; "
        "a human makes the final adoption decision.",
        "## Scenario × payload\n\n" + render_summary_table(before, after, comparisons),
        "## Cell evidence (5% measurement tolerance)\n\n" + render_cell_evidence(comparisons),
        "## Size geometric-mean evidence\n\n"
        "The result schema names `implementation`, not a separate transport field; "
        "that exact field is used as the transport grouping label below.  Every "
        "size present in either input must be comparable before its geometric mean is shown.\n\n"
        + render_aggregate_evidence(comparisons),
        "## Input state\n\n" + render_issues([*before.issues, *after.issues]),
    ))


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        before = load_snapshot("before", args.before)
        after = load_snapshot("after", args.after)
    except ReportError as error:
        print(f"compare-results: {error}", file=sys.stderr)
        return 2
    print(render(before, after))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
