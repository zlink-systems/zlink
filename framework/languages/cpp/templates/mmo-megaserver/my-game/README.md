# MyGame megaserver

This template hosts capacity-limited Region User Spots on two physical nodes in one world mesh. Entry selects the first Region instance that accepts a player. A separate Instance node creates Dungeon User Spots on demand.

```mermaid
flowchart LR
  Players -->|STREAM| Session
  Session -->|Actor relay| RegionA
  RegionA <-->|Region placement and relocation| RegionB
  RegionA -->|Dungeon GetOrCreate| Instance
  RegionA -->|Location / lease| Redis
  RegionB --> Redis
  Session --> Redis
  Instance --> Redis
```

## Prerequisites

Docker with Compose v2 and a running Linux-container daemon is required. Use Bash on Linux or PowerShell 7 on Windows. The Docker build installs the C++ toolchain and CMake; the host does not need them for the default run.

Public Framework 0.32.0 is pinned in `bootstrap.cmake`, the only file that names the version. `cmake -P bootstrap.cmake` downloads this platform's prebuilt from the `framework-cpp/v0.32.0` GitHub Release, checks it against the SHA-256 checksum published with it and extracts it into `.zlink/install`. The prebuilt carries Core, the C++ binding, the Framework libraries, Boost headers and nlohmann_json, so no package manager runs. The Docker build and local mode run the same script. Follow the public package licensing requirements.

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

Linux:

```sh
./run-e2e.sh
./run-e2e.sh --scenario region,movement,dungeon,reconnect,update
./run-e2e.sh --check-domain
```

Windows:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario region,movement,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

The runner and nodes execute in containers. The script checks structure before Redis startup, then uses a unique Compose network, project, container names and Redis key prefixes. Nodes share the runner's network namespace, preserving loopback listeners and OS-assigned ports. Configuration is copied into each node before attaching stdin and stdout; `drain`, `shutdown` and lifecycle output retain their existing protocol.

The runner removes its node containers. The script removes remaining containers carrying this run's label, its Compose resources and its image tags. Cleanup failures return exit code 1. Build and orchestration logs remain in `.logs/docker-<project>/`; node logs and generated configurations remain in `.logs/run-<id>/`. Unix runners use the caller UID and Docker socket group so those files remain caller-owned.

The runner checks `zlink.structure.json` before starting Redis and the four node containers. Each scenario has its own node configuration and Redis key prefix. Redis and Framework listeners use OS-assigned ports. Each deployment has a unique Redis key prefix. Generated configurations, message-flow diagnostics, and process logs remain in `.logs/run-<id>/`. Scenario and cleanup failures both return exit code 1.

| Scenario | Assertions |
| --- | --- |
| `region` | Two Region replicas reach capacity; the next player receives `RegionUnavailable(capacity-exhausted)` and stays connected. An unknown region receives `region-unknown`. Internal packets are rejected by Session. |
| `movement` | Server validates delta and sequence, returns `ActionRejected` without changing state, then accepts after the server-provided retry delay. The requester receives one direct result; other members receive the Region broadcast. |
| `dungeon` | Dungeon GetOrCreate resolves to the Instance node. |
| `reconnect` | A new Session binds the same Actor generation and retains Region membership and position. |
| `update` | Drain the actual Region owner, observe relocation and SafeToShutdown, retain player state and Session identity, stop and replace that owner, and verify movement and new Region work. |

`./run-e2e.sh --check-structure` validates actual files without starting Redis or game nodes. `./run-e2e.sh --check-domain` runs movement boundary checks without Redis or game nodes. The structure checker validates references, configuration paths, stable types and the canonical scenario order.

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

Validated against the published Framework 0.32.0 packages.

This template answers `SessionInfoRequest` with a typed `SessionInfo` push; Framework 0.30.2 added a typed Session reply through `reply_packet` (#1785), and the template has not switched to it.

- [#1786](https://github.com/zlink-systems/zlink/issues/1786) (open, planned for 0.31.0): the first push sent from `on_actor_joined` after a deferred join can be lost without an error; it was not observed in 0.31.0 validation.

## Deployment sequence

Start Redis, the two Region processes, the Instance process, and the Session process. Each process emits `READY` with its actual mesh endpoint; Session also reports its STREAM endpoint. Configure deployment-specific Redis endpoints and key prefixes.

Send `drain` to the Region process being replaced. Require `RELOCATION_REQUESTED`, `RELOCATED`, then `SAFE_TO_SHUTDOWN`. Send `shutdown`, require `STOPPED` and exit code zero, then start the same role with the same configuration. Framework Location Store retains authority for relocated objects. The runner selects the actual owner from RegionView; it does not prescribe placement.

`server/region/domain/positions.hpp` owns capacity, player positions and movement sequence. Entry owns index selection after `region-full`. Region stores player IDs and sends pushes through the public Actor client; its delivery handler forwards to the Actor's bound Session. Request handlers reply through the received Actor's bound Session. Relocation adapters serialize application state only. Player has no application state to duplicate.

The e2e fixture limits each Region node to two Region Spots and keeps forest-1 and forest-2 occupied before drain. After relocation the survivor is full. Once the replacement reports `READY`, a new desert Region must be served by that replacement. Existing Region state and Session identity checks remain required. Only Region nodes register the application Entry and its first admission handler, so only those nodes declare an Entry in `zlink.structure.json`.

Message Follow is explicitly set to 30 seconds. The `SAFE_TO_SHUTDOWN` observation budget is the follow duration plus the existing relocation budget; the relocation operation deadline is unchanged.
