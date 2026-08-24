#!/usr/bin/env python3
"""Capture one complete .NET ZoneWorld reference lane, including child topologies."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any


def classify(log_dir: Path) -> str:
    runner_log = ((log_dir / "runner.log").read_text(errors="replace")
                  if (log_dir / "runner.log").is_file() else "")
    if "scenario ZW-D2 passed" in runner_log:
        return "main"
    if (log_dir / "session-route-proxy-gateway.log").is_file():
        return "b8-child"
    client_log = ((log_dir / "client.log").read_text(errors="replace")
                  if (log_dir / "client.log").is_file() else "")
    if "scenario ZW-G4 passed" in client_log:
        return "g4-child"
    return "unclassified"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="new or empty raw-artifact directory")
    parser.add_argument("--runner", type=Path, required=True, help=".NET ZoneWorld run_sample.sh")
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise SystemExit(f"output directory is not empty: {output}")

    runner = args.runner.resolve()
    before = set(Path("/tmp").glob("tmp.*"))
    env = os.environ.copy()
    env.update({
        "ZLINK_SAMPLE_KEEP_RUN_DIR": "1",
        "ZLINK_SAMPLE_TRACE_STREAM": "1",
        "ZLINK_SAMPLE_SPOT_DISCOVERY_TRACE": "1",
    })

    lines: list[str] = []
    with (output / "runner.stdout.log").open("w", encoding="utf-8") as sink:
        process = subprocess.Popen(
            [str(runner), "--browser-smoke"],
            cwd=runner.parent,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert process.stdout is not None
        for line in process.stdout:
            sys.stdout.write(line)
            sink.write(line)
            sink.flush()
            lines.append(line)
        return_code = process.wait()

    advertised = []
    for line in lines:
        match = re.search(r"==> logs: (.+)/logs\s*$", line)
        if match:
            path = Path(match.group(1)).resolve()
            if path not in advertised:
                advertised.append(path)

    created = set(Path("/tmp").glob("tmp.*")) - before
    candidates = advertised + sorted(
        (path.resolve() for path in created
         if (path / "logs/client.log").is_file()
         and (path / "logs/routing-id-self-check.log").is_file()
         and path.resolve() not in advertised),
        key=str,
    )
    captures: list[dict[str, Any]] = []
    used_names: set[str] = set()
    try:
        for index, run_dir in enumerate(candidates, start=1):
            log_dir = run_dir / "logs"
            if not (log_dir / "client.log").is_file():
                continue
            name = classify(log_dir)
            if name == "unclassified" or name in used_names:
                name = f"run-{index:02d}"
            used_names.add(name)
            destination = output / name
            destination.mkdir()
            shutil.copytree(log_dir, destination / "logs")
            captures.append({
                "name": name,
                "source_run_dir": str(run_dir),
                "log_files": sorted(path.name for path in log_dir.iterdir() if path.is_file()),
            })
    finally:
        # Delete only newly-created ZoneWorld run directories after their logs are archived.
        for run_dir in candidates:
            if run_dir in before or not str(run_dir).startswith("/tmp/tmp."):
                continue
            if ((run_dir / "logs/client.log").is_file()
                    and (run_dir / "logs/routing-id-self-check.log").is_file()):
                shutil.rmtree(run_dir)

    metadata = {
        "runner": str(runner),
        "lane": "all",
        "browser_smoke": True,
        "trace_stream": True,
        "spot_discovery_trace": True,
        "exit_code": return_code,
        "captures": captures,
    }
    (output / "capture-metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    required = {"main", "g4-child", "b8-child"}
    captured = {item["name"] for item in captures}
    if required - captured:
        raise RuntimeError(f"full lane did not expose all run directories: {sorted(required - captured)}")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
