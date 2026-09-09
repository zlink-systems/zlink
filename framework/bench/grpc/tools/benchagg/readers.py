"""Readers that turn each runner's output into the normalized schema.

Two report shapes exist today and they disagree about more than formatting.

  ``framework/bench/grpc/c``          throughput in KOPS, five patterns, extra
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
from glob import glob
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


_OPTION_LINE = re.compile(r"^  ([a-z_][a-z0-9_]*): (.*)$")

#: Keys the aggregator reads out of a runner's effective-options header.
#: ``client_parallelism_ceiling`` is spec 5.1's declared ceiling.
#: ``logical_cores`` lets a runner that reports only a machine-wide percentage
#: still yield cores used, without which spec 5.1 cannot be applied at all.
_OPTION_NUMBERS = ("client_parallelism_ceiling", "logical_cores")


def parse_options(text: str) -> dict[str, Any]:
    """Read the ``  key: value`` effective-options header both shapes emit."""
    out: dict[str, Any] = {}
    for line in text.splitlines():
        if line.startswith("RESULT,") or line.lstrip().startswith("|"):
            continue
        match = _OPTION_LINE.match(line)
        if not match:
            continue
        key, value = match.group(1), match.group(2).strip()
        if key in _OPTION_NUMBERS:
            try:
                out[key] = float(value)
            except ValueError:
                continue
        else:
            out[key] = value
    return out


def apply_client_ceiling(
    cells: list[Cell],
    ceiling: float | None,
    logical_cores: float | None,
) -> None:
    """Attach spec 5.1's declared ceiling and, where derivable, cores used.

    A cell that already carries ``client_cores`` (structured input) keeps it. A
    cell that carries only a machine-wide percentage gets cores from that
    percentage and the declared logical core count -- and gets nothing at all
    when the count was not declared, which leaves saturation unjudged rather
    than silently unsaturated.
    """
    for cell in cells:
        if ceiling and cell.client_parallelism_ceiling is None:
            cell.client_parallelism_ceiling = ceiling
        if (
            cell.client_cores is None
            and logical_cores
            and cell.client_cpu_percent is not None
        ):
            cell.client_cores = cell.client_cpu_percent / 100.0 * logical_cores


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
    if pattern not in PATTERNS:
        raise ReportError(f"{source}: unsupported server-driven pattern {pattern!r}")
    return trigger, CellKey(str(implementation), str(pattern), int(payload_size))


def _target_stats(raw: Any, source: str) -> dict[str, Any]:
    """Validate the spec 4 target_stats object without inventing defaults."""
    if not isinstance(raw, dict):
        return {}
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
    """Read the structured per-cell shape new language harnesses should emit.

    ``{"schema": "with-grpc-cell-v1", "cells": [{...}]}`` where each cell names
    its implementation, pattern and payload size and gives throughput already in
    completions per second. Diagnostics travel as fields, not as printed text.
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
        if role is not None and role not in ("source", "target"):
            raise ReportError(f"{origin}: role must be source or target")
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
            cell.target_stats = _target_stats(raw.get("target_stats"), origin)
        cell.extra.update(raw.get("extra", {}))
        if raw.get("errors") is not None:
            cell.extra["errors"] = float(raw["errors"])
        cells.append(cell)
    return cells


