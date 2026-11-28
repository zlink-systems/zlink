#!/usr/bin/env python3
"""Create Markdown tables and repetition lists from multi perf RESULT records."""

import argparse
from collections import defaultdict
from dataclasses import dataclass, field
import math
from pathlib import Path
import re
from statistics import median
import sys

PATTERNS = (
    "DEALER_DEALER", "DEALER_ROUTER_SENDSEND", "ROUTER_ROUTER_SENDSEND",
    "DEALER_ROUTER_REQREP", "ROUTER_ROUTER_REQREP", "PUBSUB",
)
ALIASES = {"DEALER_ROUTER": PATTERNS[1], "ROUTER_ROUTER": PATTERNS[2]}
METRICS = ("throughput", "bandwidth", "latency", "latency_p95", "latency_p99")
SCREEN = {"throughput": "처리량", "latency": "평균", "latency_p99": "p99"}
LANGUAGES = ("C", "Rust", "Go", "Python", "C++", ".NET")
LIMIT = 0.10


@dataclass(frozen=True)
class Cell:
    pattern: str
    transport: str
    size: int

    def order(self):
        return PATTERNS.index(self.pattern), self.transport, self.size


@dataclass
class Sample:
    values: dict = field(default_factory=dict)
    diagnostic: dict = field(default_factory=dict)

    def number(self, metric):
        return number(self.values.get(metric, ""))


def number(value):
    try:
        result = float(value)
    except ValueError:
        return None
    return result if math.isfinite(result) else None


def load_run(path):
    log = path / "run.log" if path.is_dir() else path
    for meta in (log.parent / "meta.txt", log.with_suffix(".meta.txt")):
        if meta.is_file() and "exit=0" not in meta.read_text().splitlines():
            raise ValueError(f"{meta}: successful run-one exit is required")
    samples = {}
    diagnostics = {}
    log_lines = log.read_text(errors="replace").splitlines()
    for line in log_lines:
        if line.startswith("RESULT,"):
            fields = line.split(",")
            if len(fields) != 7:
                print(f"{log}: ignoring malformed RESULT row {line}", file=sys.stderr)
                continue
            _, _, pattern, transport, size, metric, value = fields
            cell = Cell(ALIASES.get(pattern, pattern), transport.lower(), int(size))
            sample = samples.setdefault(cell, Sample())
            if metric in sample.values:
                print(f"{log}: repeated RESULT for {cell} {metric}; using last value",
                      file=sys.stderr)
            sample.values[metric] = value

    console = log.parent / "console.log"
    # Rust emits diagnostics to stdout before writing the report.
    for source in (log, console):
        if source == console and not source.is_file():
            continue
        source_diagnostics = {}
        lines = log_lines if source == log else source.read_text(errors="replace").splitlines()
        for line in lines:
            if not line.startswith("BENCH_DIAG,"):
                continue
            _, pattern, size, *items = line.split(",")
            if len(items) % 2:
                raise ValueError(f"{log}: malformed BENCH_DIAG row {line}")
            key = ALIASES.get(pattern, pattern), int(size)
            values = dict(zip(items[::2], items[1::2]))
            if key in source_diagnostics and source_diagnostics[key] != values:
                raise ValueError(f"{log}: ambiguous BENCH_DIAG for {key}")
            source_diagnostics[key] = values
        for key, values in source_diagnostics.items():
            diagnostics.setdefault(key, values)

    if not samples:
        raise ValueError(f"{log}: no RESULT rows")
    if diagnostics and len({cell.transport for cell in samples}) > 1:
        raise ValueError(f"{log}: BENCH_DIAG requires one transport per log")
    for cell, sample in samples.items():
        if set(METRICS).difference(sample.values):
            raise ValueError(f"{log}: missing required metrics for {cell}")
        for metric in METRICS:
            value = sample.values[metric]
            if number(value) is None and not (metric.startswith("latency")
                                              and value.startswith("invalid:")):
                raise ValueError(f"{log}: invalid {metric} for {cell}: {value}")
        sample.diagnostic = diagnostics.get((cell.pattern, cell.size), {})
    return samples


