# MyGame megaserver

Players share one world mesh. Entry selects the first region instance with room; each Region owns its admission, membership and positions. Dungeon Spots run on the instance process. A Session binds a persistent Player Actor and relays typed packets.

```mermaid
flowchart LR
  players[Players] -->|STREAM| session[Session]
  session -->|relay| regionA[Region A]
  regionA --- regionB[Region B]
  regionA -->|GetOrCreate| instance[Dungeon]
  regionA -->|Location / lease| location[Redis]
```

## Prerequisites

Node.js 22 or later, npm, Docker Compose v2, and PowerShell 7 for the PowerShell runner. Framework packages are pinned to 0.30.1 in `package.json`; this release must be available in the public npm registry. The binding package installs its published native runtime. Configuration defaults bind listeners to port zero.

## Build

From this directory, on Bash:

```sh
npm install && npm run build
```

On PowerShell:

```powershell
npm install; if ($LASTEXITCODE -eq 0) { npm run build }
```

The build generates the Framework JSON schemas from the shared DTO classes. Application packets use the default typed serializer.

## Local e2e

```sh
./run-e2e.sh
./run-e2e.sh --scenario region,dungeon,reconnect,update
```

```powershell
./run-e2e.ps1
./run-e2e.ps1 -Scenario region,dungeon,reconnect,update
```

| Scenario  | Assertions                                                                                                             |
| --------- | ---------------------------------------------------------------------------------------------------------------------- |
| region    | p1 and p2 share forest-1; p3 enters forest-2. Movement reaches the same instance only.                                 |
| dungeon   | Region creates a Dungeon and receives its identity from the instance process.                                          |
| reconnect | A new connection preserves Actor identity, generation, instance and position.                                          |
| update    | Region host relocation preserves connected Sessions and game state. The stopped host is replaced and accepts new work. |

Each scenario uses an isolated Compose project, Redis prefix and `.logs/run-<id>/` directory. Redis uses an OS-assigned loopback port. The runner observes protocol output and cleans up its own nodes and Compose project. Cleanup failure returns a nonzero exit code.

## Known issues

Node typed handler forwarding requires the packet-name-to-class table in `server/region/infrastructure/zlink/handlers.ts` until #1731 is fixed in 0.32.0. No Framework state is included in application relocation snapshots.

Public 0.30.0 validation encounters #1725 during host relocation (participant inventory mismatch). This template pins 0.30.1, which includes the registered fix; candidate-package e2e validation is required before claiming deployment readiness.

## Deployment sequence

Start Redis, then the region and instance processes, then the Session process. Each role has its own entrypoint under `server/`; pass its JSON configuration path as the first argument. Use a unique node ID, shared Redis prefix and mesh name, and reachable advertised endpoints in deployed configurations.

Send `drain` to the selected region process. Wait for `RELOCATION_REQUESTED`, `RELOCATED` and `SAFE_TO_SHUTDOWN`. Send `shutdown`, verify `STOPPED` with outcome Stopped and reason None, then start the replacement with the same configuration. Capacity defaults to two players per Region and Region stable-type limit defaults to two instances per region process. Ensure the surviving host has capacity for relocation.

The e2e fixture limits each Region node to two Region Spots and keeps forest-1 and forest-2 occupied before drain. After relocation the survivor is full. Once the replacement reports `READY`, a new desert Region must be served by that replacement. Existing Region state and Session identity checks remain required. Only Region nodes register the application Entry and its first admission handler, so only those nodes declare an Entry in `zlink.structure.json`.

Message Follow is explicitly set to 30 seconds. The `SAFE_TO_SHUTDOWN` observation budget is the follow duration plus the existing relocation budget; the relocation operation deadline is unchanged.

## 한국어 실행 안내

Node.js 22 이상과 Docker Compose v2를 준비한 뒤 이 디렉터리에서 의존성을 설치하고 빌드합니다. Bash에서는 `./run-e2e.sh`, PowerShell 7에서는 `./run-e2e.ps1`로 지역 분리, 던전 생성, 재접속, host 교체 시나리오를 실행합니다. 개별 실행 방법은 위 Local e2e 절에 있습니다.

각 실행은 고유 Redis prefix와 Compose project를 사용하며 로그를 `.logs/run-<id>/`에 보존합니다. 지역 host를 교체할 때는 `drain` 이후 relocation 완료와 `SAFE_TO_SHUTDOWN`을 확인하고 종료합니다. Framework 패키지 0.30.1의 공개 게시가 빌드 전제입니다.