def cells_from_server_document(payload: dict[str, Any], run: str, source: str = "") -> list[Cell]:
    """Read a per-cell server-driven document with spec 4 fields at the root.

    Existing runners keep measurements in ``results`` and add ``role``,
    ``trigger``, ``streams`` and ``target_stats`` to that document. The same
    canonical fields are accepted inside ``cells`` by ``cells_from_cell_json``;
    only their container differs.
    """
    role = payload.get("role")
    if role not in ("source", "target"):
        raise ReportError(f"{source or run}: role must be source or target")
    trigger = payload.get("trigger")
    metadata = payload.get("metadata") if isinstance(payload.get("metadata"), dict) else {}
    results = payload.get("results", [])
    if not isinstance(results, list):
        raise ReportError(f"{source or run}: results must be an array")
    if not results:
        results = [{}]

    raw_cells: list[dict[str, Any]] = []
    for result in results:
        if not isinstance(result, dict):
            raise ReportError(f"{source or run}: result must be an object")
        implementation = (
            result.get("implementation")
            or payload.get("implementation")
            or metadata.get("implementation")
        )
        pattern = result.get("pattern") or (trigger or {}).get("pattern")
        payload_size = (
            result.get("payload_size")
            or result.get("payloadSize")
            or (trigger or {}).get("payloadBytes")
        )
        raw: dict[str, Any] = {
            "implementation": implementation,
            "pattern": pattern,
            "payload_size": payload_size,
            "role": role,
            "trigger": trigger,
        }
        if role == "source":
            raw["streams"] = payload.get("streams")
        if "target_stats" in payload:
            raw["target_stats"] = payload["target_stats"]

        for name in _CELL_FIELDS:
            if name in result:
                raw[name] = result[name]
            elif name in payload:
                raw[name] = payload[name]

        throughput = result.get("throughput")
        if throughput is not None:
            raw["throughput_per_second"] = float(throughput)
            raw.setdefault(
                "bandwidth_mb_s",
                float(throughput) * int(payload_size) / 1_000_000.0,
            )
        for source_name, target_name in (
            ("clientWorkingSetMb", "client_memory_mb"),
            ("serverWorkingSetMb", "server_memory_mb"),
            ("clientCores", "client_cores"),
            ("clientParallelismCeiling", "client_parallelism_ceiling"),
            ("peakInFlight", "peak_in_flight"),
            ("requestWindow", "request_window"),
            ("abandoned", "abandoned"),
        ):
            if result.get(source_name) is not None:
                raw[target_name] = result[source_name]

        mean = result.get("serverMeanMicros")
        p95 = result.get("serverP95Micros")
        p99 = result.get("serverP99Micros")
        for value, fallback, target_name in (
            (mean, result.get("meanMicros"), "latency_mean_ms"),
            (p95, result.get("p95Micros"), "latency_p95_ms"),
            (p99, result.get("p99Micros"), "latency_p99_ms"),
        ):
            selected = fallback if value is None else value
            if selected is not None:
                raw[target_name] = float(selected) / 1000.0

        duration = result.get("durationSeconds")
        logical_cores = metadata.get("logicalCores")
        if duration and logical_cores:
            for seconds_name, target_name in (
                ("clientCpuSeconds", "client_cpu_percent"),
                ("serverCpuSeconds", "server_cpu_percent"),
            ):
                if result.get(seconds_name) is not None:
                    raw[target_name] = (
                        float(result[seconds_name]) / float(duration) / float(logical_cores) * 100.0
                    )
        if result.get("errors") is not None:
            raw["errors"] = result["errors"]
        raw_cells.append(raw)

    return cells_from_cell_json(
        {"schema": CELL_JSON_VERSION, "cells": raw_cells}, run, source
    )


def _apply_target(source: Cell, target: Cell | None) -> Cell:
    """Finish one source cell from embedded or separately emitted target data."""
    if target is not None:
        if source.key != target.key:
            raise ReportError(
                f"runId={source.run_id} cellId={source.cell_id}: source/target cell keys differ"
            )
        for name in ("server_cpu_percent", "server_memory_mb"):
            value = getattr(target, name)
            if value is not None:
                setattr(source, name, value)
        if source.key.pattern == "send-saturation":
            for name in ("latency_mean_ms", "latency_p95_ms", "latency_p99_ms"):
                value = getattr(target, name)
                if value is not None:
                    setattr(source, name, value)
        if target.target_stats:
            if source.target_stats and source.target_stats != target.target_stats:
                raise ReportError(
                    f"runId={source.run_id} cellId={source.cell_id}: target_stats disagree"
                )
            source.target_stats = target.target_stats
        source.source = ",".join(filter(None, (source.source, target.source)))

    if not source.target_stats:
        source.status = "incomplete"
        source.incomplete_reason = "target record or embedded target_stats is missing"
        return source

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
    source.status = "complete"
    source.incomplete_reason = None
    return source


def merge_server_driven_cells(cells: Iterable[Cell]) -> list[Cell]:
    """Join A/B records only by the spec key ``trigger.runId``/``cellId``.

    Legacy records have no role and pass through unchanged. Server-driven source
    metrics own the workload result; target records may supply only target
    process resources and ``target_stats``. Missing counterparts are retained as
    ``incomplete`` cells, while duplicate roles are rejected as ambiguous input.
    """
    legacy: list[Cell] = []
    groups: dict[tuple[str, str], dict[str, Cell]] = {}
    order: list[tuple[str, str]] = []
    for cell in cells:
        if cell.role is None:
            legacy.append(cell)
            continue
        identity = (cell.run_id or "", cell.cell_id or "")
        group = groups.setdefault(identity, {})
        if not group:
            order.append(identity)
        if cell.role in group:
            raise ReportError(
                f"runId={identity[0]} cellId={identity[1]}: duplicate {cell.role} record"
            )
        group[cell.role] = cell

    merged = list(legacy)
    for identity in order:
        group = groups[identity]
        source = group.get("source")
        target = group.get("target")
        if source is not None:
            merged.append(_apply_target(source, target))
            continue
        assert target is not None
        target.status = "incomplete"
        target.incomplete_reason = "source record is missing"
        merged.append(target)
    return merged


