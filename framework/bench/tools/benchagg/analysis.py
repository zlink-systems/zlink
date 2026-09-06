"""Everything derived from normalized cells: rows, G5, and publication verdicts.

The point of putting this here rather than in a language harness is that the
conditions under which a number may be published become mechanical. Phase 0
published two wrong numbers that both looked plausible; the guard against a
third is that no path exists from a raw cell to a judgement except through
``judge``, and ``judge`` refuses whatever G5 refuses.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass, field

from .model import (
    CLIENT_SATURATION_FRACTION,
    G5_SPREAD_LIMIT_PERCENT,
    JUDGEMENT_PATTERN,
    JUDGEMENT_THRESHOLD,
    PATTERNS,
    Cell,
    CellKey,
    RunSet,
)

_MEDIAN_FIELDS = (
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
    "event_loop_utilization",
    "jvm_thread_cores",
    "submit_thread_cores",
    "drain_ms",
)


@dataclass
class Row:
    """One cell aggregated across runs. The unit a table and a judgement use."""

    key: CellKey
    run_count: int
    values: dict[str, float | None] = field(default_factory=dict)

    #: max deviation from the median, as a percent of the median (G5).
    spread_percent: float | None = None
    #: ``pass`` | ``fail`` | ``insufficient-runs``
    g5_status: str = "insufficient-runs"

    #: Highest depth any run reached, and the lowest of those per-run peaks.
    #: FB-017 asks whether the harness can fill the window at all, which one run
    #: reaching it answers; the low end is kept beside it so a run that could not
    #: is still visible.
    #: spec 5.1: the ceiling the client harness declared. Constant within a run,
    #: so it is carried rather than averaged.
    client_parallelism_ceiling: float | None = None
    #: FB-023 / FB-032 / FB-037: the instrument the harness declared for this row.
    #: Rows that name none are read as ``client_cores``, which is what every
    #: result predating the declaration meant.
    client_saturation_metric: str | None = None

    peak_in_flight: int | None = None
    peak_in_flight_min: int | None = None
    request_window: int | None = None
    abandoned: int | None = None
    drain_bound_hit: bool = False
    send_server_counted: bool | None = None

    excluded_runs: list[str] = field(default_factory=list)

    @property
    def g5_pass(self) -> bool:
        return self.g5_status == "pass"

    @property
    def throughput(self) -> float | None:
        return self.values.get("throughput_per_second")

    @property
    def in_flight_depth(self) -> float | None:
        """Little's law on the row medians. FB-010/FB-016 turned on this value."""
        throughput = self.values.get("throughput_per_second")
        latency = self.values.get("latency_mean_ms")
        if throughput is None or not latency:
            return None
        return throughput * latency / 1000.0

    @property
    def saturation_metric(self) -> str:
        """FB-023: the declared instrument, defaulting to cores for old results."""
        return self.client_saturation_metric or "client_cores"

    @property
    def saturation_value(self) -> float | None:
        return self.values.get(self.saturation_metric)

    @property
    def saturation_evaluated(self) -> bool:
        return self.saturation_value is not None and bool(self.client_parallelism_ceiling)

    @property
    def client_saturated(self) -> bool:
        """spec 5.1 on the row medians, against the declared ceiling.

        FB-023: which number this compares is chosen by the harness, not assumed
        here. Comparing process cores against Node's ceiling would mark every
        Node cell, because the binding's I/O threads are counted but run no user
        code.
        """
        if not self.saturation_evaluated:
            return False
        return self.saturation_value >= CLIENT_SATURATION_FRACTION * self.client_parallelism_ceiling

    def saturation_text(self) -> str:
        if not self.saturation_evaluated:
            return "not judged"
        return "**yes**" if self.client_saturated else "no"

    def g5_text(self) -> str:
        if self.spread_percent is None:
            return f"n/a ({self.run_count} run)"
        return f"{self.spread_percent:.1f}%"