def load_baseline(directory):
    rounds = {}
    for path in sorted(directory.glob("cbase3-r*-*.run.log")):
        match = re.fullmatch(r"cbase3-r(\d+)-([A-Z_]+)\.run\.log", path.name)
        if not match:
            continue
        revision, pattern = int(match[1]), match[2]
        loaded = load_run(path)
        cells = {Cell(pattern, "tcp", size) for size in (64, 256, 1024, 4096, 65536, 131072)}
        if cells != loaded.keys():
            raise ValueError(f"{path}: unexpected C baseline cells")
        round_samples = rounds.setdefault(revision, {})
        if round_samples.keys() & cells:
            raise ValueError(f"{path}: repeated C baseline cells")
        round_samples.update(loaded)
    expected = {Cell(pattern, "tcp", size) for pattern in PATTERNS
                for size in (64, 256, 1024, 4096, 65536, 131072)}
    if set(rounds) != {1, 2, 3} or any(r.keys() != expected for r in rounds.values()):
        raise ValueError("expected complete C baseline rounds 1, 2 and 3")
    return rounds


def instability(rounds, cell):
    reasons = []
    for metric, label in SCREEN.items():
        values = [rounds[i][cell].number(metric) for i in sorted(rounds)]
        if None in values:
            reasons.append(f"{label} 무효 {values.count(None)}회")
        valid = [value for value in values if value is not None]
        if valid and min(valid) > 0:
            spread = max(valid) / min(valid) - 1
            if spread > LIMIT:
                reasons.append(f"{label} {spread:.1%}")
        elif valid and max(valid) > 0:
            reasons.append(f"{label} 최솟값 0")
    return reasons


def markdown(headers, rows):
    print("| " + " | ".join(headers) + " |")
    print("|" + "|".join("---" for _ in headers) + "|")
    for row in rows:
        print("| " + " | ".join(map(str, row)) + " |")


def formatted(value, places=3):
    return "—" if value is None else f"{value:.{places}f}"


def late_percent(sample):
    sends = number(sample.diagnostic.get("latency_sends", ""))
    late = number(sample.diagnostic.get("late_sends_gt_1ms", ""))
    return 100 * late / sends if sends and late is not None else None


def legacy(rounds):
    def pair(values):
        return " / ".join("nan" if value is None else f"{value:.3f}" for value in values)

    rows = []
    for cell in sorted(rounds[1], key=Cell.order):
        samples = [rounds[i][cell] for i in (1, 2)]
        rates = [s.number("throughput") for s in samples]
        late = [late_percent(s) for s in samples]
        if None in late:
            raise ValueError(f"{cell}: missing late-send diagnostic")
        rows.append((cell.pattern, f"{cell.size}B", pair([v / 1000 for v in rates]),
                     f"{rates[1] / rates[0] - 1:+.1%}",
                     pair([s.number("latency") for s in samples]),
                     pair([s.number("latency_p99") for s in samples]),
                     " / ".join(f"{v:.2f}" for v in late)))
    markdown(("패턴", "크기", "처리량 1회 / 2회 (K/s)", "차이",
              "지연 평균 1회 / 2회 (ms)", "p99 1회 / 2회", "늦은 송신 1회 / 2회 (%)"), rows)


def load_campaign(index, paths):
    records = defaultdict(lambda: defaultdict(list))

    def add(label, path):
        if label not in LANGUAGES:
            raise ValueError(f"unknown language {label!r}")
        for cell, sample in load_run(Path(path)).items():
            records[label][cell].append(sample)

    if index:
        for line in index.read_text().splitlines():
            add(*line.split("\t", 1))
    for item in paths:
        add(*item.split("=", 1))
    if "C" not in records:
        raise ValueError("a fresh C reference is required")
    return records


def middle(samples, metric):
    values = [s.number(metric) for s in samples]
    valid = [value for value in values if value is not None]
    return median(valid) if valid else None


