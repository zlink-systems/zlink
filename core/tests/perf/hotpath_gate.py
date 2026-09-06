#!/usr/bin/env python3
"""Callgrind instruction-count gate for Core public hot paths."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


CELLS = (
    "dealer_dealer_inproc",
    "dealer_router_reqrep_inproc",
    "pair_inproc",
    "router_router_tcp",
    "stream_tcp",
)
DEFAULT_ITERATIONS = 20_000
REQREP_ITERATIONS = 5_000
TOLERANCE = 0.05


def parse_args() -> argparse.Namespace:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bench",
        type=Path,
        default=script_dir.parent.parent / "build-hp" / "bin" / "hotpath_bench",
        help="path to the hotpath_bench executable",
    )
    parser.add_argument(
        "--reference",
        type=Path,
        default=script_dir / "hotpath_reference.json",
        help="reference JSON path",
    )
    parser.add_argument(
        "--valgrind",
        default=shutil.which("valgrind") or "valgrind",
        help="valgrind executable",
    )
    parser.add_argument(
        "--cells",
        nargs="+",
        default=list(CELLS),
        help="cells to measure (space- or comma-separated)",
    )
    parser.add_argument(
        "--iterations",
        type=positive_int,
        help="override iterations for every selected cell",
    )
    parser.add_argument(
        "--update-reference",
        action="store_true",
        help="write measured values to the reference file (supervisor only)",
    )
    args = parser.parse_args()
    args.cells = normalize_cells(args.cells, parser)
    return args


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def normalize_cells(values: list[str], parser: argparse.ArgumentParser) -> list[str]:
    selected: list[str] = []
    for value in values:
        for cell in value.split(","):
            if cell not in CELLS:
                parser.error(f"unknown cell: {cell}")
            if cell not in selected:
                selected.append(cell)
    if not selected:
        parser.error("at least one cell is required")
    return selected


def iterations_for(cell: str, override: int | None) -> int:
    if override is not None:
        return override
    if cell == "dealer_router_reqrep_inproc":
        return REQREP_ITERATIONS
    return DEFAULT_ITERATIONS


def read_instruction_total(path: Path) -> int:
    summary: int | None = None
    totals: int | None = None
    number = re.compile(r"[0-9][0-9,]*")
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("summary:"):
                match = number.search(line.partition(":")[2])
                if match:
                    summary = int(match.group(0).replace(",", ""))
            elif line.startswith("totals:"):
                match = number.search(line.partition(":")[2])
                if match:
                    totals = int(match.group(0).replace(",", ""))
    # "totals:" is the run's grand total (the sum of every dump/part); prefer
    # it. "summary:" is only that one part's own tally, and with
    # --instr-atstart=no + CALLGRIND_START/STOP_INSTRUMENTATION the final
    # dump happens with instrumentation already back off, so its own
    # "summary:" legitimately reads 0 even though "totals:" carries the real,
    # correct count. Fall back to "summary:" only if "totals:" is missing.
    result = totals if totals is not None and totals > 0 else summary
    if result is None or result <= 0:
        raise RuntimeError(f"no positive summary/totals instruction count in {path}")
    return result


def measure_cell(
    valgrind: str, bench: Path, cell: str, iterations: int
) -> tuple[int, float]:
    with tempfile.TemporaryDirectory(prefix="zlink-hotpath-") as temporary:
        output = Path(temporary) / "callgrind.out"
        command = [
            valgrind,
            "--tool=callgrind",
            "--instr-atstart=no",
            f"--callgrind-out-file={output}",
            "--error-exitcode=99",
            str(bench),
            cell,
            str(iterations),
        ]
        completed = subprocess.run(command, text=True, capture_output=True)
        if completed.returncode != 0:
            if completed.stdout:
                print(completed.stdout, file=sys.stderr, end="")
            if completed.stderr:
                print(completed.stderr, file=sys.stderr, end="")
            raise RuntimeError(
                f"{cell}: callgrind/benchmark exited {completed.returncode}"
            )
        instructions = read_instruction_total(output)
        return instructions, instructions / iterations


def load_reference(path: Path) -> dict[str, float]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise RuntimeError(f"reference file not found: {path}") from error
    except json.JSONDecodeError as error:
        raise RuntimeError(f"invalid reference JSON {path}: {error}") from error
    if not isinstance(raw, dict):
        raise RuntimeError(f"reference must be a JSON object: {path}")
    references: dict[str, float] = {}
    for cell, value in raw.items():
        if cell not in CELLS:
            raise RuntimeError(f"unknown reference cell: {cell}")
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise RuntimeError(f"reference for {cell} must be numeric")
        numeric = float(value)
        if not math.isfinite(numeric) or numeric <= 0:
            raise RuntimeError(f"reference for {cell} must be positive and finite")
        references[cell] = numeric
    return references


def write_reference(path: Path, references: dict[str, float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ordered = {cell: round(references[cell], 6) for cell in CELLS if cell in references}
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    temporary.write_text(json.dumps(ordered, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def print_table(rows: list[tuple[str, float, float, float, str]]) -> None:
    print("cell | reference | measured | ratio | verdict")
    print("--- | ---: | ---: | ---: | ---")
    for cell, reference, measured, ratio, verdict in rows:
        print(
            f"{cell} | {reference:.3f} | {measured:.3f} | "
            f"{ratio:.4f} | {verdict}"
        )


def main() -> int:
    args = parse_args()
    bench = args.bench.resolve()
    if not bench.is_file() or not os.access(bench, os.X_OK):
        print(f"hotpath bench is not executable: {bench}", file=sys.stderr)
        return 2
    valgrind = shutil.which(args.valgrind) if os.sep not in args.valgrind else args.valgrind
    if not valgrind or not Path(valgrind).is_file():
        print(f"valgrind executable not found: {args.valgrind}", file=sys.stderr)
        return 2

    try:
        references = load_reference(args.reference) if args.reference.exists() else {}
        measured: dict[str, float] = {}
        for cell in args.cells:
            iterations = iterations_for(cell, args.iterations)
            instructions, per_message = measure_cell(
                valgrind, bench, cell, iterations
            )
            measured[cell] = per_message
            print(
                f"measured {cell}: {instructions} instructions / "
                f"{iterations} = {per_message:.6f}",
                file=sys.stderr,
            )

        if args.update_reference:
            references.update(measured)
            write_reference(args.reference, references)
            rows = [(cell, measured[cell], measured[cell], 1.0, "UPDATED") for cell in args.cells]
            print_table(rows)
            return 0

        missing = [cell for cell in args.cells if cell not in references]
        if missing:
            raise RuntimeError("missing reference cells: " + ", ".join(missing))
        rows = []
        failed = False
        for cell in args.cells:
            reference = references[cell]
            ratio = measured[cell] / reference
            verdict = "PASS" if 1.0 - TOLERANCE <= ratio <= 1.0 + TOLERANCE else "FAIL"
            failed = failed or verdict == "FAIL"
            rows.append((cell, reference, measured[cell], ratio, verdict))
        print_table(rows)
        return 1 if failed else 0
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"hotpath gate error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
