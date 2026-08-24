#!/usr/bin/env python3
"""Run one .NET ZoneWorld lane and capture its Redis state without modifying the runner.

The runner provisions a scoped Redis container and removes it during cleanup.  This tool
discovers that container, records Redis MONITOR output, and writes a snapshot whenever the
key/value content changes.  Binary values are retained as base64 so opaque location and
relocation records remain byte-exact and reviewable by a later extractor.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from typing import Any


CONTAINER_PREFIX = "zlink-zoneworld-dotnet-redis-"


def run_bytes(*args: str) -> bytes:
    return subprocess.run(
        args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def redis(container: str, *args: str) -> bytes:
    return run_bytes("docker", "exec", container, "redis-cli", "--raw", *args)


def redis_lines(container: str, *args: str) -> list[bytes]:
    return redis(container, *args).splitlines()


def b64(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def snapshot(container: str) -> list[dict[str, Any]]:
    keys = sorted(redis_lines(container, "KEYS", "*"))
    result: list[dict[str, Any]] = []
    for key in keys:
        key_text = key.decode("utf-8")
        key_args = key_text,
        kind = redis(container, "TYPE", *key_args).strip().decode("ascii")
        entry: dict[str, Any] = {
            "key": key_text,
            "type": kind,
            "pttl_ms": int(redis(container, "PTTL", *key_args).strip()),
            # Redis DUMP is binary-safe for every data type and excludes the changing TTL.
            # It is the authoritative byte snapshot; decoded helpers below are review aids.
            "dump_b64": b64(redis(container, "DUMP", *key_args)),
        }
        if kind == "string":
            value = redis(container, "GET", *key_args)
            entry["value_b64"] = b64(value)
            try:
                entry["value_utf8"] = value.decode("utf-8")
            except UnicodeDecodeError:
                pass
        elif kind == "hash":
            decoded = subprocess.run(
                ["docker", "exec", container, "redis-cli", "--json", "HGETALL", key_text],
                check=True,
                stdout=subprocess.PIPE,
                text=True,
            ).stdout
            entry["fields"] = json.loads(decoded)
        result.append(entry)
    return result


def semantic_digest(entries: list[dict[str, Any]]) -> str:
    stable = [{k: v for k, v in entry.items() if k != "pttl_ms"} for entry in entries]
    encoded = json.dumps(stable, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def discover_container(existing: set[str], deadline: float) -> str:
    while time.monotonic() < deadline:
        names = run_bytes("docker", "ps", "--format", "{{.Names}}").decode().splitlines()
        candidates = [n for n in names if n.startswith(CONTAINER_PREFIX) and n not in existing]
        if len(candidates) == 1:
            return candidates[0]
        if len(candidates) > 1:
            raise RuntimeError(f"multiple new ZoneWorld Redis containers: {candidates}")
        time.sleep(0.05)
    raise TimeoutError("ZoneWorld Redis container did not appear")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", help="single ZoneWorld selector, for example ZW-B2")
    parser.add_argument("output", type=Path, help="new or empty raw-artifact directory")
    parser.add_argument("--runner", type=Path, required=True, help="path to .NET run_sample.sh")
    parser.add_argument(
        "--inject-pending-follow-project",
        type=Path,
        help="pause the runner after Gateway readiness and run the pending-Follow extractor")
    args = parser.parse_args()

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise SystemExit(f"output directory is not empty: {output}")

    existing = set(run_bytes("docker", "ps", "--format", "{{.Names}}").decode().splitlines())
    existing_run_dirs = set(Path("/tmp").glob("tmp.*"))
    pending_project = (args.inject_pending_follow_project.resolve()
                       if args.inject_pending_follow_project else None)
    pending_binary: Path | None = None
    if pending_project is not None:
        subprocess.run(
            ["dotnet", "build", str(pending_project), "--maxcpucount:1", "-v", "q", "--nologo"],
            check=True)
        pending_binary = pending_project.parent / "bin/Debug/net8.0/PendingFollowProbe"
        if not pending_binary.is_file():
            raise FileNotFoundError(f"pending-Follow extractor binary missing: {pending_binary}")
    env = os.environ.copy()
    env.update({
        "ZLINK_SAMPLE_KEEP_RUN_DIR": "1",
        "ZLINK_SAMPLE_TRACE_STREAM": "1",
        "ZLINK_SAMPLE_SPOT_DISCOVERY_TRACE": "1",
    })
    runner = subprocess.Popen(
        [str(args.runner.resolve()), "--no-browser-smoke", args.scenario],
        cwd=args.runner.resolve().parent,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    stdout_lines: list[str] = []
    run_dir: Path | None = None

    def drain_stdout() -> None:
        nonlocal run_dir
        assert runner.stdout is not None
        with (output / "runner.stdout.log").open("w", encoding="utf-8") as sink:
            for line in runner.stdout:
                sys.stdout.write(line)
                sink.write(line)
                sink.flush()
                stdout_lines.append(line)
                match = re.search(r"==> logs: (.+)/logs\s*$", line)
                if match:
                    run_dir = Path(match.group(1))

    stdout_thread = threading.Thread(target=drain_stdout, daemon=True)
    stdout_thread.start()

    monitor: subprocess.Popen[bytes] | None = None
    container = ""
    snapshot_error: Exception | None = None
    injection_error: Exception | None = None
    injection_thread: threading.Thread | None = None
    try:
        container = discover_container(existing, time.monotonic() + 30)
        (output / "redis-container.txt").write_text(container + "\n", encoding="utf-8")
        monitor_sink = (output / "redis-monitor.log").open("wb")
        monitor = subprocess.Popen(
            ["docker", "exec", container, "redis-cli", "MONITOR"],
            stdout=monitor_sink,
            stderr=subprocess.STDOUT,
        )

        if pending_binary is not None:
            def inject_pending_follow() -> None:
                nonlocal injection_error
                deadline = time.monotonic() + 60
                paused = False
                try:
                    while time.monotonic() < deadline:
                        candidates = [path for path in Path("/tmp").glob("tmp.*")
                                      if path not in existing_run_dirs
                                      and (path / "config/gateway.json").is_file()]
                        if len(candidates) == 1:
                            injected_run_dir = candidates[0]
                            break
                        if len(candidates) > 1:
                            raise RuntimeError(
                                f"multiple new ZoneWorld run directories: {candidates}")
                        if runner.poll() is not None:
                            raise RuntimeError("runner exited before run-directory discovery")
                        time.sleep(0.005)
                    else:
                        raise TimeoutError("ZoneWorld run directory did not appear")

                    gateway_log = injected_run_dir / "logs/gateway.log"
                    gateway_config = injected_run_dir / "config/gateway.json"
                    while time.monotonic() < deadline:
                        if (gateway_log.is_file()
                                and "Application started." in gateway_log.read_text(errors="replace")):
                            break
                        if runner.poll() is not None:
                            raise RuntimeError("runner exited before pending-Follow injection")
                        time.sleep(0.005)
                    else:
                        raise TimeoutError("Gateway did not become ready for pending-Follow injection")

                    # The runner is in its bounded Gateway readiness loop. Pausing only the shell
                    # keeps all role processes live while the extraction-only client runs.
                    os.kill(runner.pid, signal.SIGSTOP)
                    paused = True
                    config = json.loads(gateway_config.read_text())
                    gateway = config["gateway"]["streamEndpoint"]
                    with (output / "pending-follow.stdout.log").open(
                            "w", encoding="utf-8") as sink:
                        subprocess.run(
                            [str(pending_binary), "--gateway", gateway],
                            stdout=sink,
                            stderr=subprocess.STDOUT,
                            check=True,
                            text=True)
                except Exception as exc:
                    injection_error = exc
                finally:
                    if paused:
                        os.kill(runner.pid, signal.SIGCONT)

            injection_thread = threading.Thread(target=inject_pending_follow, daemon=True)
            injection_thread.start()

        last_digest = ""
        sequence = 0
        with (output / "redis-snapshots.jsonl").open("w", encoding="utf-8") as sink:
            while runner.poll() is None:
                try:
                    entries = snapshot(container)
                except subprocess.CalledProcessError:
                    # EXIT cleanup can remove Redis a few milliseconds before the stdout
                    # drainer observes the runner's final exit.  Treat only that exact race
                    # as normal completion; a live container error remains a capture failure.
                    time.sleep(0.1)
                    live = run_bytes(
                        "docker", "ps", "--format", "{{.Names}}"
                    ).decode().splitlines()
                    if run_dir is not None or runner.poll() is not None or container not in live:
                        break
                    raise
                digest = semantic_digest(entries)
                if digest != last_digest:
                    sequence += 1
                    record = {
                        "sequence": sequence,
                        "captured_unix_ns": time.time_ns(),
                        "semantic_sha256": digest,
                        "entries": entries,
                    }
                    sink.write(json.dumps(record, sort_keys=True) + "\n")
                    sink.flush()
                    last_digest = digest
                time.sleep(0.02)
    except Exception as exc:  # preserve lane output before reporting capture failure
        snapshot_error = exc
    finally:
        return_code = runner.wait()
        stdout_thread.join()
        if injection_thread is not None:
            injection_thread.join()
        if monitor is not None:
            monitor.terminate()
            try:
                monitor.wait(timeout=3)
            except subprocess.TimeoutExpired:
                monitor.kill()
                monitor.wait()
            monitor_sink.close()

    if run_dir is not None and (run_dir / "logs").is_dir():
        shutil.copytree(run_dir / "logs", output / "logs")

    metadata = {
        "scenario": args.scenario,
        "runner": str(args.runner.resolve()),
        "container": container,
        "exit_code": return_code,
        "run_dir": str(run_dir) if run_dir else None,
        "snapshot_error": repr(snapshot_error) if snapshot_error else None,
        "injection_error": repr(injection_error) if injection_error else None,
    }
    (output / "capture-metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if snapshot_error is not None:
        raise snapshot_error
    if injection_error is not None:
        raise injection_error
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
