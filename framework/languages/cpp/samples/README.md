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

This directory is `samples/` in the `zlink-cpp-examples` repository. The procedure below uses
the Core, binding and framework packages published on GitHub Releases, Conan, and Docker for Redis.

## Contents

- [Prerequisites](#prerequisites)
- [Download and install](#download-and-install)
- [Build](#build)
- [Run](#run)
- [Verify](#verify)
- [Troubleshooting](#troubleshooting)
- [Sample list](#sample-list)
- [Configuration and contract layout](#configuration-and-contract-layout)

The command blocks of `Build`, `Run` and `Verify` are marked `title="linux"` (bash) and
`title="windows"` (PowerShell). Each block runs as is from `samples/` after cloning the
`zlink-cpp-examples` repository, and the release CI runs the same blocks verbatim.

## Prerequisites

Bash blocks run on Linux, macOS, and WSL; PowerShell blocks run on Windows PowerShell 7. `cmd` is not supported.

| Tool | Windows | Linux / WSL |
|---|---|---|
| C++20 compiler | Visual Studio 2022 17.4 or later with the **Desktop development with C++** workload (verified with MSVC 19.44) | GCC 13 or later (verified with 13.3) |
| CMake | 3.24 or later (the 3.31 Visual Studio installs was used) | 3.24 or later (3.28 was used) |
| Ninja | not needed | recommended; Makefiles are used when it is absent |
| Conan 2 | `pipx install conan` (or `py -m pip install --user conan`) | `pipx install conan` (or `python3 -m pip install --user conan`) |
| Docker Desktop | each runner starts one Redis container. Must be installed and running | same (WSL integration, or Docker Engine on Linux) |
| `curl` | included since Windows 10 | distribution package |

Nothing else is needed: no zlink repository or Node.js. Even ZoneWorld's ZW-B8 fault proxy is C++
built with the sample. Conan is installed by pipx (or pip) and downloads ConanCenter binaries for
the third-party libraries, so **the first install takes about 3 minutes** on a supported compiler;
later installs reuse its local cache.

## Download and install

Clone the [`zlink-cpp-examples`](https://github.com/zlink-systems/zlink-cpp-examples) repository.
Every command below runs inside its `samples/` directory.

One script, `bootstrap.cmake`, does the install -- it is the first line of the [Build](#build)
block. It downloads three GitHub Release assets -- this platform's Core prebuilt
(`core/v1.2.0`), the C++ binding source (`cpp/v1.2.0`) and the framework source
(`framework-cpp/v0.18.0`) -- builds the binding and the framework into `.zlink/install/`, and
configures the seven samples as one project into `build/`. Conan is the default package manager;
pass `-DZLINK_PACKAGE_MANAGER=vcpkg` for the vcpkg fallback. From the second run on it reuses what
it downloaded and built.

Parallelism defaults to the logical core count; lower it as `cmake -DZLINK_JOBS=4 -P
bootstrap.cmake`. To start over, delete `.zlink/` and `build/`.

## Build

**Linux · macOS · WSL — bash**

```bash title="linux"
cmake -P bootstrap.cmake
cmake --build build --parallel
```

**Windows — PowerShell 7**

```powershell title="windows"
cmake -P bootstrap.cmake
cmake --build build --config Release --parallel
```

28 role executables and the ZoneWorld proxy come out -- under `build\Release\` on Windows,
`build/` on Linux. On Windows the Core `zlink.dll` and the third-party DLLs are copied next to
the executables. The Linux runners rebuild their own sample's targets before running, so
`cmake --build` can be skipped when only one sample is of interest.

## Run

Every sample has a `run_sample.ps1` and a `run_sample.sh`; one invocation runs one sample.
**The runner starts Redis itself as a Docker container** (`redis:7-alpine`, a free port in
20000-20099 on `127.0.0.1`) and removes it at the end. Nothing has to be started beforehand.
The block below runs the seven in turn.

**Linux · macOS · WSL — bash**

```bash title="linux"
./Bingo/run_sample.sh
./DeliveryDispatch/run_sample.sh
./GameQuest/run_sample.sh
./ShoppingMall/run_sample.sh
./SupportChat/run_sample.sh
./TicTacToe/run_sample.sh
./ZoneWorld/run_sample.sh
```

**Windows — PowerShell 7**

```powershell title="windows"
.\Bingo\run_sample.ps1
.\DeliveryDispatch\run_sample.ps1
.\GameQuest\run_sample.ps1
.\ShoppingMall\run_sample.ps1
.\SupportChat\run_sample.ps1
.\TicTacToe\run_sample.ps1
.\ZoneWorld\run_sample.ps1
```

Run them one at a time. A runner performs build, per-role configuration files, server start,
readiness checks, the client self-check and cleanup, in that order. Application ports are
chosen per run from 20100-21999 on `127.0.0.1`.

Set `ZLINK_CPP_BUILD_DIR` to use the executables of another build tree instead of `build/`.

## Verify

Examples smoke runs this block exactly as written.

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

The block below checks this with TicTacToe alone.

**Linux · macOS · WSL — bash**

```bash title="linux"
./TicTacToe/run_sample.sh | tee tictactoe.log | tail -n 1 | grep -x 'tictactoe-placement=completed'
echo "tictactoe=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
$lines = @(& .\TicTacToe\run_sample.ps1 *>&1 | ForEach-Object { "$_" })
if ($lines[-1] -ne 'tictactoe-placement=completed') { throw ('TicTacToe failed: ' + $lines[-1]) }
Write-Output $lines[-1]
Write-Output 'tictactoe=ok'
```

A failed run keeps its run directory with the per-role stdout/stderr logs and prints its path as
`<sample> run directory preserved: ...`. The Linux runners report the framework's own test stage
as `framework tests: skipped (package tree; no framework test targets)` -- those tests exist
only in the repository tree.

## Running from an IDE

`bootstrap.cmake` records the exact arguments it configured this folder with as the preset `zlink`
in `CMakeUserPresets.json`. Visual Studio, Rider and CLion read that preset when they open the
folder, so after one bootstrap the IDE continues in the same `build/`. The bootstrap rewrites the
file every time; it is not edited by hand.

A sample is several processes plus Redis, orchestrated by `run_sample.sh` (port choice, generated
configuration files, shutdown order). In the IDE you therefore **build, browse and debug**; running
and verifying stay with the runner in the [Run](#run) section.

1. Run `cmake -P bootstrap.cmake` from the [Build](#build) section once in a terminal (an IDE cannot
   run a `-P` script).
2. **Visual Studio 2022 or 2026**: File › Open › Folder on this directory and pick the preset `zlink`
   in the CMake settings. **Rider or CLion**: open this directory's `CMakeLists.txt` as the project
   and enable the preset `zlink` under Settings › Build, Execution, Deployment › CMake. The sample
   targets appear as build targets.
3. Debug by attaching the IDE to a process the runner started; the runner finds the executables the
   IDE built in the same `build/`.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `bootstrap: could not find Conan` | Install Conan 2 with `pipx install conan` (or `python3 -m pip install --user conan`) and ensure its bin directory is on `PATH` |
| `ERROR: Invalid setting ...` | The selected compiler is not a supported ConanCenter binary configuration. Use the listed compiler version, or pass `-DZLINK_PACKAGE_MANAGER=vcpkg` |
| `bootstrap: download failed: https://github.com/...` | GitHub Releases is unreachable; check proxy and firewall, then rerun |
| `CMake Error ... No CMAKE_CXX_COMPILER could be found` / `Visual Studio 17 2022 could not find any instance` | No compiler. Install the **Desktop development with C++** workload on Windows, `g++` on Linux |
| `No configured build tree at .../build.` (Linux) / `Missing executable: ... Build C++ samples first or set ZLINK_CPP_BUILD_DIR.` (Windows) | Install or build was skipped. Run `cmake -P bootstrap.cmake`, and on Windows `cmake --build build --config Release` |
| On Windows a role process exits at once with an empty log and the runner ends with `Timed out waiting for <role>` (exit code `-1073741515`, `STATUS_DLL_NOT_FOUND`) | `zlink.dll` is not beside the executables. Rerun `cmake --build build --config Release`; the post-build step copies it into `build\Release\` |
| `Docker is required to run the <sample> sample.` / `docker: error during connect` / `Cannot connect to the Docker daemon` | Docker Desktop is not running. Start it and rerun |
| `Failed to create Redis container ...` containing `port is already allocated` | All of 20000-20099 are taken. Find `zlink-redis-cpp-sample-*` containers left by earlier runs with `docker ps` and remove them |
| `<sample> sample startup port collision; retrying with fresh ports` | Another process took a chosen port first. The runner retries with new ports up to 3 times; nothing to do |
| `Timed out waiting for <role> at tcp://127.0.0.1:<port>` | That role did not come up. Read `<role>.trace.log` (or `<role>.log`) in the preserved run directory |
| On Windows, `run_sample.ps1 cannot be loaded because running scripts is disabled` | Execution policy. Run `powershell -ExecutionPolicy Bypass -File .\TicTacToe\run_sample.ps1` |

## Sample list

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

## Configuration and contract layout

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
