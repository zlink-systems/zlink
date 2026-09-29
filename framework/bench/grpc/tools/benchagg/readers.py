"""Reads the cell JSON every runner writes into the normalized schema.

The one input is ``<run directory>/<cell directory>/results.json``, a
``with-grpc-cell-v1`` document (README section 11). ``throughput_per_second`` is
always completions per second; diagnostics travel as fields, never as printed
text. Anything else in a run directory is not input and is not read.
"""

from __future__ import annotations

import json
import os
from glob import glob
from typing import Any, Iterable

from .model import INPUT_PATTERNS, Cell, CellKey, RunSet

#: The file name every runner uses for a cell's raw result.
CELL_FILE = "results.json"


class ReportError(RuntimeError):
    """A run could not be normalized. Never resolved by guessing."""


CELL_JSON_VERSION = "with-grpc-cell-v1"

_CELL_FIELDS = (
    "throughput_per_second",
    "bandwidth_mb_s",
    "latency_mean_ms",
    "latency_p95_ms",
    "latency_p99_ms",
    "client_cpu_percent",
    "client_memory_mb",
    "server_cpu_percent",
    "server_memory_mb",
    "client_cores",
    "client_parallelism_ceiling",
    "client_saturation_metric",
    "event_loop_utilization",
    "jvm_thread_cores",
    "submit_thread_cores",
    "peak_in_flight",
    "request_window",
    "abandoned",
    "drain_ms",
    "drain_bound_hit",
    "server_received_at_close",
    "contaminated",
    "contamination_reason",
)

_TRIGGER_FIELDS = (
    "runId",
    "cellId",
    "pattern",
    "payloadBytes",
    "durationMs",
    "warmup",
    "endpoint",
    "receivedAtUnixMs",
)


def _server_identity(raw: dict[str, Any], source: str) -> tuple[dict[str, Any], CellKey]:
    """Validate the spec 4 trigger and return its merge and table identities."""
    trigger = raw.get("trigger")
    if not isinstance(trigger, dict):
        raise ReportError(f"{source}: server-driven cell has no trigger object")
    missing = [name for name in _TRIGGER_FIELDS if name not in trigger]
    if missing:
        raise ReportError(f"{source}: trigger missing {', '.join(missing)}")
    if trigger["runId"] in (None, "") or trigger["cellId"] in (None, ""):
        raise ReportError(f"{source}: trigger runId and cellId must be non-empty")

    implementation = raw.get("implementation")
    if not implementation:
        raise ReportError(f"{source}: server-driven cell has no implementation")
    pattern = raw.get("pattern", trigger["pattern"])
    payload_size = raw.get("payload_size", trigger["payloadBytes"])
    if pattern != trigger["pattern"] or int(payload_size) != int(trigger["payloadBytes"]):
        raise ReportError(f"{source}: cell key disagrees with trigger pattern/payloadBytes")
    if pattern not in INPUT_PATTERNS:
        raise ReportError(f"{source}: unsupported server-driven pattern {pattern!r}")
    return trigger, CellKey(str(implementation), str(pattern), int(payload_size))


def _target_stats(raw: Any, source: str, pattern: str) -> dict[str, Any]:
    """Validate the spec 4 target_stats object without inventing defaults."""
    if not isinstance(raw, dict):
        return {}
    if pattern == "send-saturation" and "rejected" in raw and raw["rejected"] is None:
        raise ReportError(f"{source}: target rejection count unavailable")
    missing = [name for name in ("received", "errors", "drainMs") if name not in raw]
    if missing:
        raise ReportError(f"{source}: target_stats missing {', '.join(missing)}")
    try:
        received = int(raw["received"])
        errors = int(raw["errors"])
        drain_ms = float(raw["drainMs"])
    except (TypeError, ValueError) as error:
        raise ReportError(f"{source}: target_stats values must be numeric") from error
    if received < 0 or errors < 0 or drain_ms < 0:
        raise ReportError(f"{source}: target_stats values must be non-negative")
    return {"received": received, "errors": errors, "drainMs": drain_ms}


def _streams(raw: Any, source: str) -> dict[str, Any]:
    """Validate source A's logical stream declaration from spec 4."""
    if not isinstance(raw, dict):
        raise ReportError(f"{source}: source cell has no streams object")
    missing = [name for name in ("count", "inFlightPerStream") if name not in raw]
    if missing:
        raise ReportError(f"{source}: streams missing {', '.join(missing)}")
    try:
        count = int(raw["count"])
        in_flight = raw["inFlightPerStream"]
        in_flight = None if in_flight is None else int(in_flight)
    except (TypeError, ValueError) as error:
        raise ReportError(f"{source}: streams values must be integers or null") from error
    if count <= 0 or (in_flight is not None and in_flight <= 0):
        raise ReportError(f"{source}: streams values must be positive")
    return {"count": count, "inFlightPerStream": in_flight}