#: ``results.json`` fields a harness writes when it carries diagnostics as data.
#: The camelCase names are the .NET report's convention; the values are the same
#: ones the prose reader has to recover from printed text.
_RESULTS_JSON_FIELDS = {
    "peakInFlight": "peak_in_flight",
    "requestWindow": "request_window",
    "abandoned": "abandoned",
    "drainMs": "drain_ms",
    "drainBoundHit": "drain_bound_hit",
    "serverReceivedAtClose": "server_received_at_close",
    "contaminated": "contaminated",
    "contaminationReason": "contamination_reason",
    "clientCores": "client_cores",
    "clientParallelismCeiling": "client_parallelism_ceiling",
    "clientSaturationMetric": "client_saturation_metric",
    "eventLoopUtilization": "event_loop_utilization",
    "jvmThreadCores": "jvm_thread_cores",
    "submitThreadCores": "submit_thread_cores",
}


def diagnostics_from_results_json(payload: dict[str, Any]) -> dict[tuple[str, int], dict[str, Any]]:
    """Read diagnostics out of a ``results.json`` that declares the v1 schema.

    Values that decide publication -- reached depth, drain, contamination, cores
    used -- must travel as data. A harness that does not declare the schema is
    older output and is read by the prose reader instead (FB-021).
    """
    metadata = payload.get("metadata") or {}
    if metadata.get("diagnosticsSchema") != CELL_JSON_VERSION:
        return {}
    out: dict[tuple[str, int], dict[str, Any]] = {}
    for result in payload.get("results", []):
        scenario = result.get("scenario")
        size = result.get("payloadSize")
        if scenario is None or size is None:
            continue
        entry = {
            field: result[name]
            for name, field in _RESULTS_JSON_FIELDS.items()
            if result.get(name) is not None
        }
        if entry:
            out[(scenario, int(size))] = entry
    return out


def contaminated_from_results_json(payload: dict[str, Any]) -> dict[str, str]:
    """FB-008 exclusions from a declaring ``results.json``.

    A contaminated cell was never measured, so it has no result entry to carry a
    flag; the run records it in metadata instead, as ``"<scenario>@<size>: why"``.
    """
    metadata = payload.get("metadata") or {}
    if metadata.get("diagnosticsSchema") != CELL_JSON_VERSION:
        return {}
    out: dict[str, str] = {}
    for item in metadata.get("contaminatedCells", []):
        cell, _, reason = str(item).partition(":")
        if cell:
            out[cell.strip()] = reason.strip()
    return out


def _structured_cells(run_dir: str, run: str, source: str) -> tuple[list[Cell], list[str]]:
    """Read every structured cell document in a run directory.

    A and B may write different JSON files. File names are deliberately not part
    of the schema: either the v1 ``schema`` wrapper or a spec 4 ``role`` at the
    document root identifies cell input; unrelated JSON is ignored.
    """
    cells: list[Cell] = []
    paths: list[str] = []
    for path in sorted(glob(os.path.join(run_dir, "*.json"))):
        try:
            with open(path, encoding="utf-8") as handle:
                payload = json.load(handle)
        except (json.JSONDecodeError, OSError) as error:
            if os.path.basename(path) == "cells.json":
                raise ReportError(f"{path}: invalid cell JSON: {error}") from error
            continue
        if not isinstance(payload, dict):
            continue
        if payload.get("schema") == CELL_JSON_VERSION:
            paths.append(path)
            cells.extend(cells_from_cell_json(payload, run, path))
        elif payload.get("role") in ("source", "target"):
            paths.append(path)
            cells.extend(cells_from_server_document(payload, run, path))
    notes = [f"{run}: read structured cell data from {len(paths)} file(s)"] if paths else []
    return cells, notes


