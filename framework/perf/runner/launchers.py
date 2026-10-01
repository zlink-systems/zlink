"""Language launcher table: the only place the common runner learns how a language starts its role executables."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
from typing import Callable

from scenarios import EXECUTABLES as ROLES

PACKAGES_FILE = Path(__file__).resolve().parents[1] / "schema/packages.json"


def declared_framework_version(language: str) -> str:
    packages = json.loads(PACKAGES_FILE.read_text())
    if not isinstance(packages, dict) or set(packages) != set(LAUNCHERS):
        raise ValueError("Perf packages.json must declare every supported Framework language")
    version = packages.get(language)
    if not isinstance(version, str) or not version:
        raise ValueError(f"Perf packages.json has no Framework version for '{language}'")
    return version


@dataclass(frozen=True)
class Launcher:
    build: Callable[[Path, str], list[str]]  # (perf_dir, role) -> build command
    command: Callable[[Path, str], list[str]]  # (perf_dir, role) -> command without role arguments
    provenance: Callable[[Path, list[str]], dict]  # (perf_dir, roles) -> runtime, artifact and restored-package provenance
    loaded_artifact_markers: tuple[str, ...]
    stream_scheme: str
    # Where this language documents the public ClientServer status and gates its Selectable state; quoted when a
    # ClientServer Server is Degraded although Serving, a Ready target and a typed probe reply are observed.
    clientserver_interface: str
    clientserver_gate: str


def _dotnet_role(perf_dir: Path, role: str) -> str:
    return "ZLink.Framework.Perf." + role


def _dotnet_output(perf_dir: Path, role: str) -> Path:
    return perf_dir / _dotnet_role(perf_dir, role) / "bin/Release/net8.0"


def _dotnet_provenance(perf_dir: Path, roles: list[str]) -> dict:
    # Packages come from each role's deps.json, i.e. what restore resolved; hashes are NuGet's nupkg sha512.
    artifacts, packages, runtime_settings = [], {}, {}
    for role in roles:
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
    listed_packages = sorted(packages.values(), key=lambda item: item["name"])
    version_of = lambda name: next((item["version"] for item in listed_packages if item["name"].lower() == name.lower()), None)
    return {
        "artifacts": artifacts,
        "packages": listed_packages,
        "frameworkVersion": version_of("Zlink.Framework"),
        "bindingVersion": version_of("Zlink"),
        "declaredFrameworkVersion": declared_framework_version("dotnet"),
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


NODE_ROLE_DIRS = {"Client": "client", "SessionActorLocalServer": "session-actor-local-server", "SessionServer": "session-server",
                  "ActorServer": "actor-server", "ChannelServer": "channel-server", "SpotServer": "spot-server",
                  "ActorCallerServer": "actor-caller-server", "PublisherServer": "publisher-server", "SubscriberServer": "subscriber-server"}


def _node_output(perf_dir: Path, role: str) -> Path:
    return perf_dir / "build" / NODE_ROLE_DIRS[role] / "main.js"


def _node_build(perf_dir: Path, role: str) -> list[str]:
    # Each role build reuses node_modules only when its lock hash and package source match. npm ci must leave the lock
    # unchanged; a local build relinks workspace packages, so a published build after it installs again.
    script = r'''set -euo pipefail
cd "$1"
lock_hash=$(sha256sum package-lock.json | cut -d ' ' -f 1)
marker="node_modules/.zlink-perf-package-lock.sha256"
install_key="$lock_hash $ZLINK_PERF_PACKAGE_SOURCE"
if [ ! -f "$marker" ] || [ "$(cat "$marker")" != "$install_key" ]; then
  set +e
  npm ci --no-audit --no-fund --loglevel=error
  npm_status=$?
  set -e
  installed_lock_hash=$(sha256sum package-lock.json | cut -d ' ' -f 1)
  if [ "$installed_lock_hash" != "$lock_hash" ]; then
    echo "npm ci changed package-lock.json; refusing to record the install as current" >&2
    exit 1
  fi
  if [ "$npm_status" -ne 0 ]; then
    exit "$npm_status"
  fi
  printf '%s\n' "$install_key" > "$marker.tmp"
  mv "$marker.tmp" "$marker"
fi
npm run build'''
    return ["bash", "-c", script, "zlink-node-build", str(perf_dir)]


def _node_provenance(perf_dir: Path, roles: list[str]) -> dict:
    # Packages come from package-lock.json, i.e. what npm resolved; hashes are the registry's integrity values.
    lock = json.loads((perf_dir / "package-lock.json").read_text())
    packages = {}
    for key, entry in lock.get("packages", {}).items():
        name = key.removeprefix("node_modules/")
        if key.startswith("node_modules/") and name.startswith("@zlink-systems/"):
            packages[name] = {"name": name, "version": entry["version"], "integrity": entry.get("integrity")}
    zlink_root = perf_dir / "node_modules/@zlink-systems/zlink"
    artifacts = [_node_output(perf_dir, role) for role in roles] + [Path(os.path.realpath(_node_binary()))]
    artifacts += sorted((zlink_root / "prebuilds/linux-x64").glob("*"))
    node_versions = json.loads(subprocess.check_output(["node", "-p", "JSON.stringify(process.versions)"], text=True))
    return {
        "artifacts": artifacts,
        "packages": sorted(packages.values(), key=lambda item: item["name"]),
        "frameworkVersion": packages["@zlink-systems/framework"]["version"],
        "bindingVersion": packages["@zlink-systems/zlink"]["version"],
        "declaredFrameworkVersion": declared_framework_version("node"),
        "runtimeSettings": {"node": {"execPath": _node_binary(), "versions": node_versions,
                                     "arguments": []}},
        "installedRuntimes": subprocess.check_output(["node", "--version"], text=True).strip(),
        "runtimeOptions": {key: os.environ.get(key) for key in ("NODE_OPTIONS", "UV_THREADPOOL_SIZE")},
        "serializer": {"name": "default Framework typed JSON / zlinkStreamJsonCodec", "runtime": "JSON (V8)",
                       "options": "class-named packets, canonical decimal-string 64-bit values, Base64 text payload, no compression or custom message codec"},
        "clock": {"source": "process.hrtime.bigint", "unit": "ns", "scope": "process"},
        "environment": {key: os.environ.get(key) for key in ("TMPDIR", "NPM_CONFIG_CACHE")},
    }


def _node_binary() -> str:
    return subprocess.check_output(["node", "-p", "process.execPath"], text=True).strip()


def _cpp_target(role: str) -> str:
    """Role executable -> CMake target/binary of framework/languages/cpp/perf (§17.5): SessionServer -> perf_session_server."""
    return "perf_" + re.sub(r"(?<!^)(?=[A-Z])", "_", role).lower()


def _cpp_provenance(perf_dir: Path, roles: list[str]) -> dict:
    """The C++ perf links the framework-cpp package in .zlink/install (§17.5): samples/bootstrap.cmake extracts the release
    asset (published) or scripts/build_role.sh installs this checkout (local). Core is loaded dynamically, the binding is
    static, Framework is a shared library of the package."""
    install = perf_dir / ".zlink" / "install"
    downloads = perf_dir / ".zlink" / "downloads"
    declared = declared_framework_version("cpp")

    def sha256(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def cmake_version(package: str, config: str) -> str | None:
        found = install / "lib/cmake" / package / config
        match = re.search(r'set\(PACKAGE_VERSION "([^"]+)"\)', found.read_text()) if found.is_file() else None
        return match.group(1) if match else None

    framework = cmake_version("zlink_framework", "zlink_frameworkConfigVersion.cmake")
    # A local build removes the downloaded release assets, so an archive here is the one the installed package came from.
    archives = sorted(downloads.glob(f"zlink-framework-cpp-{framework}-*.tar.gz")) if framework and downloads.is_dir() else []
    binding = cmake_version("zlink_cpp", "zlink_cppConfigVersion.cmake")
    artifacts = []
    packages = []
    if framework is not None:
        libraries = [path for path in sorted((install / "lib").glob("lib*")) if path.is_file() and not path.is_symlink()]
        artifacts.extend(archives + libraries)
        packages = [
            {"name": "Zlink.Framework", "version": framework, "declaredVersion": declared, "artifact": archives[0].name if archives else None,
             "sha256": sha256(archives[0]) if archives else None},
            {"name": "Zlink", "version": binding, "linkage": "static libzlink_cpp.a inside the package",
             "sha256": sha256(install / "lib/libzlink_cpp.a")},
        ]
    for role in roles:
        binary = perf_dir / "build" / _cpp_target(role)
        if binary.is_file():
            artifacts.append(binary)
    cache = perf_dir / "build" / "CMakeCache.txt"
    cache_values = dict(line.split("=", 1) for line in cache.read_text().splitlines() if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_FLAGS_RELEASE:", "CMAKE_CXX_COMPILER:"))) if cache.is_file() else {}
    return {
        "artifacts": artifacts,
        "packages": packages,
        "frameworkVersion": framework,
        "bindingVersion": binding,
        "declaredFrameworkVersion": declared,
        "runtimeSettings": {"buildType": cache_values.get("CMAKE_BUILD_TYPE:STRING"), "cxxFlagsRelease": cache_values.get("CMAKE_CXX_FLAGS_RELEASE:STRING"),
                            "cxxCompiler": cache_values.get("CMAKE_CXX_COMPILER:FILEPATH"), "packageRoot": str(install)},
        "installedRuntimes": subprocess.check_output(["g++", "--version"], text=True),
        "runtimeOptions": {key: os.environ.get(key) for key in ("MALLOC_ARENA_MAX", "GLIBC_TUNABLES", "LD_LIBRARY_PATH")},
        "serializer": {"name": "default Framework typed JSON serializer / stream-connector JSON codec", "runtime": "nlohmann::json 3.11.3",
                       "options": "lowerCamelCase, canonical decimal-string 64-bit values, Base64 text payload, no compression or custom message codec"},
        "clock": {"source": "clock_gettime(CLOCK_MONOTONIC)", "unit": "ns", "scope": "process"},
        "environment": {key: os.environ.get(key) for key in ("ZLINK_PERF_BUILD_JOBS", "CC", "CXX")},
    }


JAVA_LOCK = "/tmp/zlink-framework-java-kotlin-sample-gradle.lock"  # the Java/Kotlin build-only lock (§20)


def _java_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _java_module(role: str) -> str:
    """Role executable name -> Gradle project folder, e.g. SessionActorLocalServer -> session-actor-local-server."""
    return re.sub(r"(?<!^)(?=[A-Z])", "-", role).lower()


def _java_install(perf_dir: Path, role: str) -> Path:
    module = _java_module(role)
    return perf_dir / module / "build/install" / module


def _gradle_install_dist(perf_dir: Path, project: str) -> list[str]:
    return ["flock", "--exclusive", "--close", JAVA_LOCK, "bash", str(perf_dir / "scripts/build_role.sh"), str(perf_dir),
            project + ":installDist"]


def _java_provenance(language: str, perf_dir: Path, roles: list[str]) -> dict:
    """Packages are what Gradle resolved into each role's installDist lib folder (published Maven artifacts only)."""
    artifacts, packages, runtime_settings = [], {}, {}
    jackson = None
    for role in roles:
        install = _java_install(perf_dir, role)
        module = _java_module(role)
        artifacts.append(install / "bin" / module)
        for jar in sorted((install / "lib").glob("*.jar")):
            match = re.fullmatch(r"(.+?)-(\d+\.\d+\.\d+[^/]*)\.jar", jar.name)
            if jar.name.startswith("jackson-databind") and match:
                jackson = match.group(2)
            if jar.name.startswith("zlink"):
                artifacts.append(jar)
                if match:
                    packages[jar.name] = {"name": "systems.zlink:" + match.group(1), "version": match.group(2), "jarSha256": _java_sha256(jar)}
            elif jar.name in (module + ".jar", "shared.jar", "server-support.jar") or jar.name.startswith("kotlin-"):
                artifacts.append(jar)
        runtime_settings[role] = {"startScript": str(install / "bin" / module), "defaultJvmOpts": ["--enable-native-access=ALL-UNNAMED"]}
    listed = sorted(packages.values(), key=lambda item: item["name"])
    version_of = lambda name: next((item["version"] for item in listed if item["name"] == name), None)  # noqa: E731
    java = subprocess.run(["java", "--version"], capture_output=True, text=True, check=True).stdout
    return {
        "artifacts": artifacts,
        "packages": listed,
        # The launcher reports the versions resolved into the published Maven installDist artifacts.
        "frameworkVersion": version_of("systems.zlink:zlink-framework-core"),
        "bindingVersion": version_of("systems.zlink:zlink"),
        "declaredFrameworkVersion": declared_framework_version(language),
        "runtimeSettings": runtime_settings,
        "installedRuntimes": java,
        "javaHome": os.environ.get("JAVA_HOME"),
        "runtimeOptions": {key: os.environ.get(key) for key in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "JAVA_OPTS")},
        "serializer": {"name": "default Framework typed JSON serializer (framework-json-v1)", "runtime": "Jackson databind " + str(jackson),
                       "options": "lowerCamelCase, canonical decimal-string 64-bit values, Base64 text payload, no compression or custom message codec"},
        "clock": {"source": "System.nanoTime", "unit": "ns", "scope": "process"},
        "environment": {key: os.environ.get(key) for key in ("JAVA_HOME", "GRADLE_OPTS")},
    }


