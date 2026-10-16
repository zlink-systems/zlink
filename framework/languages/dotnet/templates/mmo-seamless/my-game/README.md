# MyGame MMORPG seamless world

This project runs a Session node, two Zone nodes, an Instance node, and a Redis Location Store. It includes a C# e2e client. Add server gameplay to the extension points in [AGENTS.md](AGENTS.md); no game engine project is included.

```mermaid
flowchart LR
    players[Players] -->|STREAM| session[Session node]
    session -->|relay| zoneA[Zone node A: Zones 1, 2, 3]
    zoneA -->|relocation| zoneB[Zone node B: Zones 4, 5, 6]
    zoneB -->|border pub/sub| zoneA
    zoneB -->|GetOrCreate| instance[Instance node: Dungeon User Spots]
    zoneB -->|Location / lease| location[(Redis Location Store)]
```

The initial `zlink.structure.json` uses the landing page's `mmo-seamless` composition. The structure checker accepts additional or reordered nodes, Spots and edges when identifiers, references, configuration paths and IDs, configured Zone IDs, and declared channel and Spot types agree with the format and server contracts. Runtime relocation can change the owner recorded in Redis without changing the initial structure definition.

## Prerequisites

- Docker Engine with Docker Compose v2, reachable from the current shell.
- Docker Desktop must use Linux containers; default execution needs no host .NET SDK.
- Bash for Linux, WSL, or macOS; PowerShell 7 for Windows.
- Linux native assets supported by the published `Zlink` package. Local process debugging additionally requires the .NET 8 SDK and ASP.NET Core 8 runtime.

Packages come from public NuGet only. `Directory.Packages.props` pins the Framework package family to `0.30.0`. This project has no dependency on a ZLink source checkout or local package feed.

## Build

Run commands from this project root:

```sh
./run-e2e.sh --build-only
```

PowerShell builds the same Dockerfile targets:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
```

`Dockerfile` owns fixed `SDK_IMAGE`, `RUNTIME_IMAGE`, and `DOCKER_CLI_IMAGE` defaults. Replace the appropriate base ARG when adopting a ZLink base image. Session, Zone, and Instance share the node image because their runtime dependencies match; each retains its own executable entry point. The ASP.NET Core runtime supplies shared hosting dependencies. The runner target adds Docker CLI and structure inputs without including the SDK.

Scripts remove unique image tags after each invocation. Docker retains build layers for subsequent runs.

## Local e2e

```sh
./run-e2e.sh
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
```

Both scripts build images and call the same C# orchestrator inside a runner container. A unique Compose project/network contains Redis and the runner. The runner checks structure before starting nodes. Nodes share its network namespace, preserving loopback listeners and OS-assigned ports without host port mappings. Redis publishes an ephemeral loopback port for local debugging.

The runner creates each node with interactive stdin, copies its configuration with `docker cp`, and attaches through `docker start -a -i`. `ManagedProcess` retains readiness, drain, shutdown, and replacement controls. Only the runner mounts the Docker socket; use a trusted local daemon. Scripts remove their labeled containers, Compose project, and image tags. Cleanup failure returns exit code 1.

Node stderr logs (`<node>.err.log`) include Framework message flow records because dispatch diagnostics use `Normal`. Inspect these logs from the first failing run.

On Unix sockets, Bash runs the runner with the caller's UID/GID and the socket's group so retained logs remain writable by the caller. Windows uses Docker Desktop's filesystem mapping. Build, Redis, and outer cleanup logs are in `.logs/docker-<project>/`; node settings and control logs are in `.logs/run-<id>/`.

The default runs all four scenarios. To select scenarios, use:

```sh
./run-e2e.sh --scenario cross,border,reconnect
./run-e2e.sh --check-structure
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario cross,border,reconnect
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Selection retains the canonical scenario order. Unknown or duplicate names fail before node startup. Exit status includes cleanup failures even when all selected scenarios pass.

For host process debugging, install the .NET 8 SDK and add `--local` or `-Local`. Redis remains in Docker. After a local build, `--no-build` or `-NoBuild` skips compilation:

```sh
./run-e2e.sh --local
./run-e2e.sh --local --no-build
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -Local
pwsh -NoProfile -File ./run-e2e.ps1 -Local -NoBuild
```

| Scenario | Observable behavior |
| --- | --- |
| cross | A player moves from Zone 3 on A to Zone 4 on B. Actor ID, generation, and STREAM Session remain unchanged. A subsequent movement succeeds. |
| border | A player near the Zone 3/4 border becomes visible to a player in the neighboring Zone through real pub/sub delivery. |
| reconnect | The client disconnects and returns to the same Actor generation and authoritative coordinate through a new Session. |
| update | A drains while a player continues sending movements. Spots and Actors relocate to B, the Session remains connected, and acknowledged movement continues after A is replaced. |

The client also requests a Dungeon User Spot from the Instance node through `GetOrCreate`. Scenario results use `✓`/`✗` with a total and a nonzero exit code on failure. A failed run retains diagnostics; inspect that run before repeating it.

