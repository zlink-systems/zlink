"""Language launcher table: the only place the common runner learns how a language starts its role executables."""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess
from typing import Callable

ROLES = ("Client", "SessionServer", "ChannelServer")


@dataclass(frozen=True)
class Launcher:
    build: Callable[[Path, str], list[str]]  # (perf_dir, role) -> build command
    command: Callable[[Path, str], list[str]]  # (perf_dir, role) -> command without role arguments
    provenance: Callable[[Path], dict]  # (perf_dir) -> runtime, artifact and restored-package provenance


def _dotnet_role(perf_dir: Path, role: str) -> str:
    return "ZLink.Framework.Perf." + role


def _dotnet_output(perf_dir: Path, role: str) -> Path:
    return perf_dir / _dotnet_role(perf_dir, role) / "bin/Release/net8.0"


def _dotnet_provenance(perf_dir: Path) -> dict:
    # Packages come from each role's deps.json, i.e. what restore resolved; hashes are NuGet's nupkg sha512.
    artifacts, packages, runtime_settings = [], {}, {}
    for role in ROLES:
        output = _dotnet_output(perf_dir, role)
        name = _dotnet_role(perf_dir, role)
        artifacts.extend(sorted(output.glob("*.dll")))
        artifacts.extend(sorted((output / "runtimes").glob("*/native/libzlink.so")))
        runtime_file = output / (name + ".runtimeconfig.json")
        if runtime_file.is_file():
            artifacts.append(runtime_file)
            runtime_settings[role] = json.loads(runtime_file.read_text())
        deps_file = output / (name + ".deps.json")
        if deps_file.is_file():
            for key, library in json.loads(deps_file.read_text()).get("libraries", {}).items():
                package, _, version = key.partition("/")
                if library.get("type") == "package" and package.lower().startswith(("zlink", "systems.zlink")):
                    packages[key] = {"name": package, "version": version, "nupkgSha512": library.get("sha512")}
    return {
        "artifacts": artifacts,
        "packages": sorted(packages.values(), key=lambda item: item["name"]),
        "runtimeSettings": runtime_settings,
        "dotnetInfo": subprocess.check_output(["dotnet", "--info"], text=True),
        "installedRuntimes": subprocess.check_output(["dotnet", "--list-runtimes"], text=True),
        "runtimeOptions": {key: os.environ.get(key) for key in
                           ("DOTNET_PROCESSOR_COUNT", "DOTNET_GCHeapHardLimit", "DOTNET_gcServer",
                            "DOTNET_ThreadPool_ForceMinWorkerThreads", "DOTNET_ThreadPool_ForceMaxWorkerThreads")},
        "serializer": {"name": "default Framework typed JSON / ZlinkStreamJsonCodec", "runtime": "System.Text.Json",
                       "options": "lowerCamelCase, canonical decimal-string 64-bit values, Base64 text payload, no compression or custom message codec"},
        "clock": {"source": "System.Diagnostics.Stopwatch", "unit": "ns", "scope": "process"},
        "environment": {key: os.environ.get(key) for key in ("TMPDIR", "NUGET_PACKAGES", "UseSharedCompilation",
                                                             "MSBUILDDISABLENODEREUSE", "DOTNET_CLI_TELEMETRY_OPTOUT")},
    }


LAUNCHERS = {
    "dotnet": Launcher(
        build=lambda perf_dir, role: ["dotnet", "build", str(perf_dir / _dotnet_role(perf_dir, role)), "-c", "Release", "-m:1", "--nologo"],
        command=lambda perf_dir, role: ["dotnet", str(_dotnet_output(perf_dir, role) / (_dotnet_role(perf_dir, role) + ".dll"))],
        provenance=_dotnet_provenance,
    ),
}


def launcher(language: str) -> Launcher:
    if language not in LAUNCHERS:
        raise ValueError(f"No perf launcher for language '{language}' yet; available: {', '.join(sorted(LAUNCHERS))}")
    return LAUNCHERS[language]