LAUNCHERS = {
    "node": Launcher(
        build=_node_build,
        command=lambda perf_dir, role: ["node", str(_node_output(perf_dir, role))],
        provenance=_node_provenance,
        loaded_artifact_markers=(".node", "libzlink.so"),
        stream_scheme="ws",
        clientserver_interface="framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md:480",
        clientserver_gate="Runtime implementation gates Selectable on readyTargetCount at "
                          "framework/languages/node/packages/framework/src/runtime/foundation/runtime-state-projections.ts:68.",
    ),
    "cpp": Launcher(
        build=lambda perf_dir, role: ["bash", str(perf_dir / "scripts/build_role.sh"), _cpp_target(role)],
        command=lambda perf_dir, role: [str(perf_dir / "build" / _cpp_target(role))],
        provenance=_cpp_provenance,
        loaded_artifact_markers=("libzlink.so", "libzlink_framework.so"),
        stream_scheme="tcp",
        clientserver_interface="framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:269",
        clientserver_gate="Selectable is populated from ready Client-side target snapshots at "
                          "framework/languages/cpp/framework/src/runtime/client_server/client_server_location_runtime.cpp:383; "
                          "a local Server's readiness is reported in readyServerCount at :420.",
    ),
    "dotnet": Launcher(
        build=lambda perf_dir, role: ["dotnet", "build", str(perf_dir / _dotnet_role(perf_dir, role)), "-c", "Release", "-m:1", "--nologo"],
        command=lambda perf_dir, role: ["dotnet", str(_dotnet_output(perf_dir, role) / (_dotnet_role(perf_dir, role) + ".dll"))],
        provenance=_dotnet_provenance,
        loaded_artifact_markers=("libzlink", "Systems.Zlink", "Zlink.Framework", "ZLink.Framework.Perf", "System.Text.Json"),
        stream_scheme="tcp",
        clientserver_interface="framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:359",
        clientserver_gate="Runtime implementation gates Selectable on HasClient at "
                          "framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs:99.",
    ),
    "java": Launcher(
        build=lambda perf_dir, role: _gradle_install_dist(perf_dir, ":" + _java_module(role)),
        command=lambda perf_dir, role: [str(_java_install(perf_dir, role) / "bin" / _java_module(role))],
        provenance=lambda perf_dir, roles: _java_provenance("java", perf_dir, roles),
        loaded_artifact_markers=("libzlink.so", ".jar"),
        stream_scheme="tcp",
        clientserver_interface="framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.ko.md:227",
        clientserver_gate="Runtime implementation gates ready on hostServing && readyTargetCount > 0 at "
                          "framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkTopologyRuntimeViews.java:71.",
    ),
    "kotlin": Launcher(
        build=lambda perf_dir, role: _gradle_install_dist(perf_dir.parent, ":kotlin:" + _java_module(role)),
        command=lambda perf_dir, role: [str(_java_install(perf_dir, role) / "bin" / _java_module(role))],
        provenance=lambda perf_dir, roles: _java_provenance("kotlin", perf_dir, roles),
        loaded_artifact_markers=("libzlink.so", ".jar"),
        stream_scheme="tcp",
        clientserver_interface="framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md:39",
        clientserver_gate="Runtime implementation gates ready on hostServing && readyTargetCount > 0 at "
                          "framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkTopologyRuntimeViews.java:71.",
    ),
}


def launcher(language: str) -> Launcher:
    if language not in LAUNCHERS:
        raise ValueError(f"No perf launcher for language '{language}' yet; available: {', '.join(sorted(LAUNCHERS))}")
    return LAUNCHERS[language]
