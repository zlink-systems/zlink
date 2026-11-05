# my-game — HTTP lobby

An action/FPS template with an HTTP matchmaking process, a STREAM Session process, and independently deployable match processes. The HTTP lobby pairs players by mode, prepares a dynamic Room through the public object client, and returns its ID and the Session endpoint. Room User Spots own positions, HP, action sequences, wall intersection, and match results.

```mermaid
flowchart LR
  players[Players] -->|HTTP| lobby[HTTP lobby]
  players -->|STREAM| session[Session]
  session -->|relay| matchA[Match A: Entry / Room]
  matchA <-->|my-game-arena| matchB[Match B: Entry / Room]
  lobby -->|Find room| location[(Redis)]
  matchA -->|Location / lease| location
```

## Prerequisites

Docker Compose with a running Docker daemon, and PowerShell 7 for the PowerShell runner. Public Framework packages are pinned to 0.30.3 in `package.json`. This directory can be copied independently of the repository.

## Build

POSIX shell:

```sh
./run-e2e.sh --build-only
```

PowerShell:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
```

The build compiles separate lobby, Session, and match entrypoints and generates DTO schemas for the default Framework JSON serializer. It does not register a custom codec.

## Docker e2e

Docker runs the e2e runner and each node in containers. All roles use one image; the runner image adds Docker CLI 28.5.1. `NODE_IMAGE` pins `node:22.20.0-trixie` with a slim runtime and the native binding's OpenSSL dependency. Nodes share the runner network namespace, preserving loopback listeners and OS-assigned ports. Configuration is copied with `docker cp`; stdin/stdout controls use `docker start -a -i`. Each invocation owns a unique Compose project, network and image tags. Cleanup failures return exit code 1, and shared Docker build cache remains available.

Build and outer cleanup logs are in `.logs/docker-<project>/`; node configuration, stdout and message-flow stderr remain in `.logs/run-<id>/`. For local process debugging, install Node.js 22 or later and npm, then run `./run-e2e.sh --local` or `pwsh -NoProfile -File ./run-e2e.ps1 -Local`. Add `--no-build` or `-NoBuild` to reuse an existing local build. Redis remains in Docker.

```sh
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

```powershell
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

| Scenario | Observed behavior |
| --- | --- |
| `lobby` | Omitting `mode` selects `duel`; both players receive the same prepared Room assignment with a `room-` prefix and 32 lowercase UUID hex characters. Before either matched player joins, an outsider is rejected with `admission-rejected` and the first Room view confirms the rejection did not change state. A client `PrepareRoom` packet is rejected with `packet-not-allowed`, and restarting the lobby produces a new Room ID. |
| `hit` | Accepted shots reduce HP and advance `LastSequence`. If a cooldown rejection occurs, the client waits for its `RetryAfterMs` before retrying the same sequence. Out-of-range and missing-target rejections leave the Room view unchanged. The lethal hit reports `Phase` and `Winner`. Domain checks exercise the cooldown boundary deterministically. |
| `wall` | Players on opposite sides of the wall cannot shoot through it; each `blocked-by-wall` rejection leaves HP and sequence unchanged. Moving both players below the wall opens a clear shot lane. |
| `input` | Accepted and rejected movement requests are sent consecutively before waiting for replies, followed by consecutive shot requests. `MoveResult` and `ShotResult` identify accepted actions; `move-budget` and `fire-too-soon` preserve accepted state and sequence. Retrying the same request after the server's `RetryAfterMs` succeeds. Oversized, stale, out-of-map, wall-crossing and over-limit sequences are also rejected without state changes. |
| `reconnect` | A new Session binds the same Actor generation and restores the finished Room view, including sequence, `Phase`, and `Winner`. |
| `update` | Planned maintenance relocates the Room and its `{Game, Roster}` snapshot, preserves Session bindings and matched membership, and lets the replacement node accept a new Room. |

The runner validates structure before starting an isolated Compose project. It starts match processes, Session, then lobby; the lobby receives the Session's actual published endpoint. HTTP, mesh, and STREAM listeners bind port zero. Each scenario uses a unique Redis prefix and records node output, message-flow diagnostics, and client observations in `.logs/run-<id>/`. Cleanup failures fail the run.

## Known issues

Validated against the published Framework 0.30.3 packages.

Typed push delivery restores the declared DTO class through the packet-name-to-class table in `handlers.ts` (#1731).

## Deployment sequence

Configure Redis and a unique shared key prefix in `server/config/`. Start `node dist/server/match/main.js server/config/match-a.json`, then the same entrypoint with `server/config/match-b.json`. Start `node dist/server/session/main.js server/config/session.json`. Read its `READY.streamEndpoint`, set the lobby's `sessionEndpoint` to that actual address, and start `node dist/server/lobby/main.js server/config/lobby.json`. The HTTP lobby announces its bound address in `READY.httpEndpoint`.

Clients send `POST /match` with `{playerId, mode}`; omitted `mode` defaults to `duel`. The first request waits until another player in the same mode arrives. The lobby generates a Room ID from `room-` and a 32-character lowercase UUID, creates the Room, sends `PrepareRoom(matchId, members)`, and waits for `RoomPrepared` before returning `{roomId, sessionEndpoint}`. `RoomRoster` owns admission and rejects players outside the matched list. Session relays only `JoinRoomRequest`, `RoomViewRequest`, `MoveRequest`, `ShootRequest`, and `SessionInfoRequest`; internal packets receive `ActionRejected("packet-not-allowed", 0)`. Integer contract values use the common maximum `9,007,199,254,740,991`. Domain time uses a monotonic clock, and relocation snapshots store movement and fire timers as remaining milliseconds.

The lobby queue remains in process memory and is not relocated. The e2e runner sets each match host's Room capacity to one so that the replacement must accept the next Room while its peer hosts the relocated Room. Deployment configurations allow multiple Rooms.

Write `drain` to the Room host's standard input and wait for `RELOCATION_REQUESTED`, `RELOCATED`, and `SAFE_TO_SHUTDOWN`. Write `shutdown`, verify `STOPPED` with outcome `Stopped` and reason `None`, and wait for process exit. Start a replacement with the same configuration and wait for `READY` before assigning new work. Keep Redis, the lobby, and Session available throughout the update.