def repeat_cells(records, label):
    for cell in sorted(records[label], key=Cell.order):
        samples = records[label][cell]
        reference = records["C"].get(cell)
        if reference is None:
            raise ValueError(f"missing C reference for {cell}")
        for metric in SCREEN:
            value, ref = middle(samples, metric), middle(reference, metric)
            if (value is None or ref is None
                    or (ref > 0 and not 1 - LIMIT <= value / ref <= 1 + LIMIT)
                    or (ref == 0 and value != 0)):
                yield cell
                break


def compare(records):
    rows = []
    for label in LANGUAGES:
        for cell, samples in sorted(records.get(label, {}).items(), key=lambda item: item[0].order()):
            reference = records["C"].get(cell, [])
            values = [middle(samples, metric) for metric in SCREEN]
            ratios = [value / ref if value is not None and ref else None
                      for value, metric in zip(values, SCREEN)
                      for ref in [middle(reference, metric)]]

            def diag(key):
                return " / ".join(s.diagnostic.get(key, "—") for s in samples)

            invalid = sorted({v for s in samples for k, v in s.values.items()
                              if k.startswith("latency") and v.startswith("invalid:")})
            rows.append((label, cell.pattern, cell.transport, cell.size, len(samples),
                         *[formatted(v) for v in values],
                         *["—" if v is None else f"{v:.1%}" for v in ratios],
                         diag("target_sender_rate"), diag("paced_received_rate"), diag("caught_up"),
                         " / ".join(formatted(late_percent(s), 2) for s in samples),
                         f"{sum(s.number('latency') is not None for s in samples)}/{len(samples)}",
                         ", ".join(invalid) or "—"))
    markdown(("언어", "패턴", "transport", "크기 (B)", "회차", "처리량 (/s)", "평균 (ms)",
              "p99 (ms)", "처리량 / C", "평균 / C", "p99 / C", "paced 목표 (/s)",
              "paced 수신 (/s)", "caught_up", "늦은 송신 (%)", "유효 평균 지연", "무효 사유"), rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subs = parser.add_subparsers(dest="mode", required=True)
    for mode in ("legacy", "unstable", "matrix"):
        sub = subs.add_parser(mode)
        sub.add_argument("baseline", type=Path)
        if mode == "unstable":
            sub.add_argument("--tsv", action="store_true")
    for mode in ("compare", "repeat"):
        sub = subs.add_parser(mode)
        sub.add_argument("--index", type=Path, help="TSV rows of language and run directory")
        if mode == "repeat":
            sub.add_argument("--language", required=True, choices=LANGUAGES[1:])
        sub.add_argument("runs", nargs="*", metavar="LANG=DIR")
    sub = subs.add_parser("expect")
    sub.add_argument("run", type=Path)
    sub.add_argument("pattern", choices=PATTERNS)
    sub.add_argument("transport")
    sub.add_argument("sizes", help="comma-separated integers")

    args = parser.parse_args()
    if args.mode == "expect":
        expected = {Cell(args.pattern, args.transport, int(size)) for size in args.sizes.split(",")}
        actual = load_run(args.run).keys()
        if actual != expected:
            raise ValueError(f"{args.run}: missing {expected - actual}, extra {actual - expected}")
        return
    if args.mode in ("legacy", "unstable", "matrix"):
        rounds = load_baseline(args.baseline)
        if args.mode == "legacy":
            legacy(rounds)
            return
        rows = []
        for cell in sorted(rounds[1], key=Cell.order):
            reasons = instability(rounds, cell)
            if args.mode == "matrix" or reasons:
                rows.append((cell.pattern, cell.transport, cell.size, ", ".join(reasons) or "—"))
        if args.mode == "unstable" and args.tsv:
            for row in rows:
                print(*row[:3], sep="\t")
        else:
            markdown(("패턴", "transport", "크기 (B)", "C 회차 간 변동 또는 무효"), rows)
        return
    records = load_campaign(args.index, args.runs)
    if args.mode == "compare":
        compare(records)
    else:
        for cell in repeat_cells(records, args.language):
            print(cell.pattern, cell.transport, cell.size, sep="\t")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        sys.exit(str(error))
