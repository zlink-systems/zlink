#!/usr/bin/env python3
"""Run CTest sanitizer suites and publish a machine-readable summary."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any


SANITIZER_PATTERNS = (
    re.compile(r"ERROR: (AddressSanitizer|LeakSanitizer):\s*([^\n]+)"),
    re.compile(r"SUMMARY: (AddressSanitizer|UndefinedBehaviorSanitizer):\s*([^\n]+)"),
    re.compile(r"ERROR: UndefinedBehaviorSanitizer:\s*([^\n]+)"),
    re.compile(r"runtime error:\s*([^\n]+)"),
)
ASSERTION_PATTERN = re.compile(
    r"^(?!\s*#)[^\n]*(?:Assertion|EXPECT_|FAILED)[^\n]*", re.MULTILINE
)


def diagnostics(output: str) -> tuple[list[str], list[str], bool]:
    sanitizer_reports: list[str] = []
    for pattern in SANITIZER_PATTERNS:
        for match in pattern.finditer(output):
            if len(match.groups()) == 2:
                sanitizer, detail = match.groups()
                sanitizer_reports.append(f"{sanitizer}: {detail}")
            else:
                sanitizer_reports.append(f"UndefinedBehaviorSanitizer: {match.group(1)}")

    return (
        sanitizer_reports,
        ASSERTION_PATTERN.findall(output),
        "AddressSanitizer:DEADLYSIGNAL" in output,
    )


def normalize_result(result: dict[str, Any]) -> dict[str, Any]:
    test_name = result.get("test")
    if not test_name:
        command = result.get("command", [])
        test_name = " ".join(str(part) for part in command) or "unknown"

    return {
        "test": str(test_name),
        "returncode": result.get("returncode", 1),
        "sanitizer": list(result.get("sanitizer", [])),
        "assertions": list(result.get("assertions", [])),
        "deadly_signal": bool(result.get("deadly_signal", False)),
    }


def summarize(results: list[dict[str, Any]]) -> dict[str, Any]:
    normalized = [normalize_result(result) for result in results]
    failed = [
        result
        for result in normalized
        if result["returncode"] != 0
        or result["sanitizer"]
        or result["assertions"]
        or result["deadly_signal"]
    ]
    return {
        "results": normalized,
        "total": len(normalized),
        "passed": len(normalized) - len(failed),
        "failed": failed,
    }


def parse_junit(path: Path, suite: str) -> list[dict[str, Any]]:
    root = ET.parse(path).getroot()
    results = []
    for case in root.iter("testcase"):
        name = case.attrib.get("name", "unknown")
        classname = case.attrib.get("classname")
        test_name = f"{suite}::{classname}::{name}" if classname else f"{suite}::{name}"
        output_parts = []
        case_failed = False
        for child in case:
            if child.tag in {"failure", "error"}:
                case_failed = True
            if child.text:
                output_parts.append(child.text)
            output_parts.extend(str(value) for value in child.attrib.values())

        reports, assertions, deadly_signal = diagnostics("\n".join(output_parts))
        # Passing tests may print these words; only a failed case's output is evidence.
        if not case_failed:
            assertions = []
        results.append(
            {
                "test": test_name,
                "returncode": 1 if case_failed else 0,
                "sanitizer": reports,
                "assertions": assertions,
                "deadly_signal": deadly_signal,
            }
        )
    return results


def run_suite(
    suite: str,
    build_dir: Path,
    label: str | None,
    log_dir: Path,
) -> list[dict[str, Any]]:
    log_dir.mkdir(parents=True, exist_ok=True)
    junit_path = log_dir / f"{suite}.junit.xml"
    log_path = log_dir / f"{suite}.ctest.log"
    command = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "--verbose",
        "--output-on-failure",
        "--parallel",
        "1",
        "--output-junit",
        str(junit_path),
    ]
    if label:
        command.extend(["-L", label])

    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    output = completed.stdout + completed.stderr
    log_path.write_text(output, encoding="utf-8")

    try:
        results = parse_junit(junit_path, suite) if junit_path.is_file() else []
    except ET.ParseError as error:
        results = [
            {
                "test": f"{suite}::ctest-junit",
                "returncode": 1,
                "sanitizer": [],
                "assertions": [f"Could not parse CTest JUnit output: {error}"],
                "deadly_signal": False,
            }
        ]

    if not results:
        reports, assertions, deadly_signal = diagnostics(output)
        message = "CTest produced no test case results."
        if completed.returncode != 0:
            message = f"CTest exited with code {completed.returncode}."
        results.append(
            {
                "test": f"{suite}::ctest",
                "returncode": completed.returncode or 1,
                "sanitizer": reports,
                "assertions": assertions + [message],
                "deadly_signal": deadly_signal,
            }
        )
    else:
        suite_reports, _, suite_deadly_signal = diagnostics(output)
        known_reports = {
            report for result in results for report in result["sanitizer"]
        }
        unassigned_reports = [
            report for report in suite_reports if report not in known_reports
        ]
        known_deadly_signal = any(result["deadly_signal"] for result in results)
        suite_failed = any(result["returncode"] != 0 for result in results)
        if (
            unassigned_reports
            or (suite_deadly_signal and not known_deadly_signal)
            or (completed.returncode != 0 and not suite_failed)
        ):
            results.append(
                {
                    "test": f"{suite}::ctest",
                    "returncode": completed.returncode,
                    "sanitizer": unassigned_reports,
                    "assertions": [],
                    "deadly_signal": suite_deadly_signal and not known_deadly_signal,
                }
            )

    return results


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--suite",
        action="append",
        nargs=2,
        metavar=("NAME", "BUILD_DIR"),
        required=True,
        help="CTest build directory to run; may be supplied more than once.",
    )
    parser.add_argument(
        "--label",
        action="append",
        nargs=2,
        metavar=("NAME", "REGEX"),
        default=[],
        help="CTest label filter for a named suite.",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--log-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    labels = dict(args.label)
    results = []
    for name, build_dir in args.suite:
        results.extend(
            run_suite(name, Path(build_dir), labels.get(name), args.log_dir)
        )

    summary = summarize(results)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(
        json.dumps(
            {
                "total": summary["total"],
                "passed": summary["passed"],
                "failed": len(summary["failed"]),
            }
        )
    )
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
