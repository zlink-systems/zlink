"""Normalized schema shared by every with-grpc bench language.

A language process emits one cell record per (implementation, pattern, payload).
Everything derived from those records -- medians, reproducibility spread, ratios,
publication verdicts -- belongs to this package, never to a language harness.

Units are fixed here so that no consumer has to know which runner produced a row:

  throughput_per_second   completions per second (request patterns) or
                          server-received messages per second (send-saturation)
  latency_*_ms            milliseconds
  bandwidth_mb_s          megabytes (10^6 bytes) per second
  *_cpu_percent           percent of the whole machine (all logical cores)
  *_memory_mb             resident set in megabytes
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

#: Patterns the bench spec defines. A runner may emit more (the C bench emits
#: ``request-saturation`` and ``send-blocking``); those are outside the spec and
#: are dropped with a note rather than silently folded into a table.
PATTERNS: tuple[str, ...] = ("request-serial", "request-window", "send-saturation")

#: Payload sizes the spec fixes (spec 2, spec 3).
PAYLOAD_SIZES: tuple[int, ...] = (1024, 4096)

#: The nine metric names spec 4 fixes for ``RESULT`` lines, in output order.
RESULT_METRICS: tuple[str, ...] = (
    "throughput",
    "bandwidth",
    "latency",
    "latency_p95",
    "latency_p99",
    "client_cpu_percent",
    "client_memory_mb",
    "server_cpu_percent",
    "server_memory_mb",
)

#: G5 (plan 6): each run within +-10% of the median of the runs.
G5_SPREAD_LIMIT_PERCENT = 10.0

#: spec 7.2 / FB-003: the layer ratios pass at 0.80.
JUDGEMENT_THRESHOLD = 0.80

#: spec 5.1 / G6: at this fraction of the client parallelism ceiling the cell
#: measured the client runtime, not the transport, so it may not decide a
#: throughput comparison. The ceiling is declared per language, not assumed: a
#: percentage of every logical core cannot express the saturation of a
#: single-threaded client, which tops out near 5% on a 20-core machine.
CLIENT_SATURATION_FRACTION = 0.95

#: spec 7.2 / FB-003: judgement runs on this pattern only.
JUDGEMENT_PATTERN = "request-window"


@dataclass(frozen=True, order=True)
class CellKey:
    """One cell of the 18-cell grid: implementation, pattern, payload size."""

    implementation: str
    pattern: str
    payload_size: int

    def scenario(self) -> str:
        """The ``<implementation>-<pattern>`` name spec 4 puts in RESULT lines."""
        return f"{self.implementation}-{self.pattern}"

    def __str__(self) -> str:
        return f"{self.scenario()}@{self.payload_size}"


@dataclass
class Cell:
    """One measured cell from one run, in normalized units."""

    key: CellKey
    run: str
    source: str = ""

    throughput_per_second: float | None = None
    bandwidth_mb_s: float | None = None
    latency_mean_ms: float | None = None
    latency_p95_ms: float | None = None
    latency_p99_ms: float | None = None
    client_cpu_percent: float | None = None
    client_memory_mb: float | None = None
    server_cpu_percent: float | None = None
    server_memory_mb: float | None = None

    #: spec 5.1: CPU the client used, expressed as cores rather than as a share
    #: of the machine, and the parallelism ceiling the harness declared for that
    #: client. Both are needed to judge saturation; neither substitutes for the
    #: other.
    client_cores: float | None = None
    client_parallelism_ceiling: float | None = None

    # Diagnostics. FB-017 (depth), FB-008 (drain), spec 5 / G3 (server-counted
    # send throughput). ``None`` means the runner did not report the value, which
    # is not the same as zero and is never rendered as one.
    peak_in_flight: int | None = None
    request_window: int | None = None
    abandoned: int | None = None
    drain_ms: float | None = None
    drain_bound_hit: bool | None = None
    server_received_at_close: int | None = None

    contaminated: bool = False
    contamination_reason: str | None = None

    #: Runner-specific columns kept verbatim (C: submitted/completed/errors/
    #: blocked/max_outstanding/submit_wait_ms). Never used for judgement.
    extra: dict[str, float] = field(default_factory=dict)

    @property
    def in_flight_depth(self) -> float | None:
        """Requests actually outstanding, from Little's law.

        FB-010/FB-016: a configured ``request_window`` of 100 against a measured
        depth of 8 means the two sides of a ratio ran different experiments. This
        is why the value is computed for every cell and not only on demand.
        """
        if self.throughput_per_second is None or not self.latency_mean_ms:
            # A zero mean latency means the runner did not measure one -- the C
            # bench prints 0.000 for every send cell -- and multiplying by it
            # would render "the pipe was empty" instead of "not measured".
            return None
        return self.throughput_per_second * self.latency_mean_ms / 1000.0

    @property
    def saturation_evaluated(self) -> bool:
        """Whether spec 5.1 could be applied at all to this cell."""
        return self.client_cores is not None and bool(self.client_parallelism_ceiling)

    @property
    def client_saturated(self) -> bool:
        """spec 5.1: the client runtime, not the transport, set this ceiling.

        A cell that declares no ceiling is not saturated here -- it is simply not
        judged, and ``saturation_evaluated`` says so. Treating an undeclared
        ceiling as saturation would exclude every legacy result; treating it as
        proof of headroom would be the error spec 5.1 exists to prevent, which is
        why the unevaluated state is carried rather than collapsed either way.
        """
        if not self.saturation_evaluated:
            return False
        return self.client_cores >= CLIENT_SATURATION_FRACTION * self.client_parallelism_ceiling

    @property
    def send_throughput_server_counted(self) -> bool | None:
        """G3: whether a ``send-saturation`` throughput came from the server.

        ``None`` for request patterns, where the question does not apply. For a
        send cell the answer is decided by evidence in the run artefacts: a
        runner that never reports what the server received cannot have counted
        it, so the cell is unverified rather than assumed good (FB-014).
        """
        if self.key.pattern != "send-saturation":
            return None
        return self.server_received_at_close is not None


@dataclass
class RunSet:
    """Every cell read for one comparison, across runs and across runners."""

    cells: list[Cell] = field(default_factory=list)
    runs: list[str] = field(default_factory=list)
    metadata: dict[str, Any] = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)

    def add(self, cell: Cell) -> None:
        self.cells.append(cell)
        if cell.run not in self.runs:
            self.runs.append(cell.run)

    def for_key(self, key: CellKey) -> list[Cell]:
        """Every run's copy of one cell, contaminated cells excluded (FB-008)."""
        return [c for c in self.cells if c.key == key and not c.contaminated]

    def contaminated(self) -> list[Cell]:
        return [c for c in self.cells if c.contaminated]

    def keys(self) -> list[CellKey]:
        return sorted({c.key for c in self.cells})

    def implementations(self) -> list[str]:
        return sorted({c.key.implementation for c in self.cells})