def cells_from_cell_json(payload: dict[str, Any], run: str, source: str = "") -> list[Cell]:
    """Read the per-cell shape every runner writes.

    ``{"schema": "with-grpc-cell-v1", "cells": [{...}]}`` where each cell names
    its implementation, pattern and payload size and gives throughput already in
    completions per second. Diagnostics travel as fields, not as printed text.
    A cell has no ``role`` (client-driven) or ``role: "source"`` (server-driven,
    completed from its own embedded ``target_stats``).
    """
    if payload.get("schema") != CELL_JSON_VERSION:
        raise ReportError(f"unsupported cell schema {payload.get('schema')!r}")
    raw_cells = payload.get("cells", [])
    if not isinstance(raw_cells, list):
        raise ReportError(f"{source or run}: cells must be an array")
    cells = []
    for index, raw in enumerate(raw_cells):
        origin = f"{source or run}:cells[{index}]"
        if not isinstance(raw, dict):
            raise ReportError(f"{origin}: cell must be an object")
        role = raw.get("role")
        if role not in (None, "source"):
            raise ReportError(f"{origin}: role must be absent or source")
        if role is None:
            key = CellKey(raw["implementation"], raw["pattern"], int(raw["payload_size"]))
            trigger: dict[str, Any] = {}
        else:
            trigger, key = _server_identity(raw, origin)
        cell = Cell(
            key=key,
            run=run,
            source=source,
            role=role,
            run_id=None if role is None else str(trigger["runId"]),
            cell_id=None if role is None else str(trigger["cellId"]),
            trigger=trigger,
        )
        for name in _CELL_FIELDS:
            if name in raw and raw[name] is not None:
                setattr(cell, name, raw[name])
        if role == "source":
            cell.streams = _streams(raw.get("streams"), origin)
        if "target_stats" in raw:
            cell.target_stats = _target_stats(raw.get("target_stats"), origin, key.pattern)
        cell.extra.update(raw.get("extra", {}))
        if raw.get("errors") is not None:
            cell.extra["errors"] = float(raw["errors"])
        if role == "source":
            _complete_from_target_stats(cell)
        cells.append(cell)
    return cells


def _complete_from_target_stats(source: Cell) -> None:
    """Finish a server-driven source cell from its own ``target_stats``.

    A cell without ``target_stats`` stays ``incomplete``. On ``send-saturation``
    the throughput is what the target received, not what the source submitted.
    """
    if not source.target_stats:
        source.status = "incomplete"
        source.incomplete_reason = "embedded target_stats is missing"
        return

    source.server_received_at_close = int(source.target_stats["received"])
    source.target_errors = int(source.target_stats["errors"])
    source.drain_ms = float(source.target_stats["drainMs"])
    if source.key.pattern == "send-saturation":
        duration_ms = float(source.trigger["durationMs"])
        if duration_ms <= 0:
            raise ReportError(
                f"runId={source.run_id} cellId={source.cell_id}: durationMs must be positive"
            )
        source.throughput_per_second = source.server_received_at_close * 1000.0 / duration_ms
        source.bandwidth_mb_s = (
            source.throughput_per_second * source.key.payload_size / 1_000_000.0
        )


def read_run(run_dir: str) -> tuple[list[Cell], list[str]]:
    """Read every ``<run_dir>/*/results.json`` into normalized cells plus notes."""
    run = os.path.basename(os.path.normpath(run_dir))
    paths = sorted(glob(os.path.join(run_dir, "*", CELL_FILE)))
    if not paths:
        raise ReportError(f"{run_dir}: no {CELL_FILE} in any cell directory")
    cells: list[Cell] = []
    for path in paths:
        try:
            with open(path, encoding="utf-8") as handle:
                payload = json.load(handle)
        except (json.JSONDecodeError, OSError) as error:
            raise ReportError(f"{path}: invalid cell JSON: {error}") from error
        if not isinstance(payload, dict):
            raise ReportError(f"{path}: cell JSON must be an object")
        cells.extend(cells_from_cell_json(payload, run, path))
    notes = [f"{run}: read cell data from {len(paths)} file(s)"]
    excluded = [c for c in cells if c.contaminated]
    if excluded:
        notes.append(f"{run}: {len(excluded)} contaminated cell(s) excluded (FB-008)")
    incomplete = [c for c in cells if not c.complete]
    if incomplete:
        notes.append(f"{run}: {len(incomplete)} incomplete cell(s)")
    return cells, notes


def read_runs(run_dirs: Iterable[str]) -> RunSet:
    """Read every run directory into one comparison set."""
    run_set = RunSet()
    for run_dir in run_dirs:
        cells, notes = read_run(run_dir)
        for cell in cells:
            run_set.add(cell)
        run_set.notes.extend(notes)
    incomplete = run_set.incomplete()
    if incomplete:
        run_set.notes.append(f"{len(incomplete)} incomplete server-driven cell(s) excluded")
    return run_set