def spread_percent(values: list[float]) -> float | None:
    """Widest distance from the median, as a percent of it (plan 6, G5)."""
    if len(values) < 2:
        return None
    median = statistics.median(values)
    if not median:
        return None
    return max(abs(value - median) / median for value in values) * 100.0


def _median(cells: list[Cell], name: str) -> float | None:
    values = [getattr(c, name) for c in cells]
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def build_row(run_set: RunSet, key: CellKey, min_runs_for_g5: int = 3) -> Row | None:
    """Aggregate one cell over the runs that measured it.

    Contaminated runs are dropped first (FB-008): a contaminated cell is not
    measured badly, it is not measured at all, so it may not move a median.
    """
    cells = run_set.for_key(key)
    if not cells:
        return None
    excluded = [c.run for c in run_set.cells if c.key == key and c.contaminated]

    row = Row(key=key, run_count=len(cells), excluded_runs=excluded)
    for name in _MEDIAN_FIELDS:
        row.values[name] = _median(cells, name)

    throughputs = [c.throughput_per_second for c in cells if c.throughput_per_second is not None]
    row.spread_percent = spread_percent(throughputs)
    if len(throughputs) < min_runs_for_g5 or row.spread_percent is None:
        row.g5_status = "insufficient-runs"
    else:
        row.g5_status = "pass" if row.spread_percent <= G5_SPREAD_LIMIT_PERCENT else "fail"

    peaks = [c.peak_in_flight for c in cells if c.peak_in_flight is not None]
    row.peak_in_flight = max(peaks) if peaks else None
    row.peak_in_flight_min = min(peaks) if peaks else None
    windows = [c.request_window for c in cells if c.request_window is not None]
    row.request_window = max(windows) if windows else None
    abandoned = [c.abandoned for c in cells if c.abandoned is not None]
    row.abandoned = max(abandoned) if abandoned else None
    ceilings = [c.client_parallelism_ceiling for c in cells if c.client_parallelism_ceiling]
    row.client_parallelism_ceiling = max(ceilings) if ceilings else None
    metrics = {c.client_saturation_metric for c in cells if c.client_saturation_metric}
    if len(metrics) > 1:
        # Runs of one cell that declare different instruments cannot be compared
        # against one ceiling, so the row declares none and reads as not judged.
        row.client_saturation_metric = None
    elif metrics:
        row.client_saturation_metric = metrics.pop()
    row.drain_bound_hit = any(c.drain_bound_hit for c in cells)
    counted = [c.send_throughput_server_counted for c in cells]
    counted = [c for c in counted if c is not None]
    row.send_server_counted = all(counted) if counted else None
    return row


def build_rows(run_set: RunSet, min_runs_for_g5: int = 3) -> dict[CellKey, Row]:
    rows = {}
    for key in run_set.keys():
        row = build_row(run_set, key, min_runs_for_g5)
        if row is not None:
            rows[key] = row
    return rows


@dataclass
class Judgement:
    """One spec 7.2 ratio for one payload size, with the reason it may be used."""

    formula: str
    numerator: CellKey
    denominator: CellKey
    payload_size: int
    #: ``published`` | ``unsupported``
    status: str
    #: ``pass`` | ``fail`` | ``None`` when unsupported.
    verdict: str | None = None
    value: float | None = None
    reason: str = ""

    @property
    def published(self) -> bool:
        return self.status == "published"

    def value_text(self) -> str:
        if self.value is None:
            return "n/a"
        return f"{self.value:.3f}" if self.published else f"({self.value:.3f})"


