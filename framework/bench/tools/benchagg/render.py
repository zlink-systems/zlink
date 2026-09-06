"""Output: the spec 4 table, RESULT lines, and the tables judgement rests on.

Rendering is deliberately last in the pipeline and reads only ``Row`` and
``Judgement``. A number that reaches a table has already been through G5 and the
contamination filter; a number that reaches a judgement has been through both
sides of FB-011. Nothing here re-derives anything.
"""

from __future__ import annotations

from .analysis import Judgement, Row, ordered_keys
from .model import (
    G5_SPREAD_LIMIT_PERCENT,
    JUDGEMENT_THRESHOLD,
    PATTERNS,
    RESULT_METRICS,
    Cell,
    CellKey,
)

_SEND_PATTERN = "send-saturation"

_SPEC4_HEADER = (
    "      | Implementation          | Size     |       Throughput |    Bandwidth "
    "|  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Client CPU | Client Mem "
    "| Server CPU | Server Mem |"
)
_SPEC4_RULE = (
    "      |-------------------------|----------|------------------|--------------"
    "|--------------|--------------|--------------|------------|------------"
    "|------------|------------|"
)


def unit_of(pattern: str) -> str:
    """spec 5: completions for request patterns, server-received for send."""
    return "KMSG/s" if pattern == _SEND_PATTERN else "KOPS"


def _peak_text(row: Row) -> str:
    """Highest depth reached, with the lowest per-run peak when they differ."""
    if row.peak_in_flight is None:
        return "—"
    if row.peak_in_flight_min is not None and row.peak_in_flight_min != row.peak_in_flight:
        return f"{row.peak_in_flight} (low {row.peak_in_flight_min})"
    return str(row.peak_in_flight)


def _num(value: float | None, decimals: int, suffix: str = "") -> str:
    if value is None:
        return "n/a"
    return f"{value:.{decimals}f}{suffix}"


def render_spec4_table(rows: dict[CellKey, Row], payload_sizes, implementations=None) -> str:
    """The spec 4 report table, in the column layout the spec fixes.

    spec 5.1 requires a saturated cell to be marked, so a client CPU above the
    threshold carries a trailing ``*``. No column is added: the spec fixes this
    table's columns, and the diagnostics that Phase 0 showed to be decisive get
    their own table below rather than widening this one.
    """
    lines: list[str] = []
    for pattern in PATTERNS:
        keys = [k for k in ordered_keys(rows, payload_sizes, implementations) if k.pattern == pattern]
        if not keys:
            continue
        lines.append(f"  > Benchmarking current for {pattern}...")
        lines.append("    Testing local:")
        lines.append(_SPEC4_HEADER)
        lines.append(_SPEC4_RULE)
        for key in keys:
            row = rows[key]
            value = row.values
            throughput = value.get("throughput_per_second")
            throughput_text = (
                "n/a" if throughput is None else f"{throughput / 1000.0:.2f} {unit_of(pattern)}"
            )
            cpu = value.get("client_cpu_percent")
            cpu_text = "n/a" if cpu is None else f"{cpu:.1f}%" + ("*" if row.client_saturated else "")
            lines.append(
                f"      | {key.implementation:<23} | {str(key.payload_size) + 'B':<8} "
                f"| {throughput_text:>16} | {_num(value.get('bandwidth_mb_s'), 2, ' MB/s'):>12} "
                f"| {_num(value.get('latency_mean_ms'), 3, ' ms'):>12} "
                f"| {_num(value.get('latency_p95_ms'), 3, ' ms'):>12} "
                f"| {_num(value.get('latency_p99_ms'), 3, ' ms'):>12} "
                f"| {cpu_text:>10} | {_num(value.get('client_memory_mb'), 1, ' MB'):>10} "
                f"| {_num(value.get('server_cpu_percent'), 1, '%'):>10} "
                f"| {_num(value.get('server_memory_mb'), 1, ' MB'):>10} |"
            )
        lines.append("")
    return "\n".join(lines)


def render_result_lines(rows: dict[CellKey, Row], payload_sizes, implementations=None) -> str:
    """spec 4 ``RESULT`` lines, with throughput in completions per second.

    This is where spec 7.4 stops being needed: whichever runner produced a row,
    the number written here is in the unit spec 4 names, so a consumer never
    divides anything by 1000 to compare a C row with a .NET row.
    """
    field_of = {
        "throughput": "throughput_per_second",
        "bandwidth": "bandwidth_mb_s",
        "latency": "latency_mean_ms",
        "latency_p95": "latency_p95_ms",
        "latency_p99": "latency_p99_ms",
        "client_cpu_percent": "client_cpu_percent",
        "client_memory_mb": "client_memory_mb",
        "server_cpu_percent": "server_cpu_percent",
        "server_memory_mb": "server_memory_mb",
    }
    lines = []
    for key in ordered_keys(rows, payload_sizes, implementations):
        row = rows[key]
        for metric in RESULT_METRICS:
            value = row.values.get(field_of[metric])
            if value is None:
                continue
            lines.append(
                f"RESULT,current,{key.scenario()},local,{key.payload_size},{metric},{value:.3f}"
            )
    return "\n".join(lines)


