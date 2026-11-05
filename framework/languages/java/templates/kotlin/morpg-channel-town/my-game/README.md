# my-game — MORPG channel town

Players gather in Town channels, form parties of up to four members, and enter a dedicated Dungeon User Spot. Player Actors preserve identity while the Framework moves them between towns and dungeons. Each Spot owns its game state; the Framework owns placement, membership, binding, and relocation.

```mermaid
flowchart LR
    players[Players] -->|STREAM| session[Session]
    session -->|relay| townA[Town A: town-1]
    townA -->|relocation| townB[Town B: town-2]
    townB -->|GetOrCreate| instances[Instance nodes A and B: Dungeon]
    townB -->|Location / lease| location[Redis]
```

## Prerequisites

Docker with Compose v2 and Linux containers, network access to Maven Central and the Gradle Plugin Portal, and PowerShell 7 on Windows. JDK 25 is required only for local process debugging. The Gradle wrapper is included; public package versions are pinned in `build.gradle.kts`.

## Build

Bash:

```sh
./run-e2e.sh --build-only
```

PowerShell:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
```

## Docker e2e

Docker builds a shared node image for all roles and a runner image. The pinned Eclipse Temurin JDK/JRE bases are defined by the Dockerfile ARGs. The runner uses the Docker socket to create sibling nodes sharing its network namespace, preserving loopback listeners and stdin/stdout control. Each invocation removes its containers, network and image tags; cleanup errors return exit 1. Logs remain in `.logs/`.

For local process debugging, run `./run-e2e.sh --local` or `pwsh -NoProfile -File ./run-e2e.ps1 -Local`. Local mode uses JDK 25 and Compose Redis. Scenario and structure options work in both modes.

```sh
./run-e2e.sh
./run-e2e.sh --scenario channel,dungeon,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario channel,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

The runner checks the structure, starts a unique Compose project with Redis on an OS-assigned loopback port, starts the application processes, and removes its resources. Run-specific configurations and stdout, stderr, and message-flow records are in `.logs/run-<id>/`. Use `--check-structure` or `-CheckStructure` for structure validation only. Use `--check-domain` or `-CheckDomain` to run the domain checks without starting nodes.

| Scenario | Verification |
| --- | --- |
| `channel` | Session continuity while changing towns; invalid channel rejection leaves state unchanged; a forming party leaves with its member. |
| `dungeon` | Server-issued PartyId and RunId; duplicate entry sends; identical active Dungeon views; nonleader and internal packet rejection leave state unchanged; FinalStage completion; distinct dungeons for parties in town-1 and town-2. |
| `reconnect` | Actor identity and Dungeon RunId, Phase, Stage, and roster after reconnect. |
| `update` | Drain completion, preserved Dungeon state and Session identity, and a replacement instance accepting a distinct party run from the other channel. |

## Known issues

Validated against the published Framework 0.32.0 packages.

## Deployment sequence

Bootstrap Town providers sequentially and wait for each READY before starting the next. Each provider's public Town stable-type capacity must equal its configured fixed Town count; the structure checker enforces this. Earlier providers are full and later providers are absent, making each fixed Town's declared initial owner deterministic. Entry declarations describe the application Entry registered by Town and Instance, excluding Framework default Entries.

1. Start Redis and supply an independent key prefix for each deployment.
2. Start townA and townB using `server/config/townA.json` and `townB.json`; each bootstraps its configured fixed Town. Start instanceA and instanceB. Roles have separate `MyGame.town.ProgramKt` and `MyGame.instance.ProgramKt` entry points.
3. Start `MyGame.session.ProgramKt` with `server/config/session.json`. Nodes publish `READY` with their actual listener endpoints. Mesh and STREAM listeners bind port zero.
4. Send `drain` on stdin to the instance being updated. Wait for `RELOCATION_REQUESTED`, `RELOCATED`, and `SAFE_TO_SHUTDOWN`. Send `shutdown`, wait for `STOPPED`, and start the replacement with the same configuration. Each sample instance has one Dungeon slot so the update scenario can verify replacement placement.

Application distributions are under `server/<role>/build/install/<role>/`. The e2e scripts build them before starting nodes. Deployment must supply durable Redis storage and reachable listener addresses appropriate to the network.

All Object Servers that register Player factories also register the shared Entry and first-admission handler, including both Instance nodes. Entry admission joins the requested fixed Town channel. Each Instance declares its dynamic Dungeon factory in `zlink.structure.json`.

The e2e configuration uses one Dungeon slot per Instance. After drain, the surviving Instance is full, so only the replacement can accept the fresh Dungeon. The structure checker enforces this configuration. Message Follow remains 30 seconds; the SafeToShutdown observation budget is derived from that duration plus the existing relocation operation budget. Party members can leave a Forming party when changing towns; run rosters remain recorded after departure.
