# my-game — MORPG lobby and dungeon instances

A Kotlin server template using the public ZLink Framework coroutine APIs. Players create a party in the lobby, mark every member ready, and enter a shared dungeon. The Party User Spot owns the roster and readiness; the Dungeon User Spot owns the dungeon stage. The Entry Spot owns no party state.

```mermaid
flowchart LR
    players -->|STREAM| session
    session -->|relay| lobby
    lobby -->|GetOrCreate| instanceA
    instanceA <-->|relocation| instanceB
    lobby -->|Location / lease| location[(Redis)]
```

The Lobby and Instance roles (`lobby`, `instanceA`, and `instanceB`) explicitly register the shared stateless application Entry and initial party-admission handlers. The structure declares Entry only for those application registrations; Framework infrastructure Entry instances are not declared. Party state remains in the Lobby Party User Spot. Message Follow is explicitly 30 seconds; the safe-shutdown observation budget adds that duration to the existing host-operation budget. Each instance admits one Dungeon, so the relocated Dungeon fills the surviving instance and fresh work must use the replacement.

## Prerequisites

Docker with Compose v2 and Linux containers, network access to Maven Central and the Gradle Plugin Portal, and PowerShell 7 on Windows. JDK 25 is required only for local process debugging. The Gradle wrapper is included; public package versions are pinned in `build.gradle.kts`.

## Build

```sh
./run-e2e.sh --build-only
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
```

## Docker e2e

Docker builds a shared node image for all roles and a runner image. The pinned Eclipse Temurin JDK/JRE bases are defined by the Dockerfile ARGs. The runner uses the Docker socket to create sibling nodes sharing its network namespace, preserving loopback listeners and stdin/stdout control. Each invocation removes its containers, network and image tags; cleanup errors return exit 1. Logs remain in `.logs/`.

For local process debugging, run `./run-e2e.sh --local` or `pwsh -NoProfile -File ./run-e2e.ps1 -Local`. Local mode uses JDK 25 and Compose Redis. Scenario and structure options work in both modes.

```sh
./run-e2e.sh
./run-e2e.sh --scenario party,reconnect,update
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario party,reconnect,update
```

The runner validates `zlink.structure.json`, starts its own Redis Compose project and node processes, then removes those resources. Each scenario has isolated nodes, a unique Redis key prefix, ephemeral listener ports, and logs in `.logs/run-<id>/`. Cleanup failure produces a failing exit code.

| Scenario | Assertions |
| --- | --- |
| party | Server-issued IDs; invalid-party, not-ready and non-leader rejection without state changes; internal packet rejection; back-to-back entry reaches one active run; distinct parties get distinct Dungeons; FinalStage completes the run |
| reconnect | Same Actor, Dungeon, RunId, Phase and Stage after reconnect; advancement and completion continue |
| update | Drain and safe-shutdown observations; preserved Sessions, RunId, Phase and Stage; replacement accepts a distinct party and Dungeon |

Run the structure checker alone with `./run-e2e.sh --check-structure` or `pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure`. Party domain tests run with `./gradlew --no-daemon :server:lobby:test`.

## Known issues

Validated against the published Framework 0.31.0 packages.

## Deployment sequence

1. Provision Redis, then start `lobby`, the two `instance` processes, and `session` with their respective files in `server/config/`. The application entry points are `MyGame.lobby.ProgramKt`, `MyGame.instance.ProgramKt`, and `MyGame.session.ProgramKt`. The two instances use the same executable and distinct configuration files.
2. Wait for `READY` from each process. Its JSON contains the actual bound mesh and stream endpoints; a port of zero delegates port selection to the OS.
3. Send `drain` to an occupied instance. Wait for `RELOCATION_REQUESTED`, `RELOCATED`, and `SAFE_TO_SHUTDOWN` before sending `shutdown`.
4. Confirm `STOPPED` and exit code zero. Start a replacement with the same configuration and wait for `READY` before assigning new work.

Instance capacity is one Dungeon per process to make replacement placement observable. Framework owns placement, membership, route continuity, and relocation. Application adapters capture only Party and Dungeon game state; Player has no application state. Non-request pushes use player IDs through Actor delivery handlers, without retaining Actor references in Spots.