Pass `--check-structure` to `run-e2e.sh` or `-CheckStructure` to `run-e2e.ps1` to check composition without starting nodes. After a local build, structure validation can also run without Docker:

```sh
dotnet e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll --root . --check-structure
```

Message Follow is explicitly configured to 30 seconds in shared hosting. The wait for `SAFE_TO_SHUTDOWN` is derived from that period plus the existing host relocation budget. Packet deadlines and the relocation operation budget remain unchanged.

Every e2e run validates the composition before starting nodes. `--check-structure` checks the supplied files without starting servers. Verify missing Entry and User Spot declarations with temporary input copies during development. The checker does not require the initial preset's node counts, deployment IDs or edge indices.

## Known issues

The pinned public Framework `0.30.0` has the following defects. The template preserves the assertions and reports failures; it does not compensate with retries or delays. Fixes for #1690 and #1691 are scheduled for `0.30.1`.

- [#1697](https://github.com/zlink-systems/zlink/issues/1697): Zone startup is blocked because startup validation calls `ZoneSpot.Configure()` with an empty `Context.SpotId`. The public Spot contract requires an instance identity, which this application uses to select adjacent-zone subscription topics. This fails with `ArgumentOutOfRangeException` through `ZLinkSpotStartupValidator.ValidateAsync` before scenarios run. No fixed topic is registered to compensate; the Framework must provide the contracted Context.

- [#1690](https://github.com/zlink-systems/zlink/issues/1690): Session node `ShutdownAsync` with a bound connection can return `TeardownFailed` with native errno 110. Cleanup errors remain in the result and process logs, even if selected scenarios passed.
- [#1691](https://github.com/zlink-systems/zlink/issues/1691): After relocation admission seal, an already accepted handler's `BoundSession.Send` can fail with `application admission is sealed`. Once startup is available, the default `update` scenario also requires this fix; passing cross, border, and reconnect does not establish update continuity.

## Deployment sequence

The JSON files in `server/config/` are examples. Set a reachable Redis endpoint and a shared, isolated key prefix for each game environment. Set routable mesh endpoints and the public Session STREAM endpoint. Configure authentication before exposing this minimal example to players.

1. Start Redis. For local manual deployment, use `docker compose -p my-game-local up -d --wait location` and obtain its chosen host port with `docker compose -p my-game-local port location 6379`.
2. Start Zone A using `server/config/zone-a.json`. Wait for its `READY` response after Zones 1–3 are created.
3. Start Zone B using `server/config/zone-b.json`. Wait for Zones 4–6. A's capacity is three; B's capacity is six so it can receive A's Zones during drain.
4. Start the Instance and Session processes using their configurations. Each process accepts `--config <path>`.
5. Connect clients to the Session endpoint. Requests bind a player Session to the player Actor and relay gameplay to its current Zone.

Example process invocation after updating the configuration's Redis port:

```sh
dotnet server/MyGame.Zone/bin/Debug/net8.0/MyGame.Zone.dll --config server/config/zone-a.json
```

For replacement, send `drain` to A's standard input. The application calls public host relocation in `PlannedMaintenance` mode, moving workload to the compatible remaining host. Wait for `RELOCATED` and `SAFE_TO_SHUTDOWN`, send `shutdown`, and wait for process exit. Start the replacement with the same logical node configuration and `Bootstrap` set to `false`. Existing Zones remain on B; replacement does not automatically redistribute them.

Use an external deployment orchestrator to control process lifetime in production. Planned maintenance here demonstrates drain and replacement at the same application version. A versioned rolling update requires a prepared compatible target and the Framework's `RollingUpdate` operation; changing a process binary alone does not migrate state.

Remove a manually started local Redis project with `docker compose -p my-game-local down --volumes`. The e2e runner removes only its own project automatically.

Fixed Zones are bootstrapped serially: Zone A fills its three-Spot capacity before Zone B starts. This makes the declared initial owners deterministic through public stable-type capacity. During e2e, Zone B has capacity for all but one world Zone. Before update drains Zone A, `vacate-zone` closes the unused first Zone through the public Spot manager. The remaining Zones fill Zone B; a fresh player then recreates the first Zone at the replacement Zone A and verifies its host and continued movement. Existing relocated players remain on Zone B.

## 한국어 실행 안내

Docker와 Compose v2를 설치한 뒤 Bash에서 `./run-e2e.sh`, PowerShell 7에서 `pwsh -NoProfile -File ./run-e2e.ps1`을 실행한다. 기본 실행은 빌드부터 node와 e2e client 실행까지 컨테이너에서 처리하므로 호스트 .NET SDK가 필요 없다. 로그는 `.logs/`에 남으며 자신이 만든 컨테이너·network·이미지 태그를 자동으로 제거한다.

로컬 디버깅은 .NET 8 SDK를 설치한 뒤 `--local` 또는 `-Local`로 실행한다. 공개 0.30.0에서는 #1697로 Zone이 READY 이전에 실패한다. 시나리오의 기대값을 유지하고 최초 실패 로그와 종료 코드를 보존한다. 게임 확장 위치와 공개 API 규칙은 `AGENTS.md`를 따른다.
