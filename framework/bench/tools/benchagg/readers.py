"""Readers that turn each runner's output into the normalized schema.

Two report shapes exist today and they disagree about more than formatting.

  ``bindings/c/bench/with_grpc``      throughput in KOPS, five patterns, extra
                                      columns (Submitted, Completed, Errors,
                                      Blocked, MaxOut, SubmitMs), CPU columns
                                      named ``C.CPU%``/``S.CPU%``, no server
                                      receive count, no depth or drain reporting.
  ``framework/languages/*/bench``      throughput in completions per second, the
                                      three spec patterns, spec 4 column names,
                                      depth and drain reported on stdout.

The ``RESULT`` stream is the one thing both emit, so it is the interchange
format. Its throughput scale is not declared anywhere, so it is *derived* rather
than configured: ``bandwidth`` is fixed by spec 5 to be MB/s, so
``bandwidth * 1e6 / payload_size`` is the rate in completions per second no
matter which runner wrote the line, and dividing that by the reported
``throughput`` yields the scale the runner used. Measured over the Phase 0
material the two populations sit at 1.0000 and 1000.0, so the classification has
a margin of 100x and never has to guess.
"""

from __future__ import annotations

import json
import os
import re
from typing import Any, Iterable

from .model import PATTERNS, Cell, CellKey, RunSet

_RESULT_FIELDS = 7

#: Accepted scales for the raw ``throughput`` value in a RESULT line.
_SCALES: tuple[tuple[float, str], ...] = ((1.0, "per-second"), (1000.0, "KOPS"))

#: How far a report's observed scale may sit from a candidate and still match.
_SCALE_TOLERANCE = 0.05


class ReportError(RuntimeError):
    """A report could not be normalized. Never resolved by guessing."""


def split_scenario(scenario: str) -> tuple[str, str] | None:
    """``zlink-framework-dotnet-request-window`` -> implementation, pattern.

    Returns ``None`` for a scenario whose pattern is not one of the three the
    spec defines, so that the C bench's ``request-saturation`` and
    ``send-blocking`` cells are dropped visibly instead of being matched by a
    loose suffix rule.
    """
    for pattern in PATTERNS:
        suffix = "-" + pattern
        if scenario.endswith(suffix) and len(scenario) > len(suffix):
            return scenario[: -len(suffix)], pattern
    return None


def parse_result_lines(text: str) -> dict[tuple[str, int], dict[str, float]]:
    """Collect ``RESULT,current,<scenario>,local,<size>,<metric>,<value>`` rows."""
    out: dict[tuple[str, int], dict[str, float]] = {}
    for line in text.splitlines():
        if not line.startswith("RESULT,"):
            continue
        fields = line.strip().split(",")
        if len(fields) != _RESULT_FIELDS:
            continue
        _, _, scenario, _, size, metric, value = fields
        try:
            key = (scenario, int(size))
            out.setdefault(key, {})[metric] = float(value)
        except ValueError:
            continue
    return out


def detect_throughput_scale(rows: dict[tuple[str, int], dict[str, float]]) -> tuple[float, str]:
    """Derive the throughput scale a report used, from its own bandwidth column.

    Raises ``ReportError`` when the observed scale is neither of the two known
    ones or when rows disagree, because a report whose unit cannot be
    established must not reach a table.
    """
    observed: list[float] = []
    for (scenario, size), metrics in rows.items():
        if split_scenario(scenario) is None:
            continue
        throughput = metrics.get("throughput")
        bandwidth = metrics.get("bandwidth")
        if not throughput or not bandwidth or size <= 0:
            continue
        observed.append(bandwidth * 1e6 / size / throughput)
    if not observed:
        raise ReportError("no cell carries both throughput and bandwidth; scale undecidable")

    matches = set()
    for value in observed:
        hit = next((s for s, _ in _SCALES if abs(value / s - 1.0) <= _SCALE_TOLERANCE), None)
        if hit is None:
            raise ReportError(
                f"throughput scale {value:.4f} matches no known unit "
                f"(expected 1 for per-second or 1000 for KOPS)"
            )
        matches.add(hit)
    if len(matches) > 1:
        raise ReportError(f"report mixes throughput units: {sorted(matches)}")

    scale = matches.pop()
    return scale, dict(_SCALES)[scale]


_METRIC_TO_FIELD = {
    "bandwidth": "bandwidth_mb_s",
    "latency": "latency_mean_ms",
    "latency_p95": "latency_p95_ms",
    "latency_p99": "latency_p99_ms",
    "client_cpu_percent": "client_cpu_percent",
    "client_memory_mb": "client_memory_mb",
    "server_cpu_percent": "server_cpu_percent",
    "server_memory_mb": "server_memory_mb",
}

