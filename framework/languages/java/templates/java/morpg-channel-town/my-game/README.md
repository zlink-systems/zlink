# my-game

A Java MORPG template with two fixed town channels and dynamic party dungeons. Town owns channel membership and parties. Dungeon owns its prepared roster and stage; Entry owns no game state.

```mermaid
flowchart LR
  players -->|STREAM| session
  session -->|relay| townA
  townA -->|relocation| townB
  townB -->|GetOrCreate| instanceA
  instanceA <-->|relocation| instanceB
  townB -->|Location / lease| location[(Redis)]
```

## Prerequisites

Docker with Compose v2, and PowerShell 7 on Windows. Gradle Wrapper is included. Public Framework packages are pinned to 0.30.2 and the binding to 1.20.0 in `build.gradle.kts`. Framework 0.30.2 must be available in Maven Central before these commands can resolve it.

## Build

```sh
./run-e2e.sh --build-only
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
```

## Docker e2e

Docker runs the Java e2e runner and every node role in containers. Nodes share the runner's network namespace and keep the existing stdin/stdout control protocol. Only the runner mounts the Docker socket. Each invocation removes its own containers, network and image tags; logs remain under `.logs/`.

For local process debugging, install JDK 25 and use `./run-e2e.sh --local` or `pwsh -NoProfile -File ./run-e2e.ps1 -Local`. Add `--no-build` or `-NoBuild` to reuse a local build. Docker uses its image layer cache.

> **Base images.** Dockerfile ARG defaults pin Eclipse Temurin JDK/JRE 25.0.2+10 and Docker CLI 28.5.1. Replace these defaults when adopting a deployment base image.


```sh
./run-e2e.sh
./run-e2e.sh --scenario channel,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario channel,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

The runner validates `zlink.structure.json`, creates one isolated Redis Compose project per scenario, launches the node roles, and retains logs and effective configuration under `.logs/run-<id>/`. Listeners bind port zero; Redis uses an ephemeral loopback port. Cleanup failures fail the run.

- `channel`: move from town-1 on townA to town-2 on townB while retaining the connection and Session binding.
- `dungeon`: create and join a two-player party in town-2, enter the same dungeon at stage zero, and advance together.
- `reconnect`: reconnect the second player; check the same Actor generation, dungeon, roster, and stage with a new Session.
- `update`: drain the actual dungeon host, observe relocation and safe shutdown, retain both Sessions and game state, replace the host, and verify a new dungeon uses its free capacity.

For structure checks only, pass `--check-structure` to the shell runner or `-CheckStructure` to PowerShell. Checks examine real files and permit additional valid composition declarations.

## Known issues

Public 0.30.0 validation on Windows stopped before the first TownView: the Session's Actor GetOrCreate remained pending and no destination node recorded the request, matching the reported symptom of [#1782](https://github.com/zlink-systems/zlink/issues/1782). Linux reached cross-node deferred JoinSpot and failed with DATA_LOST, matching [#1729](https://github.com/zlink-systems/zlink/issues/1729). The pinned 0.30.2 candidate requires validation. No runtime workaround is included.

## Deployment sequence

Bootstrap Town providers sequentially and wait for each READY before starting the next. Each provider's public Town stable-type capacity must equal its configured fixed Town count; the structure checker enforces this. Earlier providers are full and later providers are absent, making each fixed Town's declared initial owner deterministic. Entry declarations describe the application Entry registered by Town and Instance, excluding Framework default Entries.

Start Redis, townA, townB, both instance nodes, and then the Session node. Wait for each node to print READY before starting the next; each town bootstraps its configured fixed Town Spot before READY. Configure unique node IDs and Redis key prefixes in `server/config/`. Start each role using its installed distribution under `server/<role>/build/install/<role>/bin/`, supplying the corresponding configuration path. Instance nodes register `Dungeon` with capacity one and the shared application Entry.

Each process prints `READY` with actual listener endpoints. Send `drain` on standard input to request planned maintenance. Wait for `RELOCATION_REQUESTED`, `RELOCATED`, and `SAFE_TO_SHUTDOWN`; send `shutdown` and require `STOPPED` before replacing the process with the same configuration. Town and Dungeon adapters preserve application state only; Framework owns memberships, routes, leases, queues, and relocation progress.

The first party member is the leader. A party accepts at most four distinct players from its town channel. Only the leader can enter the dungeon. Dungeon preparation fixes the party roster before travel. Pushes use Actor IDs, and Actor delivery handlers forward the typed payload to the bound Session. `SessionInfo` reads Actor identity from the Session binding rather than game state.

All Object Servers that register Player factories also register the shared Entry and first-admission handler, including both Instance nodes. Entry admission joins the requested fixed Town channel. Each Instance declares its dynamic Dungeon factory in `zlink.structure.json`.

The e2e configuration uses one Dungeon slot per Instance. After drain, the surviving Instance is full, so only the replacement can accept the fresh Dungeon. The structure checker enforces this configuration. Message Follow remains 30 seconds; the SafeToShutdown observation budget is derived from that duration plus the existing relocation operation budget.

## 한국어 실행 안내

기본 e2e는 runner와 node를 Docker에서 실행합니다. 로컬 process 디버깅에는 JDK 25를 준비하고 `--local` 또는 `-Local`을 지정합니다.

Docker Compose v2를 준비한 뒤 위 Build 명령으로 빌드합니다. `run-e2e.sh` 또는 `run-e2e.ps1`을 실행하면 구조 검사, Redis와 node 기동, 시나리오 실행, 자원 정리를 순서대로 수행합니다. 실행 설정과 로그는 `.logs/run-<id>/`에 보존합니다.

`channel`은 채널 이동을, `dungeon`은 파티 구성과 던전 입장을, `reconnect`는 Actor와 게임 상태 유지를, `update`는 instance node의 relocation과 교체를 검증합니다. 기본 실행은 네 시나리오를 모두 수행합니다. 공개 0.30.2 패키지가 게시되기 전에는 해당 버전의 실행 검증이 완료되지 않은 상태입니다.
