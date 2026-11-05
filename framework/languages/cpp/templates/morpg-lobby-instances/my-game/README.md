# my-game

A C++ MORPG server template with a stateless lobby entry, dynamic Party User Spots and Dungeon instances. Parties contain up to four players. The leader enters a Dungeon after every member is ready. Dungeon state survives reconnect and planned host relocation.

```mermaid
flowchart LR
  players -->|STREAM| session
  session -->|relay| lobby
  lobby -->|GetOrCreate| instanceA
  lobby -->|Location / lease| location[(Redis)]
  instanceA <-->|host relocation| instanceB
```

The Lobby and Instance roles (`lobby`, `instanceA`, and `instanceB`) explicitly register the shared stateless application Entry and initial party-admission handlers. The structure declares Entry only for those application registrations; Framework infrastructure Entry instances are not declared. Party state remains in the Lobby Party User Spot. Message Follow is explicitly 30 seconds; the safe-shutdown observation budget adds that duration to the existing host-operation budget. Each instance admits one Dungeon, so the relocated Dungeon fills the surviving instance and fresh work must use the replacement.

## Prerequisites

Docker with Compose v2 and a running Linux-container daemon is required. Use Bash on Linux or PowerShell 7 on Windows. The Docker build installs the C++ toolchain and CMake; the host does not need them for the default run.

Public Framework 0.31.0 is pinned in `bootstrap.cmake`, the only file that names the version. `cmake -P bootstrap.cmake` downloads this platform's prebuilt from the `framework-cpp/v0.31.0` GitHub Release, checks it against the SHA-256 checksum published with it and extracts it into `.zlink/install`. The prebuilt carries Core, the C++ binding, the Framework libraries, Boost headers and nlohmann_json, so no package manager runs. The Docker build and local mode run the same script. Follow the public package licensing requirements.

## Build

Build the node and runner images from this directory:

```sh
./run-e2e.sh --build-only
```

The script removes its unique image tags after checking the build. Docker retains build layers for subsequent runs.

On Windows, use PowerShell 7:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
```

`Dockerfile` defines the Ubuntu 24.04 build and runtime base once in `UBUNTU_IMAGE`. All node roles share the multi-stage image. The runner adds Docker CLI 28.5.1 to the same runtime. These base arguments can be replaced when the deployment base image becomes available.

## Docker e2e

```sh
./run-e2e.sh
./run-e2e.sh --scenario party,reconnect,update
./run-e2e.sh --check-structure
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario party,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

The runner and nodes execute in containers. The script checks structure before Redis startup, then uses a unique Compose network, project, container names and Redis key prefixes. Nodes share the runner's network namespace, preserving loopback listeners and OS-assigned ports. Configuration is copied into each node before attaching stdin and stdout; `drain`, `shutdown` and lifecycle output retain their existing protocol.

The runner removes its node containers. The script removes remaining containers carrying this run's label, its Compose resources and its image tags. Cleanup failures return exit code 1. Build and orchestration logs remain in `.logs/docker-<project>/`; node logs and generated configurations remain in `.logs/run-<id>/`. Unix runners use the caller UID and Docker socket group so those files remain caller-owned.

The runner checks the structure. The script starts Redis under a unique Compose project. Each selected scenario starts lobby, instanceA, instanceB and session with a distinct Redis key prefix. Each instance admits one Dungeon, so replacement verification requires new work to use the replacement instance while the surviving instance retains the relocated Dungeon. Listener and Redis ports are assigned by the OS. Run configurations and message-flow diagnostics remain in `.logs/run-<id>/`. Cleanup errors fail the run.

| Scenario | Assertions |
| --- | --- |
| party | Server-issued IDs; not-ready and non-leader rejection without state changes; internal packet rejection; back-to-back entry reaches one active run; distinct parties get distinct Dungeons; FinalStage completes the run |
| reconnect | Same Actor, Dungeon, RunId, Phase and Stage after reconnect; advancement and completion continue |
| update | Drain and safe-shutdown observations; preserved Sessions, RunId, Phase and Stage; replacement accepts a distinct party and Dungeon |

## Local debugging

Local processes remain available for debugging. Install a C++20 compiler (GCC 13 or later, or Visual Studio 2022 17.4 or later), CMake 3.24+ and Ninja. Windows requires a Visual Studio 2022 developer shell. No ZLink checkout or package manager is needed.

Build and run local processes from this directory:

```sh
./run-e2e.sh --local
```

The script runs `cmake -P bootstrap.cmake`, `cmake --preset default` and `cmake --build --preset default` before executing `build/bin/my-game-e2e`. Redis still runs in an isolated Compose project.

On Windows, run:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -Local
```

Use `--no-build` or `-NoBuild` with local mode to reuse a debugger build. Scenario selection works in both modes. Local build-only uses `--local --build-only` or `-Local -BuildOnly`.

## Known issues

Validated against the published Framework 0.31.0 packages.

This template answers `SessionInfoRequest` with a typed `SessionInfo` push; Framework 0.30.2 added a typed Session reply through `reply_packet` (#1785), and the template has not switched to it.

## Deployment sequence

1. Provision Redis and configure a shared unique KeyPrefix and RedisEndpoint for this deployment.
2. Start the lobby executable with `server/config/lobby.json` and the instance executable separately with each instance configuration. Wait for their READY lines.
3. Start the session executable with `server/config/session.json`. Use its published streamEndpoint for clients.
4. For an instance update, write `drain` to its standard input and wait for RELOCATION_REQUESTED, RELOCATED and SAFE_TO_SHUTDOWN. Keep a compatible target instance available.
5. Write `shutdown`, require STOPPED and a zero process exit, then start the replacement with the same configuration and require READY before new work.
