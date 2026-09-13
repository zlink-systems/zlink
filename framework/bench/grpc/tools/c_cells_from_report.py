#!/usr/bin/env python3
"""Turn the C reference bench's printed report into per-cell ``results.json``.

The C harness is client-driven (README §1.2): one client process walks the whole
grid and prints a table plus ``RESULT`` lines. Every other language writes a
``with-grpc-cell-v1`` document per cell. Rather than give the aggregator two
input shapes to keep straight, the C runner converts its own output here, so the
denominator of spec 7.2 formula 1 is read through exactly the same path as the
numerators.

The conversion reuses ``cells_from_report``, which derives the report's
throughput unit from its own bandwidth column instead of assuming one.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from benchagg.model import PATTERNS  # noqa: E402
from benchagg.readers import CELL_JSON_VERSION, ReportError, cells_from_report  # noqa: E402

FIELDS = (
    "throughput_per_second",
    "bandwidth_mb_s",
    "latency_mean_ms",
    "latency_p95_ms",
    "latency_p99_ms",
    "client_cpu_percent",
    "client_memory_mb",
    "server_cpu_percent",
    "server_memory_mb",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", help="the client's printed output")
    parser.add_argument("output", help="run directory to write cell directories into")
    parser.add_argument("--metadata", default="{}", help="JSON object recorded on every cell document")
    arguments = parser.parse_args()

    with open(arguments.report, encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    try:
        cells, _ = cells_from_report(text, os.path.basename(arguments.output))
    except ReportError as error:
        # A client that died mid-grid leaves a report the reader cannot scale. Saying so
        # beats a traceback, because the runner's caller is a shell, not a Python reader.
        print(f"{arguments.report}: unreadable client report: {error}", file=sys.stderr)
        return 1

    metadata = json.loads(arguments.metadata)
    written = 0
    for cell in cells:
        # The C bench also measures patterns outside the spec grid. They stay in
        # the client log; a cell document for them would put them in the table.
        if cell.key.pattern not in PATTERNS:
            continue
        document = {
            "implementation": cell.key.implementation,
            "pattern": cell.key.pattern,
            "payload_size": cell.key.payload_size,
        }
        for name in FIELDS:
            value = getattr(cell, name)
            if value is not None:
                document[name] = value
        if cell.extra:
            document["extra"] = dict(cell.extra)
        directory = os.path.join(arguments.output, f"{cell.key.scenario()}-{cell.key.payload_size}")
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, "results.json"), "w", encoding="utf-8") as handle:
            json.dump({"schema": CELL_JSON_VERSION, "metadata": metadata, "cells": [document]},
                      handle, indent=2)
            handle.write("\n")
        written += 1

    if not written:
        print(f"{arguments.report}: no spec-grid cell found", file=sys.stderr)
        return 1
    print(f"[bench] wrote {written} cell document(s) under {arguments.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