#: Columns only one runner emits. Carried through, never used for judgement.
_EXTRA_METRICS = (
    "submitted",
    "completed",
    "errors",
    "blocked",
    "max_outstanding",
    "submit_wait_ms",
)


def cells_from_report(text: str, run: str, source: str = "") -> tuple[list[Cell], list[str]]:
    """Normalize one ``report.txt`` into cells. Returns the cells and notes."""
    rows = parse_result_lines(text)
    if not rows:
        raise ReportError("report carries no RESULT lines")
    scale, unit_name = detect_throughput_scale(rows)

    notes = [f"{run}: throughput read as {unit_name} (scale {scale:g})"]
    dropped: set[str] = set()
    cells: list[Cell] = []
    for (scenario, size), metrics in sorted(rows.items()):
        split = split_scenario(scenario)
        if split is None:
            dropped.add(scenario)
            continue
        implementation, pattern = split
        cell = Cell(key=CellKey(implementation, pattern, size), run=run, source=source)
        throughput = metrics.get("throughput")
        if throughput is not None:
            cell.throughput_per_second = throughput * scale
        for metric, field_name in _METRIC_TO_FIELD.items():
            if metric in metrics:
                setattr(cell, field_name, metrics[metric])
        for metric in _EXTRA_METRICS:
            if metric in metrics:
                cell.extra[metric] = metrics[metric]
        # The C bench reports max outstanding per cell, which is the closest
        # thing that runner has to FB-017's peak depth. It is only that for the
        # request patterns: on a one-way send the C harness leaves it at zero,
        # and carrying that through would read as "the depth was zero".
        if "max_outstanding" in cell.extra and pattern != "send-saturation":
            cell.peak_in_flight = int(cell.extra["max_outstanding"])
        cells.append(cell)

    if dropped:
        notes.append(
            f"{run}: dropped {len(dropped)} out-of-spec scenario(s): " + ", ".join(sorted(dropped))
        )
    return cells, notes


# --- diagnostics -----------------------------------------------------------
#
# FB-008 and FB-017 values reach the aggregator on stdout today, because the
# .NET harness prints them and does not put them in results.json. Parsing text a
# human reads is the weak link in this pipeline; ``cells_from_cell_json`` below
# is the structured channel the four remaining languages should write instead.

_PAYLOAD_MARK = re.compile(r"^\[bench\] (?:request|send) payload=(\d+)")
_WINDOW_MARK = re.compile(
    r"^\[bench\] window (?P<scenario>\S+): peak_in_flight=(?P<peak>\d+) of (?P<window>\d+)"
    r" abandoned=(?P<abandoned>\d+)"
)
_DRAIN_MARK = re.compile(
    r"^\[bench\] drain (?P<scenario>\S+): (?P<ms>[\d.]+) ms bound_hit=(?P<hit>True|False)"
)
_BOUNDARY_MARK = re.compile(
    r"^\[bench\] boundary (?P<scenario>\S+): server_received_at_close=(?P<close>\d+)"
    r" post_drain=(?P<post>\d+) drain_ms=(?P<ms>[\d.]+)"
)
_CONTAMINATED_HEADER = "## Contaminated"
_CONTAMINATED_ITEM = re.compile(r"^-\s+(?P<cell>\S+?):\s*(?P<reason>.*)$")


def parse_diagnostics(text: str) -> dict[tuple[str, int], dict[str, Any]]:
    """Read depth, drain and boundary lines, keyed by scenario and payload.

    The lines carry no payload size of their own, so the size comes from the
    most recent ``[bench] ... payload=<n>`` section marker. That marker is what
    makes the attribution deterministic rather than positional guesswork.
    """
    out: dict[tuple[str, int], dict[str, Any]] = {}
    payload: int | None = None
    for line in text.splitlines():
        mark = _PAYLOAD_MARK.match(line)
        if mark:
            payload = int(mark.group(1))
            continue
        if payload is None:
            continue
        window = _WINDOW_MARK.match(line)
        if window:
            entry = out.setdefault((window.group("scenario"), payload), {})
            entry["peak_in_flight"] = int(window.group("peak"))
            entry["request_window"] = int(window.group("window"))
            entry["abandoned"] = int(window.group("abandoned"))
            continue
        drain = _DRAIN_MARK.match(line)
        if drain:
            entry = out.setdefault((drain.group("scenario"), payload), {})
            entry["drain_ms"] = float(drain.group("ms"))
            entry["drain_bound_hit"] = drain.group("hit") == "True"
            continue
        boundary = _BOUNDARY_MARK.match(line)
        if boundary:
            entry = out.setdefault((boundary.group("scenario"), payload), {})
            entry["server_received_at_close"] = int(boundary.group("close"))
            entry["drain_ms"] = float(boundary.group("ms"))
    return out