def render_median_table(rows: dict[CellKey, Row], payload_sizes, implementations=None) -> str:
    """Medians in markdown, the shape a language summary document carries."""
    header = (
        "| Pattern | Size | Implementation | Throughput | Lat.Mean(ms) | Lat.P95(ms) "
        "| Lat.P99(ms) | Client CPU% | Client cores | Client MB | Server CPU% | Server MB "
        "| drain ms |"
    )
    lines = [header, "|" + "---|" * 13]
    for key in ordered_keys(rows, payload_sizes, implementations):
        row = rows[key]
        value = row.values
        throughput = value.get("throughput_per_second")
        cpu = value.get("client_cpu_percent")
        lines.append(
            f"| {key.pattern} | {key.payload_size} | `{key.implementation}` "
            f"| {'n/a' if throughput is None else f'{throughput / 1000.0:.3f}'} "
            f"| {_num(value.get('latency_mean_ms'), 3)} | {_num(value.get('latency_p95_ms'), 3)} "
            f"| {_num(value.get('latency_p99_ms'), 3)} "
            f"| {'n/a' if cpu is None else f'{cpu:.1f}' + ('*' if row.client_saturated else '')} "
            f"| {_num(value.get('client_cores'), 2)} "
            f"| {_num(value.get('client_memory_mb'), 1)} "
            f"| {_num(value.get('server_cpu_percent'), 1)} "
            f"| {_num(value.get('server_memory_mb'), 1)} "
            f"| {_num(value.get('drain_ms'), 0).replace('n/a', '—')} |"
        )
    return "\n".join(lines)


def render_diagnostics_table(rows: dict[CellKey, Row], payload_sizes, implementations=None) -> str:
    """FB-017 depth, FB-008 drain, G6 saturation, G8 measured in-flight depth.

    ``depth`` is throughput times mean latency. In Phase 0 a configured window of
    100 sat against a measured depth of 8, which is what disqualified a ratio
    that otherwise looked publishable, so it is a column here and not a remark.
    """
    header = (
        "| Pattern | Size | Implementation | peak_in_flight | window | abandoned "
        "| depth (thr x lat) | drain ms | drain bound hit | client cores | declared ceiling "
        "| saturated | send counted by |"
    )
    lines = [header, "|" + "---|" * 13]
    for key in ordered_keys(rows, payload_sizes, implementations):
        row = rows[key]
        depth = row.in_flight_depth
        drain = row.values.get("drain_ms")
        ceiling = row.client_parallelism_ceiling
        if row.send_server_counted is None:
            counted = "—"
        else:
            counted = "server" if row.send_server_counted else "**client (G3 fail)**"
        lines.append(
            f"| {key.pattern} | {key.payload_size} | `{key.implementation}` "
            f"| {_peak_text(row)} "
            f"| {'—' if row.request_window is None else row.request_window} "
            f"| {'—' if row.abandoned is None else row.abandoned} "
            f"| {'n/a' if depth is None else f'{depth:.1f}'} "
            f"| {'—' if drain is None else f'{drain:.0f}'} "
            f"| {'yes' if row.drain_bound_hit else 'no'} "
            f"| {_num(row.values.get('client_cores'), 2)} "
            f"| {'—' if ceiling is None else f'{ceiling:g}'} "
            f"| {row.saturation_text()} | {counted} |"
        )
    return "\n".join(lines)


def render_g5_table(rows: dict[CellKey, Row], payload_sizes, implementations=None) -> str:
    """G5 per row: spread of the runs about their median, and the verdict."""
    lines = [
        "| Pattern | Size | Implementation | runs | spread | G5 |",
        "|---|---|---|---|---|---|",
    ]
    for key in ordered_keys(rows, payload_sizes, implementations):
        row = rows[key]
        mark = {"pass": "pass", "fail": "**fail**"}.get(row.g5_status, "n/a")
        lines.append(
            f"| {key.pattern} | {key.payload_size} | `{key.implementation}` "
            f"| {row.run_count} | {row.g5_text()} | {mark} |"
        )
    return "\n".join(lines)


def render_judgement_table(judgements: list[Judgement]) -> str:
    """spec 7.2 ratios with the publication decision and the reason for it."""
    lines = [
        f"| Formula | Payload | Value | Status | Verdict (>= {JUDGEMENT_THRESHOLD:.2f}) | Reason |",
        "|---|---|---|---|---|---|",
    ]
    for judgement in judgements:
        verdict = judgement.verdict or "—"
        if judgement.verdict == "fail":
            verdict = "**fail**"
        status = judgement.status if judgement.published else "**unsupported**"
        lines.append(
            f"| `{judgement.formula}` | {judgement.payload_size} | {judgement.value_text()} "
            f"| {status} | {verdict} | {judgement.reason} |"
        )
    return "\n".join(lines)


def render_contaminated(cells: list[Cell]) -> str:
    """FB-008: cells excluded from every table and every judgement."""
    if not cells:
        return "None. Every cell drained within the bound."
    lines = ["| Run | Cell | Reason |", "|---|---|---|"]
    for cell in sorted(cells, key=lambda c: (c.run, str(c.key))):
        lines.append(f"| {cell.run} | `{cell.key}` | {cell.contamination_reason or 'unstated'} |")
    return "\n".join(lines)


def render_g5_note() -> str:
    return (
        f"G5 spread is the widest distance of any run from the median of the runs, "
        f"as a percent of that median. The limit is {G5_SPREAD_LIMIT_PERCENT:.0f}%."
    )
