#!/usr/bin/env python3
"""Render one run directory as the unified with-grpc bench report.

Every language runner calls this at the end of a run, so ``report.txt`` has the
same columns and the same units no matter which harness produced it. The runner
never formats the table itself — a runner that prints its own table drifts, and
the drift is only visible when someone compares two languages by eye.

Throughput is printed in thousands per second, labelled by what the pattern
counts: ``KOPS`` for the request patterns (completed request/reply round trips)
and ``Kmsg/s`` for ``send-saturation`` (one-way messages the target received).
The number is the same scale in both; the label says what was counted.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from benchagg.readers import read_run  # noqa: E402

COLUMNS = ("Scenario", "Size", "Throughput", "Lat.Mean(ms)", "Lat.P95(ms)", "Lat.P99(ms)")
# The widest scenario name in the grid is ``zlink-framework-dotnet-request-backpressure``.
WIDTHS = (43, 6, 15, 12, 11, 11)


def throughput_cell(pattern: str, value: float | None) -> str:
    if value is None:
        return "n/a"
    unit = "Kmsg/s" if pattern == "send-saturation" else "KOPS"
    return f"{value / 1000.0:.3f} {unit}"


def number(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.3f}"


def render(run_dir: str) -> str:
    cells, notes = read_run(run_dir)
    lines = [
        "| " + " | ".join(name.ljust(width) for name, width in zip(COLUMNS, WIDTHS)) + " |",
        "|" + "|".join("-" * (width + 2) for width in WIDTHS) + "|",
    ]
    for cell in sorted(cells, key=lambda c: c.key):
        row = (
            cell.key.scenario().ljust(WIDTHS[0]),
            str(cell.key.payload_size).rjust(WIDTHS[1]),
            throughput_cell(cell.key.pattern, cell.throughput_per_second).rjust(WIDTHS[2]),
            number(cell.latency_mean_ms).rjust(WIDTHS[3]),
            number(cell.latency_p95_ms).rjust(WIDTHS[4]),
            number(cell.latency_p99_ms).rjust(WIDTHS[5]),
        )
        lines.append("| " + " | ".join(row) + " |")
    for note in notes:
        lines.append(f"# {note}")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", help="run directory holding the per-cell records")
    parser.add_argument("--output", help="write here instead of stdout")
    arguments = parser.parse_args()
    text = render(arguments.run_dir)
    if arguments.output:
        with open(arguments.output, "w", encoding="utf-8") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