def parse_contaminated(text: str) -> dict[str, str]:
    """Read the ``## Contaminated`` section a harness writes on FB-008 overrun."""
    out: dict[str, str] = {}
    inside = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("## "):
            inside = stripped.startswith(_CONTAMINATED_HEADER)
            continue
        if not inside or not stripped:
            continue
        item = _CONTAMINATED_ITEM.match(stripped)
        if item:
            out[item.group("cell")] = item.group("reason").strip()
    return out


def _diagnostic_texts(run_dir: str) -> list[str]:
    """Every file that may carry diagnostics for one run directory."""
    base = os.path.basename(os.path.normpath(run_dir))
    candidates = [
        os.path.join(run_dir, "stdout.txt"),
        os.path.join(run_dir, "bench.stdout"),
        os.path.join(run_dir, "failures.txt"),
        os.path.join(os.path.dirname(os.path.normpath(run_dir)), base + ".stdout"),
    ]
    texts = []
    for path in candidates:
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as handle:
                texts.append(handle.read())
    return texts


# --- structured per-cell input (the target shape for new languages) ---------

CELL_JSON_VERSION = "with-grpc-cell-v1"


def cells_from_cell_json(payload: dict[str, Any], run: str, source: str = "") -> list[Cell]:
    """Read the structured per-cell shape new language harnesses should emit.

    ``{"schema": "with-grpc-cell-v1", "cells": [{...}]}`` where each cell names
    its implementation, pattern and payload size and gives throughput already in
    completions per second. Diagnostics travel as fields, not as printed text.
    """
    if payload.get("schema") != CELL_JSON_VERSION:
        raise ReportError(f"unsupported cell schema {payload.get('schema')!r}")
    cells = []
    for raw in payload.get("cells", []):
        cell = Cell(
            key=CellKey(raw["implementation"], raw["pattern"], int(raw["payload_size"])),
            run=run,
            source=source,
        )
        for name in (
            "throughput_per_second",
            "bandwidth_mb_s",
            "latency_mean_ms",
            "latency_p95_ms",
            "latency_p99_ms",
            "client_cpu_percent",
            "client_memory_mb",
            "server_cpu_percent",
            "server_memory_mb",
            "peak_in_flight",
            "request_window",
            "abandoned",
            "drain_ms",
            "drain_bound_hit",
            "server_received_at_close",
            "contaminated",
            "contamination_reason",
        ):
            if name in raw and raw[name] is not None:
                setattr(cell, name, raw[name])
        cell.extra.update(raw.get("extra", {}))
        cells.append(cell)
    return cells


def read_run(run_dir: str, source: str = "") -> tuple[list[Cell], list[str]]:
    """Read one run directory into normalized cells plus notes."""
    run = os.path.basename(os.path.normpath(run_dir))
    cell_json = os.path.join(run_dir, "cells.json")
    if os.path.isfile(cell_json):
        with open(cell_json, encoding="utf-8") as handle:
            return cells_from_cell_json(json.load(handle), run, source), [
                f"{run}: read structured {CELL_JSON_VERSION}"
            ]

    report = os.path.join(run_dir, "report.txt")
    if not os.path.isfile(report):
        raise ReportError(f"{run_dir}: no cells.json and no report.txt")
    with open(report, encoding="utf-8", errors="replace") as handle:
        cells, notes = cells_from_report(handle.read(), run, source)

    diagnostics: dict[tuple[str, int], dict[str, Any]] = {}
    contaminated: dict[str, str] = {}
    for text in _diagnostic_texts(run_dir):
        for key, entry in parse_diagnostics(text).items():
            diagnostics.setdefault(key, {}).update(entry)
        contaminated.update(parse_contaminated(text))

    for cell in cells:
        entry = diagnostics.get((cell.key.scenario(), cell.key.payload_size))
        if entry:
            for name, value in entry.items():
                setattr(cell, name, value)
        reason = contaminated.get(str(cell.key)) or contaminated.get(cell.key.scenario())
        if reason is not None:
            cell.contaminated = True
            cell.contamination_reason = reason
    if contaminated:
        notes.append(f"{run}: {len(contaminated)} contaminated cell(s) excluded (FB-008)")
    return cells, notes


def read_runs(run_dirs: Iterable[str], source: str = "") -> RunSet:
    """Read many run directories into one comparison set."""
    run_set = RunSet()
    for run_dir in run_dirs:
        cells, notes = read_run(run_dir, source)
        for cell in cells:
            run_set.add(cell)
        run_set.notes.extend(notes)
    return run_set