def _read_run_fragments(run_dir: str, source: str = "") -> tuple[list[Cell], list[str]]:
    """Read one directory without finalizing cross-file/cross-directory joins.

    Preference order: structured ``cells`` or server-driven ``results`` JSON;
    legacy ``report.txt`` with v1 diagnostics in ``results.json``; and finally
    printed ``[bench]`` lines. The last is what older output leaves behind; it
    is a fallback, not a transport (FB-021).
    """
    run = os.path.basename(os.path.normpath(run_dir))
    structured_cells, structured_notes = _structured_cells(run_dir, run, source)
    if structured_cells:
        return structured_cells, structured_notes

    report = os.path.join(run_dir, "report.txt")
    if not os.path.isfile(report):
        raise ReportError(f"{run_dir}: no structured cell JSON and no report.txt")
    with open(report, encoding="utf-8", errors="replace") as handle:
        report_text = handle.read()
    cells, notes = cells_from_report(report_text, run, source)
    options = parse_options(report_text)

    diagnostics: dict[tuple[str, int], dict[str, Any]] = {}
    contaminated: dict[str, str] = {}

    results_json = os.path.join(run_dir, "results.json")
    structured: dict[tuple[str, int], dict[str, Any]] = {}
    structured_contamination: dict[str, str] = {}
    declared = False
    if os.path.isfile(results_json):
        try:
            with open(results_json, encoding="utf-8") as handle:
                payload = json.load(handle)
            structured = diagnostics_from_results_json(payload)
            structured_contamination = contaminated_from_results_json(payload)
            declared = (payload.get("metadata") or {}).get(
                "diagnosticsSchema"
            ) == CELL_JSON_VERSION
        except (json.JSONDecodeError, OSError):
            declared = False
    if declared:
        diagnostics.update(structured)
        contaminated.update(structured_contamination)
        notes.append(f"{run}: diagnostics read from results.json ({CELL_JSON_VERSION})")
    else:
        for text in _diagnostic_texts(run_dir):
            for key, entry in parse_diagnostics(text).items():
                diagnostics.setdefault(key, {}).update(entry)
            contaminated.update(parse_contaminated(text))
        if diagnostics or contaminated:
            notes.append(f"{run}: diagnostics recovered from printed [bench] lines (legacy)")

    for cell in cells:
        entry = diagnostics.get((cell.key.scenario(), cell.key.payload_size))
        if entry:
            for name, value in entry.items():
                setattr(cell, name, value)
        reason = contaminated.get(str(cell.key)) or contaminated.get(cell.key.scenario())
        if reason is not None:
            cell.contaminated = True
            cell.contamination_reason = reason

    measured = {str(cell.key) for cell in cells}
    for name, reason in contaminated.items():
        if name in measured:
            continue
        scenario, _, size = name.partition("@")
        split = split_scenario(scenario)
        if split is None or not size.isdigit():
            continue
        cells.append(
            Cell(
                key=CellKey(split[0], split[1], int(size)),
                run=run,
                source=source,
                contaminated=True,
                contamination_reason=reason,
            )
        )

    apply_client_ceiling(
        cells,
        options.get("client_parallelism_ceiling"),
        options.get("logical_cores"),
    )
    if not any(cell.saturation_evaluated for cell in cells):
        notes.append(
            f"{run}: no client parallelism ceiling declared; spec 5.1 saturation not judged"
        )
    excluded = [c for c in cells if c.contaminated]
    if excluded:
        notes.append(f"{run}: {len(excluded)} contaminated cell(s) excluded (FB-008)")
    return cells, notes


def read_run(run_dir: str, source: str = "") -> tuple[list[Cell], list[str]]:
    """Read and merge one run directory into normalized cells plus notes."""
    cells, notes = _read_run_fragments(run_dir, source)
    merged = merge_server_driven_cells(cells)
    incomplete = [cell for cell in merged if not cell.complete]
    if incomplete:
        notes.append(f"{os.path.basename(run_dir)}: {len(incomplete)} incomplete cell(s)")
    return merged, notes


def read_runs(run_dirs: Iterable[str], source: str = "") -> RunSet:
    """Read runs and merge server-driven records across every supplied file."""
    run_set = RunSet()
    fragments: list[Cell] = []
    for run_dir in run_dirs:
        cells, notes = _read_run_fragments(run_dir, source)
        fragments.extend(cells)
        run_set.notes.extend(notes)
    for cell in merge_server_driven_cells(fragments):
        run_set.add(cell)
    incomplete = run_set.incomplete()
    if incomplete:
        run_set.notes.append(f"{len(incomplete)} incomplete server-driven cell(s) excluded")
    return run_set