def _block_reason(role: str, row: Row | None, key: CellKey) -> str | None:
    """Why a row may not carry a judgement, naming the row and its spread."""
    if row is None or row.throughput is None:
        return f"{role} {key} was not measured"
    if row.excluded_runs and row.run_count == 0:
        return f"{role} {key} was contaminated in every run (FB-008)"
    if row.g5_status == "insufficient-runs":
        return f"{role} {key} has {row.run_count} run(s); G5 needs 3"
    if row.g5_status == "fail":
        return (
            f"{role} {key} fails G5 at {row.spread_percent:.1f}% "
            f"(limit {G5_SPREAD_LIMIT_PERCENT:.0f}%)"
        )
    if row.client_saturated:
        return (
            f"{role} {key} is client-saturated at {row.saturation_value:.2f} of "
            f"{row.client_parallelism_ceiling:g} declared {row.saturation_metric} "
            f"(spec 5.1, G6)"
        )
    if row.send_server_counted is False:
        return f"{role} {key} throughput is client-counted, not server-counted (G3, FB-014)"
    return None


def judge_pair(
    rows: dict[CellKey, Row],
    formula: str,
    numerator_impl: str,
    denominator_impl: str,
    payload_size: int,
    pattern: str = JUDGEMENT_PATTERN,
) -> Judgement:
    """Decide one ratio. FB-011: both rows must pass G5 or nothing is published."""
    numerator_key = CellKey(numerator_impl, pattern, payload_size)
    denominator_key = CellKey(denominator_impl, pattern, payload_size)
    numerator = rows.get(numerator_key)
    denominator = rows.get(denominator_key)

    value = None
    if (
        numerator is not None
        and denominator is not None
        and numerator.throughput
        and denominator.throughput
    ):
        value = numerator.throughput / denominator.throughput

    blockers = [
        reason
        for reason in (
            _block_reason("numerator", numerator, numerator_key),
            _block_reason("denominator", denominator, denominator_key),
        )
        if reason
    ]
    if blockers:
        return Judgement(
            formula=formula,
            numerator=numerator_key,
            denominator=denominator_key,
            payload_size=payload_size,
            status="unsupported",
            value=value,
            reason="; ".join(blockers),
        )

    verdict = "pass" if value is not None and value >= JUDGEMENT_THRESHOLD else "fail"
    return Judgement(
        formula=formula,
        numerator=numerator_key,
        denominator=denominator_key,
        payload_size=payload_size,
        status="published",
        verdict=verdict,
        value=value,
        reason="both rows pass G5",
    )


def judge_language(
    rows: dict[CellKey, Row],
    lang: str,
    baseline: str = "zlink-c",
    payload_sizes: tuple[int, ...] = (1024, 4096),
    pattern: str = JUDGEMENT_PATTERN,
) -> list[Judgement]:
    """The spec 7.2 pair of formulas, per payload size (FB-005)."""
    out = []
    for size in payload_sizes:
        out.append(
            judge_pair(rows, f"zlink-{lang} / {baseline}", f"zlink-{lang}", baseline, size, pattern)
        )
    for size in payload_sizes:
        out.append(
            judge_pair(
                rows,
                f"zlink-framework-{lang} / zlink-{lang}",
                f"zlink-framework-{lang}",
                f"zlink-{lang}",
                size,
                pattern,
            )
        )
    return out


def language_verdict(judgements: list[Judgement]) -> tuple[str, str]:
    """FB-005: a language passes only when every size is published and passes."""
    unsupported = [j for j in judgements if not j.published]
    if unsupported:
        return (
            "incomplete",
            f"{len(unsupported)} of {len(judgements)} judgement(s) unsupported; "
            "spec 7.2 needs both payload sizes",
        )
    failed = [j for j in judgements if j.verdict == "fail"]
    if failed:
        names = ", ".join(f"{j.formula}@{j.payload_size}={j.value:.3f}" for j in failed)
        return "fail", f"below {JUDGEMENT_THRESHOLD:.2f}: {names}"
    return "pass", f"all judgements published and at or above {JUDGEMENT_THRESHOLD:.2f}"


def ordered_keys(rows: dict[CellKey, Row], payload_sizes, implementations=None):
    """Table order: pattern, then payload size, then implementation."""
    impls = implementations or sorted({k.implementation for k in rows})
    for pattern in PATTERNS:
        for size in payload_sizes:
            for impl in impls:
                key = CellKey(impl, pattern, size)
                if key in rows:
                    yield key
