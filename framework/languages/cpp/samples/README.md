[English](./README.md) | [한국어](./README.ko.md)

# ZLink C++ Framework Samples

The seven C++ samples show how several server roles are composed from the framework's public
API. The business flows and their acceptance criteria follow the common sample document
(`framework/doc/framework/common/sample/README.ko.md`); the C++ code registers handlers with
compile-time types instead of runtime reflection.

Each server executable configures its own role only. A runner starts the per-role processes,
waits for readiness, executes the public client scenario, and on exit cleans up the processes
and the Redis container it started. Sample code never starts another server role in the same
process.

This directory closes on itself: the procedure below uses only the Core, binding and framework
archives published on GitHub Releases, vcpkg, and Docker for Redis.

## Contents

1. [Prerequisites](#1-prerequisites)
2. [Download and install](#2-download-and-install)
3. [Build](#3-build)
4. [Run](#4-run)
5. [Verify](#5-verify)
6. [Troubleshooting](#6-troubleshooting)
7. [Sample list](#7-sample-list)
8. [Configuration and contract layout](#8-configuration-and-contract-layout)

## 1. Prerequisites

| Tool | Windows | Linux / WSL |
|---|---|---|
| C++20 compiler | Visual Studio 2022 17.4 or later with the **Desktop development with C++** workload (verified with MSVC 19.44) | GCC 13 or later (verified with 13.3) |
| CMake | 3.24 or later (the 3.31 Visual Studio installs was used) | 3.24 or later (3.28 was used) |
| Ninja | not needed | recommended; Makefiles are used when it is absent |
| vcpkg | the copy Visual Studio installs is found automatically; for a separate clone set `VCPKG_ROOT` | `git clone https://github.com/microsoft/vcpkg`, `./bootstrap-vcpkg.sh`, then set `VCPKG_ROOT` |
| Docker Desktop | each runner starts one Redis container. Must be installed and running | same (WSL integration, or Docker Engine on Linux) |
| `curl` | included since Windows 10 | distribution package |

Nothing else is needed: no zlink repository, no Python, no Node.js. Even ZoneWorld's ZW-B8 fault
proxy is C++ built with the sample. vcpkg builds the third-party libraries from source, so
**the first install takes about 20 minutes**; later installs finish in minutes from vcpkg's
binary cache. The third-party versions are pinned with a vcpkg `builtin-baseline`; a clone
older than that commit needs `git -C $VCPKG_ROOT pull`.

## 2. Download and install

Download
[`zlink-samples-cpp.zip`](https://github.com/zlink-systems/zlink/releases/latest/download/zlink-samples-cpp.zip)
and unpack it. Every command below runs inside the unpacked `zlink-samples-cpp/`.

One script, `bootstrap.cmake`, does the whole install. It downloads three GitHub Release
assets -- this platform's Core prebuilt (`core/v1.2.0`), the C++ binding source (`cpp/v1.2.0`)
and the framework source (`framework-cpp/v0.18.0`) -- builds the binding and the framework into
`.zlink/install/`, and configures the seven samples as one project into `build/`.

Windows PowerShell:

```powershell
cmake -P bootstrap.cmake
```

Linux / WSL bash:

```bash
export VCPKG_ROOT=$HOME/vcpkg
cmake -P bootstrap.cmake
```

Parallelism defaults to the logical core count; lower it with `-DZLINK_JOBS=4` placed before
`-P`. To start over, delete `.zlink/` and `build/`.

## 3. Build

Windows PowerShell:

```powershell
cmake --build build --config Release --parallel
```

Linux / WSL bash:

```bash
cmake --build build --parallel
```

28 role executables and the ZoneWorld proxy come out -- under `build\Release\` on Windows,
`build/` on Linux. On Windows the Core `zlink.dll` and the third-party DLLs are copied next to
the executables. The Linux runners rebuild their own sample's targets before running, so this
step can be skipped when only one sample is of interest.

## 4. Run

Every sample has a `run_sample.ps1` and a `run_sample.sh`; one invocation runs one sample.
**The runner starts Redis itself as a Docker container** (`redis:7-alpine`, a free port in
20000-20099 on `127.0.0.1`) and removes it at the end. Nothing has to be started beforehand.

Windows PowerShell:

```powershell
.\Bingo\run_sample.ps1
.\DeliveryDispatch\run_sample.ps1
.\GameQuest\run_sample.ps1
.\ShoppingMall\run_sample.ps1
.\SupportChat\run_sample.ps1
.\TicTacToe\run_sample.ps1
.\ZoneWorld\run_sample.ps1
```

Linux / WSL bash:

```bash
./Bingo/run_sample.sh
./DeliveryDispatch/run_sample.sh
./GameQuest/run_sample.sh
./ShoppingMall/run_sample.sh
./SupportChat/run_sample.sh
./TicTacToe/run_sample.sh
./ZoneWorld/run_sample.sh
```

Run them one at a time. A runner performs build, per-role configuration files, server start,
readiness checks, the client self-check and cleanup, in that order. Application ports are
chosen per run from 20100-21999 on `127.0.0.1`.

Set `ZLINK_CPP_BUILD_DIR` to use the executables of another build tree instead of `build/`.

## 5. Verify

A sample passes when the runner's last line is the marker below and its exit code is 0. The
items the client self-check confirmed precede it as `<sample>-...=verified` lines.

| Sample | Last line |
|---|---|
| Bingo | `bingo-placement=completed` |
| DeliveryDispatch | `deliverydispatch-placement=completed` |
| GameQuest | `gamequest-placement=completed` |
| ShoppingMall | `shoppingmall-placement=completed` |
| SupportChat | `supportchat-placement=completed` |
| TicTacToe | `tictactoe-placement=completed` |
| ZoneWorld | `zoneworld=completed` |

A failed run keeps its run directory with the per-role stdout/stderr logs and prints its path as
`<sample> run directory preserved: ...`. The Linux runners report the framework's own test stage
as `framework tests: skipped (package tree; no framework test targets)` -- those tests exist
only in the repository tree.

## 6. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `bootstrap: vcpkg was not found. Set VCPKG_ROOT ...` | No vcpkg. On Windows enable the **vcpkg package manager** component in the Visual Studio Installer; on either platform point `VCPKG_ROOT` at a vcpkg clone |
| `error: no version database entry for <port> at <version>`, or another baseline error | The vcpkg clone predates the baseline commit. `git -C $VCPKG_ROOT pull`, then rerun |
| `bootstrap: download failed: https://github.com/...` | GitHub Releases is unreachable; check proxy and firewall, then rerun |
| `CMake Error ... No CMAKE_CXX_COMPILER could be found` / `Visual Studio 17 2022 could not find any instance` | No compiler. Install the **Desktop development with C++** workload on Windows, `g++` on Linux |
| `No configured build tree at .../build.` (Linux) / `Missing executable: ... Build C++ samples first or set ZLINK_CPP_BUILD_DIR.` (Windows) | Install or build was skipped. Run `cmake -P bootstrap.cmake`, and on Windows `cmake --build build --config Release` |
| On Windows a role process exits at once with an empty log and the runner ends with `Timed out waiting for <role>` (exit code `-1073741515`, `STATUS_DLL_NOT_FOUND`) | `zlink.dll` is not beside the executables. Rerun `cmake --build build --config Release`; the post-build step copies it into `build\Release\` |
| `Docker is required to run the <sample> sample.` / `docker: error during connect` / `Cannot connect to the Docker daemon` | Docker Desktop is not running. Start it and rerun |
| `Failed to create Redis container ...` containing `port is already allocated` | All of 20000-20099 are taken. Find `zlink-redis-cpp-sample-*` containers left by earlier runs with `docker ps` and remove them |
| `<sample> sample startup port collision; retrying with fresh ports` | Another process took a chosen port first. The runner retries with new ports up to 3 times; nothing to do |
| `Timed out waiting for <role> at tcp://127.0.0.1:<port>` | That role did not come up. Read `<role>.trace.log` (or `<role>.log`) in the preserved run directory |
| On Windows, `run_sample.ps1 cannot be loaded because running scripts is disabled` | Execution policy. Run `powershell -ExecutionPolicy Bypass -File .\TicTacToe\run_sample.ps1` |

## 7. Sample list

| Sample | What it shows | Connection layout | Payload codec |
|---|---|---|---|
| `Bingo` | Session gateway, Entry Spot, room Spot, Actor binding, timers and bound-session push | Redis location store | Protobuf |
| `TicTacToe` | Scale-out of two API and two Play nodes, room route lookup and a live game | Manual peer endpoints and a Redis room route store | JSON |
| `SupportChat` | Conversation Spot, agent assignment, reconnect, idle timer and close notice | Redis location store | JSON |
| `DeliveryDispatch` | Courier selection, timeout reassignment, tracking and customer push | Redis location store | JSON |
| `GameQuest` | Per-player quest owner Spot, event stream and read model | Redis location store | JSON |
| `ShoppingMall` | ChannelName service, order workflow, event stream and fanout notices | Redis location store | JSON |
| `ZoneWorld` | Gateway, two ZoneNodes and Ops as separate roles: zone moves, actor relocation, border sync and operations fanout | Redis location store | JSON |

Only TicTacToe configures peer endpoints by hand. The other samples use the Redis location store
to find Spots and Actors and to compose MeshNode peers; application code manages neither peer
lists nor connection order.

`samples/TicTacToe/run_sample.sh` and `samples/Bingo/run_sample.sh` are the full client/server
self-checks: every server role plus the public client scenario in one run. Inside the repository
tree the framework's own tests run before them.

One physical mesh is one MeshNode per process. A `ChannelName` is a logical service group that
MeshNode joins; it opens no extra ROUTER endpoint. Node direct, ChannelName select-one, Spot,
Actor and Logical Multicast share that MeshNode. Classic fanout to every receiver is a separate
PUB/SUB channel.

## 8. Configuration and contract layout

A server role takes a configuration file path, binds what `app.config()` read into typed
configuration and hands it to the framework builder. Endpoints, Redis, routing ids, timeouts
and log paths are never read from application environment variables. A standalone client takes
only the external endpoints it must connect to and its timeouts, through validated CLI options
or a client configuration file.

`Shared/Contracts` holds only the message contracts both client and server serialize. Server
topology, ChannelNames, endpoint names, packet names and timing live in `Server/Configuration`;
client-only settings in `Client/Configuration`.

Running one role by hand also takes its role configuration file.

```bash
sample_cpp_framework_tictactoe_play --config=./appsettings.play-a.json
sample_cpp_framework_tictactoe_api --config=./appsettings.api-a.json
sample_cpp_framework_tictactoe_client --api-http-endpoint=http://127.0.0.1:48113
```

Each sample's `CMakeLists.txt` also configures on its own: give `find_package(zlink_framework
CONFIG REQUIRED)` the `.zlink/install` prefix through `CMAKE_PREFIX_PATH`. The root
`CMakeLists.txt` gathers the seven into one build tree.
