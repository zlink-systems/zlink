#!/usr/bin/env python3
"""Report advisory C++ test source sizes and compiler-launch RSS."""

import argparse
import html
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEST_LINE_LIMIT = 3000
# Starts just above the current largest TU (7 GiB) so only growth warns.
# Lower it step by step to the #5133 target of 2 GiB once the header diet lands.
TU_RSS_LIMIT_KIB = 7 * 1024 * 1024


def warning(path, message):
    file = str(path).replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    file = file.replace(":", "%3A").replace(",", "%2C")
    print(f"::warning file={file}::{message}")


def table_row(path, value):
    cell = html.escape(str(path)).replace("|", "&#124;")
    return f"| <code>{cell}</code> | {value:,} |"


def test_lines():
    oversized = []
    for path in sorted((ROOT / "framework/languages/cpp/tests").rglob("*")):
        if path.suffix not in {".cpp", ".hpp"} or not path.is_file():
            continue
        with path.open("rb") as source:
            lines = sum(1 for _ in source)
        if lines > TEST_LINE_LIMIT:
            relative = path.relative_to(ROOT)
            warning(relative, f"C++ test source has {lines:,} lines (limit: {TEST_LINE_LIMIT:,}).")
            oversized.append(table_row(relative, lines))
    return [
        "### C++ test sources over the line limit",
        "",
        f"Limit: {TEST_LINE_LIMIT:,} lines.",
        "",
        "| Source | Lines |",
        "| --- | ---: |",
        *(oversized or ["| No sources exceed the limit | |"]),
    ]


def tu_rss(log, limit):
    peaks = {}
    if log.is_file():
        with log.open(encoding="utf-8", errors="surrogateescape") as samples:
            for sample in samples:
                kib, source = sample.rstrip("\n").split("\t", 1)
                path = Path(os.path.normpath(source))
                if path.is_relative_to(ROOT):
                    path = path.relative_to(ROOT)
                peaks[path] = max(peaks.get(path, 0), int(kib))

    ranked = sorted(peaks.items(), key=lambda item: (-item[1], item[0]))
    for path, kib in ranked:
        if kib > limit:
            warning(path, f"C++ compile launch peaked at {kib:,} KiB RSS (limit: {limit:,} KiB).")
    return [
        "### C++ compile launches by peak RSS",
        "",
        f"Top 10 sources by maximum observed RSS. Warning limit: {limit:,} KiB.",
        "Recorded only for builds without sccache (pre-pr); the sccache server runs the compiler outside GNU time.",
        "",
        "| Source | Peak RSS (KiB) |",
        "| --- | ---: |",
        *([table_row(path, kib) for path, kib in ranked[:10]] or ["| No compile launches recorded | |"]),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    reports = parser.add_subparsers(dest="report", required=True)
    reports.add_parser("test-lines")
    rss = reports.add_parser("tu-rss")
    rss.add_argument("log", type=Path)
    rss.add_argument("--limit-kib", type=int, default=TU_RSS_LIMIT_KIB)
    args = parser.parse_args()

    rows = test_lines() if args.report == "test-lines" else tu_rss(args.log, args.limit_kib)
    summary = "\n".join([*rows, ""])
    if destination := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(destination, "a", encoding="utf-8", errors="backslashreplace") as output:
            output.write(summary + "\n")
    else:
        print(summary)


if __name__ == "__main__":
    main()
