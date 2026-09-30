#!/usr/bin/env python3
"""Public OS/runtime and artifact provenance, with no mutable machine tuning."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import sys

from launchers import ROLES, launcher

ROOT = Path(__file__).resolve().parents[3]
SCHEMA = Path(__file__).resolve().parents[1] / "schema"


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read(path: str) -> str | None:
    try:
        return Path(path).read_text().strip()
    except FileNotFoundError:
        return None


def collect(language: str, perf_dir: Path, roles: list[str], require_versions: bool = True) -> dict:
    cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.processor())
    runtime = launcher(language).provenance(perf_dir, roles)
    framework_version = runtime.pop("frameworkVersion", None)
    binding_version = runtime.pop("bindingVersion", None)
    declared_framework_version = runtime.pop("declaredFrameworkVersion", None)
    if require_versions:
        missing = [name for name, value in (("frameworkVersion", framework_version),
                                            ("bindingVersion", binding_version),
                                            ("declaredFrameworkVersion", declared_framework_version))
                   if not isinstance(value, str) or not value.strip()]
        if missing:
            raise ValueError("Launcher did not report required versions: " + ", ".join(missing))
    artifacts = []
    seen = set()
    for path in [SCHEMA / "histogram-bounds.json", *runtime.pop("artifacts")]:
        if path.is_file() and str(path) not in seen:
            seen.add(str(path))
            artifacts.append({"path": str(path), "resolvedPath": str(path.resolve()), "sha256": digest(path)})
    limits = resource.getrlimit(resource.RLIMIT_NOFILE)
    return {
        "schemaVersion": 2,
        "language": language,
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)),
        "buildMode": "Release",
        # Version values come from this language's launcher; the common runner does not inspect package names.
        "frameworkVersion": framework_version,
        "coreVersion": None,  # reported by the role processes from the libzlink they load
        "bindingVersion": binding_version,
        "declaredFrameworkVersion": declared_framework_version,
        "cpuModel": cpu, "effectiveProcessorCount": len(os.sched_getaffinity(0)),
        "cpuAffinity": sorted(os.sched_getaffinity(0)),
        "loadAverage": list(os.getloadavg()),
        "cpuQuota": read("/sys/fs/cgroup/cpu.max"),
        "cpuset": read("/sys/fs/cgroup/cpuset.cpus.effective"),
        "memoryLimit": read("/sys/fs/cgroup/memory.max"),
        "memoryCurrent": read("/sys/fs/cgroup/memory.current"),
        "memoryAvailable": next(line for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemAvailable:")),
        "os": platform.platform(), "kernel": platform.release(), "host": platform.node(),
        "container": Path("/.dockerenv").exists(), "cgroup": read("/proc/self/cgroup"),
        "fdLimit": {"soft": str(limits[0]), "hard": str(limits[1])},
        "ephemeralPortRange": read("/proc/sys/net/ipv4/ip_local_port_range"),
        "listenBacklog": read("/proc/sys/net/core/somaxconn"),
        "tcpMaxSynBacklog": read("/proc/sys/net/ipv4/tcp_max_syn_backlog"),
        "tcpTimeWaitReuse": read("/proc/sys/net/ipv4/tcp_tw_reuse"),
        **runtime,
        "deployment": "same-host loopback; source and target share CPU resources",
        "artifacts": artifacts,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Collect public OS/runtime and package provenance.")
    parser.add_argument("--language", required=True)
    parser.add_argument("--perf-dir", required=True, type=Path)
    parser.add_argument("output", nargs="?", type=Path, help="new output file; stdout when omitted")
    args = parser.parse_args()
    output = json.dumps(collect(args.language, args.perf_dir.resolve(), list(ROLES)), indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with args.output.open("x") as target:
            target.write(output)
    else:
        print(output, end="")
