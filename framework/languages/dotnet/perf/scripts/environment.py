#!/usr/bin/env python3
"""Public OS/runtime and artifact provenance, with no mutable machine tuning."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[5]
PERF = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read(path: str) -> str | None:
    try:
        return Path(path).read_text().strip()
    except FileNotFoundError:
        return None


def collect() -> dict:
    cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.processor())
    artifacts = []
    runtime_settings = {}
    paths = [ROOT / ".artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg"]
    paths.append(PERF / "ZLink.Framework.Perf.Shared/histogram-bounds.json")
    paths.extend((ROOT / "core/build-dev/lib").glob("libzlink.so*"))
    for role in ("Client", "SessionServer", "ChannelServer"):
        paths.extend((PERF / f"ZLink.Framework.Perf.{role}/bin/Release/net8.0").glob("*.dll"))
        runtime_file = PERF / f"ZLink.Framework.Perf.{role}/bin/Release/net8.0/ZLink.Framework.Perf.{role}.runtimeconfig.json"
        if runtime_file.is_file():
            paths.append(runtime_file)
            runtime_settings[role] = json.loads(runtime_file.read_text())
        paths.extend((PERF / f"ZLink.Framework.Perf.{role}/bin/Release/net8.0/runtimes").glob("*/native/libzlink.so"))
    seen = set()
    for path in paths:
        if path.is_file() and str(path) not in seen:
            seen.add(str(path))
            artifacts.append({"path": str(path), "resolvedPath": str(path.resolve()), "sha256": digest(path)})
    limits = resource.getrlimit(resource.RLIMIT_NOFILE)
    return {
        "schemaVersion": 2,
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)),
        "buildMode": "Release", "frameworkVersion": "0.10.0", "bindingVersion": "0.17.0",
        "coreVersion": next(line.split("=", 1)[1] for line in (ROOT / "VERSION").read_text().splitlines() if line.startswith("LIBZLINK_VERSION=")),
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
        "dotnetInfo": subprocess.check_output(["dotnet", "--info"], text=True),
        "installedRuntimes": subprocess.check_output(["dotnet", "--list-runtimes"], text=True),
        "runtimeSettings": runtime_settings,
        "runtimeOptions": {key: os.environ.get(key) for key in
                           ("DOTNET_PROCESSOR_COUNT", "DOTNET_GCHeapHardLimit", "DOTNET_gcServer",
                            "DOTNET_ThreadPool_ForceMinWorkerThreads", "DOTNET_ThreadPool_ForceMaxWorkerThreads")},
        "environment": {key: os.environ.get(key) for key in ("TMPDIR", "ZLINK_LIBRARY_PATH", "NUGET_PACKAGES",
                            "UseSharedCompilation", "MSBUILDDISABLENODEREUSE", "DOTNET_CLI_TELEMETRY_OPTOUT")},
        "serializer": {"name": "default Framework typed JSON / ZlinkStreamJsonCodec", "runtime": "System.Text.Json",
                       "options": "lowerCamelCase, canonical decimal-string 64-bit values, Base64 text payload, no compression or custom message codec"},
        "clock": {"source": "System.Diagnostics.Stopwatch", "unit": "ns", "scope": "process"},
        "deployment": "same-host loopback; source and target share CPU resources",
        "artifacts": artifacts,
    }


if __name__ == "__main__":
    output = json.dumps(collect(), indent=2, ensure_ascii=False) + "\n"
    if len(sys.argv) == 2:
        with open(sys.argv[1], "x") as target:
            target.write(output)
    elif len(sys.argv) == 1:
        print(output, end="")
    else:
        raise SystemExit("usage: collect_env.sh [new-output-file]")
