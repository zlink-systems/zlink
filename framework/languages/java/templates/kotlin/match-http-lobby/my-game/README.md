# my-game — HTTP lobby and match servers

This Kotlin template pairs two players through HTTP and runs their duel through ZLink STREAM. The lobby prepares a dynamic Room through the public object client before returning its ID and the Session endpoint. Room owns combat state; Player owns its Framework Actor context.

```mermaid
flowchart LR
  players[Players] -->|HTTP| lobby[HTTP lobby]
  players -->|STREAM| session[Session node]
  session -->|relay| matchA[Match node A]
  matchA <--> matchB[Match node B]
  lobby -->|Find room| location[(Redis Location Store)]
  matchA -->|Location / lease| location
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

Bash:

```sh
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,reconnect,update
./run-e2e.sh --check-structure
```

PowerShell:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

The runner validates `zlink.structure.json`, creates its own Redis Compose project with an OS-assigned loopback port, and starts match, session, and lobby processes. Each scenario uses a fresh cluster, unique Redis prefix and `.logs/run-<id>/` directory. Cleanup errors fail the run. `lobby` checks equal HTTP assignments and both Room participants; `hit` checks damage, wall and range rejection, missing targets and the winner; `reconnect` checks Actor identity and HP; `update` checks relocation, unchanged SessionInfo and Room state, graceful shutdown, replacement readiness and new Room admission on the replacement node.

## Known issues

Validated against the published Framework 0.30.2 packages.

## Deployment sequence

Start Redis, then both match processes using `server/config/match-a.json` and `match-b.json`. Start Session using `session.json`. Read its `READY` stream endpoint, write that value into the lobby configuration's `sessionEndpoint`, then start the lobby using `lobby.json`. Each installed application accepts its configuration path as its only argument. Configuration IDs match structure node IDs; endpoint port zero requests an OS-assigned port.

The match configuration `roomCapacity` sets the positive per-node Room stable-type limit. Deployment configurations allow multiple Rooms; the e2e runner uses one so the surviving host is full after relocation and the replacement must handle the next Room.

For a match host update, send `drain` to stdin and require `RELOCATION_REQUESTED`, `RELOCATED` and `SAFE_TO_SHUTDOWN`. Send `shutdown`, require `STOPPED` and exit zero, then start the replacement with the same configuration. The lobby remains outside host relocation. HTTP matchmaking holds the first POST response until another player requests the same mode; shutdown fails pending requests. The Netty HTTP adapter removes an unpaired request when its client disconnects, allowing the same player ID to request again. Cancellation does not change paired requests.

The Room domain is in `server/match/.../domain/RoomState.kt`. HTTP and ZLink lobby adapters implement the application matching port. No player route, membership, lease or Actor reference is serialized by an application relocation adapter.

## 한국어 실행 안내

기본 e2e는 Docker에서 실행합니다. 로컬 process 디버깅은 JDK 25를 준비하고 `--local` 또는 `-Local`로 실행합니다. Docker Compose를 설치한 뒤 위 Build 명령으로 빌드합니다. Windows에서는 PowerShell 7을 사용합니다. e2e runner는 실행마다 별도 Redis project와 key prefix를 만들고 임시 포트를 사용하며, 로그를 `.logs/run-<id>/`에 보존합니다.

HTTP 로비는 같은 mode의 두 요청을 모아 Room을 준비한 뒤 Session endpoint를 반환합니다. 배포 시 Session의 READY 출력에서 실제 endpoint를 확인하여 로비 설정에 넣습니다. match node 교체는 drain 완료와 SAFE_TO_SHUTDOWN을 확인한 뒤 shutdown 순서로 수행합니다.
