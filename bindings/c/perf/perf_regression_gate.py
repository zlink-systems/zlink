#!/usr/bin/env python3
"""Fail a C perf comparison when any reported cell regresses by more than 5%."""

from __future__ import annotations

import argparse
import importlib.util
import math
import pathlib
import sys
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
RUNNER_PATH = SCRIPT_DIR / "run_comparison.py"
CellKey = Tuple[str, str, int, str]

HIGHER_IS_BETTER = {"throughput", "bandwidth"}
LATENCY_PREFIX = "latency"


def _load_report_parser():
    """Load the established C multi report parser used by the runners."""
    spec = importlib.util.spec_from_file_location("c_perf_report_parser", RUNNER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load C perf report parser: {RUNNER_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


REPORT_PARSER = _load_report_parser()


@dataclass
class ParsedReport:
    path: pathlib.Path
    cells: Dict[CellKey, float] = field(default_factory=dict)
    duplicate_cells: List[CellKey] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)
    skip_count: int = 0
    fail_count: int = 0
    completion_status: str = ""


@dataclass
class CellVerdict:
    suite: str
    key: CellKey
    baseline: float | None
    candidate: float | None
    ratio: float | None
    rule: str
    status: str


def parse_report(path: pathlib.Path) -> ParsedReport:
    """Read a runner report, delegating RESULT validation to its existing parser."""
    result = ParsedReport(path=path)
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        result.errors.append(f"could not read report: {exc}")
        return result

    for line_number, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if line.startswith("RESULT,"):
            parts = line.split(",")
            if len(parts) != 7:
                result.errors.append(f"line {line_number}: malformed RESULT line")
                continue
            try:
                transport = parts[3].strip().lower()
                msg_size = int(parts[4].strip())
            except ValueError:
                result.errors.append(f"line {line_number}: malformed RESULT numeric field")
                continue

            parsed, warning = REPORT_PARSER.parse_result_line(
                line, transport, {msg_size}
            )
            if warning:
                result.errors.append(f"line {line_number}: {warning}")
                continue
            if parsed is None:
                result.errors.append(f"line {line_number}: RESULT line was rejected")
                continue

            _parsed_transport, _parsed_size, metric, value = parsed
            pattern = parts[2].strip().upper()
            if not pattern:
                result.errors.append(f"line {line_number}: empty RESULT pattern")
                continue
            key = (pattern, transport, msg_size, metric)
            if key in result.cells:
                result.duplicate_cells.append(key)
            else:
                result.cells[key] = value
            continue

        if line.startswith("- skip:"):
            result.skip_count = _parse_count(line, "skip", result, line_number)
        elif line.startswith("- fail:"):
            result.fail_count = _parse_count(line, "fail", result, line_number)
        elif line.startswith("- status:"):
            result.completion_status = line.partition(":")[2].strip().lower()
        elif line in ("## Skips", "## Failures"):
            # Single reports do not have numeric skip/fail counters; their
            # sections still make the report non-comparable.
            result.errors.append(f"line {line_number}: {line[3:].lower()} section present")

    if not result.cells:
        result.errors.append("no RESULT cells found")
    if result.skip_count:
        result.errors.append(f"unexpected skip count: {result.skip_count}")
    if result.fail_count:
        result.errors.append(f"unexpected fail count: {result.fail_count}")
    if result.completion_status and result.completion_status != "complete":
        result.errors.append(f"report completion status is {result.completion_status}")
    return result


def _parse_count(line: str, name: str, report: ParsedReport, line_number: int) -> int:
    value = line.partition(":")[2].strip()
    try:
        parsed = int(value)
    except ValueError:
        report.errors.append(f"line {line_number}: malformed {name} count")
        return 0
    if parsed < 0:
        report.errors.append(f"line {line_number}: negative {name} count")
        return 0
    return parsed


def metric_rule(metric: str) -> str:
    if metric in HIGHER_IS_BETTER:
        return "candidate/baseline >= 0.95"
    if metric.startswith(LATENCY_PREFIX):
        return "candidate/baseline <= 1.05"
    return "unsupported metric"


def compare_reports(
    suite: str, baseline: ParsedReport, candidate: ParsedReport
) -> List[CellVerdict]:
    verdicts: List[CellVerdict] = []
    for key in sorted(set(baseline.cells) | set(candidate.cells)):
        baseline_value = baseline.cells.get(key)
        candidate_value = candidate.cells.get(key)
        rule = metric_rule(key[3])
        ratio = None

        if baseline_value is None:
            status = "FAIL missing-baseline"
        elif candidate_value is None:
            status = "FAIL missing-candidate"
        elif not math.isfinite(baseline_value) or not math.isfinite(candidate_value):
            status = "FAIL non-finite"
        elif baseline_value == 0:
            status = "FAIL baseline-zero"
        elif rule == "unsupported metric":
            status = "FAIL unsupported-metric"
        else:
            ratio = candidate_value / baseline_value
            passes = (
                ratio >= 0.95
                if key[3] in HIGHER_IS_BETTER
                else ratio <= 1.05
            )
            status = "PASS" if passes else "FAIL regression"

        verdicts.append(
            CellVerdict(suite, key, baseline_value, candidate_value, ratio, rule, status)
        )
    return verdicts


def report_errors(label: str, report: ParsedReport) -> Iterable[str]:
    for key in report.duplicate_cells:
        yield f"{label}: duplicate cell {format_key(key)}"
    for error in report.errors:
        yield f"{label}: {error} ({report.path})"


def format_key(key: CellKey) -> str:
    pattern, transport, msg_size, metric = key
    return f"{pattern}/{transport}/{msg_size}/{metric}"


def format_value(value: float | None) -> str:
    return "-" if value is None else f"{value:.6g}"


def print_verdicts(verdicts: Sequence[CellVerdict]) -> None:
    print("| Suite | Pattern | Transport | Size | Metric | Baseline | Candidate | Ratio | Rule | Status |")
    print("|---|---|---|---:|---|---:|---:|---:|---|---|")
    for verdict in verdicts:
        pattern, transport, msg_size, metric = verdict.key
        ratio = "-" if verdict.ratio is None else f"{verdict.ratio:.4f}"
        print(
            f"| {verdict.suite} | {pattern} | {transport} | {msg_size} | {metric} | "
            f"{format_value(verdict.baseline)} | {format_value(verdict.candidate)} | "
            f"{ratio} | {verdict.rule} | {verdict.status} |"
        )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare C single/multi perf reports cell-by-cell with a 5%% gate."
    )
    parser.add_argument("--baseline-single", type=pathlib.Path)
    parser.add_argument("--candidate-single", type=pathlib.Path)
    parser.add_argument("--baseline-multi", type=pathlib.Path)
    parser.add_argument("--candidate-multi", type=pathlib.Path)
    args = parser.parse_args(argv)
    pairs = ((args.baseline_single, args.candidate_single), (args.baseline_multi, args.candidate_multi))
    if not any(left is not None for pair in pairs for left in pair):
        parser.error("provide at least one baseline/candidate report pair")
    for suite, (baseline, candidate) in zip(("single", "multi"), pairs):
        if (baseline is None) != (candidate is None):
            parser.error(f"{suite} requires both --baseline-{suite} and --candidate-{suite}")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    all_verdicts: List[CellVerdict] = []
    errors: List[str] = []
    for suite, baseline_path, candidate_path in (
        ("single", args.baseline_single, args.candidate_single),
        ("multi", args.baseline_multi, args.candidate_multi),
    ):
        if baseline_path is None:
            continue
        baseline = parse_report(baseline_path)
        candidate = parse_report(candidate_path)
        errors.extend(report_errors(f"{suite} baseline", baseline))
        errors.extend(report_errors(f"{suite} candidate", candidate))
        all_verdicts.extend(compare_reports(suite, baseline, candidate))

    if all_verdicts:
        print_verdicts(all_verdicts)
    if errors:
        print("\nReport errors:")
        for error in errors:
            print(f"- {error}")

    failed_cells = sum(not verdict.status.startswith("PASS") for verdict in all_verdicts)
    failed = bool(errors or failed_cells)
    print(f"\nFinal: {'FAIL' if failed else 'PASS'} (cells={len(all_verdicts)}, failed_cells={failed_cells}, report_errors={len(errors)})")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
