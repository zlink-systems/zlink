# my-game — FPS matchmaking API node

This Node.js FPS server template runs an API node, a Session node, play nodes A and B (match), and a Redis Location Store. The API node pairs `POST /match` requests by mode, generates a GUID Room ID, creates a dynamic Room User Spot, and waits for `RoomPrepared` before returning the Room ID and Session endpoint. `RoomRoster` owns the prepared member list and admission decision. Each Room owns movement, HP, action sequences, wall intersection, cooldowns, and match results.

```mermaid
flowchart LR
  subgraph playNodes["Play nodes"]
    matchA["Play node A (match): Entry / Room"]
    matchB["Play node B (match): Entry / Room"]
  end
  players[Players] -->|STREAM JoinRoomRequest| session["Session node"]
  session -->|relay| matchA
  matchA <-->|my-game-arena| matchB
  lobby -->|getOrCreate + PrepareRoom / RoomPrepared| matchA
  matchA -->|Location / lease| location[(Redis)]
  players -->|HTTP POST /match| lobby["API node (matchmaking)"]
```

## Prerequisites

Docker Compose with a running Docker daemon, and PowerShell 7 for the PowerShell runner. `package.json` pins public Framework and connector packages to version `0.34.0`. The template directory builds independently of the ZLink repository.

## Build

POSIX shell:

```sh
./run-e2e.sh --build-only
```

PowerShell:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
```

The build compiles the role entrypoints and generates DTO schemas for the default Framework JSON serializer. The generator does not register a custom message codec.

## Client connection

### Endpoint and first packet

Start with the API node's HTTP endpoint in `server/config/lobby.json`: `http://127.0.0.1:0`. Read its actual `endpoint` from the API node `READY` JSON. Send two concurrent `POST /match` JSON requests with distinct `playerId` values and the same `mode` (`duel` by default). Each HTTP response contains `roomId` and `sessionEndpoint`; the response waits for a pair and room preparation. See `e2e/client.mts` and `server/shared/lobby-messages.ts`.

Connect to that `sessionEndpoint` using STREAM. The Session `players` listener defaults to `ws://127.0.0.1:0` in `server/config/session.json`; Session `READY` JSON publishes its actual `endpoint`. Configure the API node's advertised `sessionEndpoint` with that running listener, as the e2e runner does. Both port-zero defaults need runtime addresses. WebSocket uses root path `/`. Match mesh listeners and Redis are internal endpoints.

The first STREAM packet is `JoinRoomRequest(playerId, roomId)`, using the assigned player and room. Only assigned members can join; success produces `MatchFound` and `RoomView` pushes. After binding, use request/reply for `RoomViewRequest` and `SessionInfoRequest`. A `RoomView` push is an unsolicited update and does not complete a room query. Room admission is not product authentication; validate identity before binding when adding authentication.

The local runner and Docker node entrypoint preload `dist/server/shared/diagnostics.js` with Node's `--require` option. Keep this preload when starting a node directly so the Framework telemetry worker inherits the logger provider and writes message-flow records to stderr.

### Connector installation

The default loopback listener is reachable only within its host/network namespace. Docker e2e nodes share the runner network namespace and are removed at the end. External clients need a deployment kept running with a reachable listener and an exposed container port. Browser, Unity WebGL, and Cocos Creator web clients can use this WebSocket endpoint; HTTPS pages require a secure `wss://` deployment. HTTP entry points also need a browser-accessible origin/CORS policy when called from a web client.

Use the same connector release as `package.json` dependencies `@zlink-systems/framework` and `@zlink-systems/stream-connector`. Replace `<version>` in the installation commands with that release. Check linked guides against that pin; engine adapters follow the linked repository READMEs. All clients use this template's packet contract.

| Client                             | Connector installation                                                                                                                                                                                                                                                                                                                                                                                                                                                          | Documentation guide                                                                                                                                                          |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Unity                              | Native/Editor: restore `Zlink.Stream.Connector` with NuGetForUnity. WebGL: add the `com.zlink.stream-connector.webgl` UPM Git package; include only the connector assembly for the active target. See the [Unity README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.md) and its package manifest.                                                                                                                                | [.NET engine integration](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/)                                                                   |
| Unreal                             | Copy `framework/languages/cpp/connector/engines/unreal` into `Plugins/ZLinkStreamConnector`, then package the native connector under `ThirdParty/ZLink` as described in the [Unreal README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unreal/README.md).                                                                                                                                                                                     | [C++ engine integration](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/)                                                                       |
| Godot                              | .NET: add the NuGet package `Zlink.Stream.Connector` and run `dotnet restore`; see the [C# README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Godot/csharp/README.md). GDExtension: build the C++ Godot adapter against the framework prebuilt and matching `godot-cpp`; see the [C++ README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Godot/cpp/README.md). The .NET sample does not support Web export. | [.NET](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/), [C++](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/) |
| Cocos Creator                      | Web target: `npm install @zlink-systems/stream-connector@<version>`; see the [Cocos Creator README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/CocosCreator/README.md). This package does not provide a Creator native adapter.                                                                                                                                                                                                               | [TypeScript engine integration](https://docs.zlink.systems/node/guide/stream-connector/12-engine-integration/)                                                               |
| Axmol                              | Copy `framework/languages/cpp/connector/engines/axmol` into `third_party/zlink_axmol_connector` and link `zlink::stream_connector` from the installed C++ framework package; see the [Axmol README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Axmol/README.md).                                                                                                                                                                              | [C++ engine integration](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/)                                                                       |
| Web browser (TypeScript connector) | `npm install @zlink-systems/stream-connector@<version>`; import the package root in the browser build. Use `ws://` or `wss://`, subject to the page's security policy.                                                                                                                                                                                                                                                                                                          | [TypeScript connector](https://docs.zlink.systems/node/guide/stream-connector/)                                                                                              |
| C++                                | Install the C++ framework prebuilt containing the connector; use `find_package(zlink_stream_connector_cpp CONFIG REQUIRED)` and link `zlink::stream_connector`.                                                                                                                                                                                                                                                                                                                 | [C++ connector](https://docs.zlink.systems/cpp/guide/stream-connector/)                                                                                                      |
| C#/.NET                            | `dotnet add package Zlink.Stream.Connector --version <version>`                                                                                                                                                                                                                                                                                                                                                                                                                 | [.NET connector](https://docs.zlink.systems/dotnet/guide/stream-connector/)                                                                                                  |
| Java                               | Add Maven/Gradle dependency `systems.zlink:zlink-stream-connector:<version>`.                                                                                                                                                                                                                                                                                                                                                                                                   | [Java connector](https://docs.zlink.systems/java/guide/stream-connector/)                                                                                                    |
| Kotlin                             | Add `systems.zlink:zlink-stream-connector:<version>` and `systems.zlink:zlink-framework-kotlin:<version>` for the coroutine wrapper.                                                                                                                                                                                                                                                                                                                                            | [Kotlin connector](https://docs.zlink.systems/kotlin/guide/stream-connector/)                                                                                                |
| Node                               | `npm install @zlink-systems/stream-connector@<version>`; use the package root and Node.js 22 or later.                                                                                                                                                                                                                                                                                                                                                                          | [Node connector](https://docs.zlink.systems/node/guide/stream-connector/)                                                                                                    |

### HTTP assignment and STREAM join example

Save as `e2e/connect-once.mts` in a copy, build with `npm run build`, then run `node dist/e2e/connect-once.mjs <actual-lobby-http-endpoint>` against a separately running API node, session node, play nodes (match), and Redis. Use fresh player IDs. The e2e command removes its deployment when finished. This example reads only the room ID; received contract int64 values remain decimal strings with the JSON codec.

```typescript
import '../.zlink-framework-json-schemas.cjs';
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import { JoinRoomRequest } from '../server/shared/lobby-messages.js';
import type { MatchResponse } from '../server/shared/lobby-messages.js';

const lobby = process.argv[2];
if (!lobby) throw new Error('Pass the API node HTTP endpoint from READY');
const players = ['guide-player-a', 'guide-player-b'];
const assignments = await Promise.all(
  players.map(async (playerId) => {
    const response = await fetch(lobby + '/match', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ playerId, mode: 'duel' })
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}: ${await response.text()}`);
    return (await response.json()) as MatchResponse;
  })
);
const clients = assignments.map((assignment) =>
  zlinkStreamConnectorFactory.create({
    endpoint: assignment.sessionEndpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    reconnect: { enabled: false }
  })
);
try {
  await Promise.all(clients.map((client) => client.connect()));
  const views = clients.map(
    (client) =>
      new Promise<{ roomId: string }>((resolve, reject) => {
        client.on<{ roomId: string }>('RoomView', (message) => resolve(message.payload));
        client.on('ActionRejected', () => reject(new Error('Admission rejected')));
        client.onErrorReceived((error) => reject(new Error(error.message)));
        client.onDisconnected(() => reject(new Error('Disconnected before RoomView')));
      })
  );
  await Promise.all(
    clients.map((client, index) =>
      client.send(new JoinRoomRequest(players[index]!, assignments[index]!.roomId)).submit()
    )
  );
  console.log(await Promise.all(views));
} finally {
  await Promise.all(clients.map((client) => client.close()));
}
```

## Docker e2e

Docker runs the e2e runner and each node in containers. All roles use one image; the runner image adds Docker CLI 28.5.1. `NODE_IMAGE` pins `node:22.20.0-trixie` with a slim runtime and the native binding's OpenSSL dependency. Nodes share the runner network namespace, preserving loopback listeners and OS-assigned ports. Configuration is copied with `docker cp`; stdin/stdout controls use `docker start -a -i`. Each invocation owns a unique Compose project, network, and image tags. Cleanup failures return exit code 1, and shared Docker build cache remains available.

Build and outer cleanup logs are written to `.logs/docker-<project>/`. Node configuration, stdout, and message-flow stderr are kept in `.logs/run-<id>/`. For local process debugging, install Node.js 22 or later and npm, then run `./run-e2e.sh --local` or `pwsh -NoProfile -File ./run-e2e.ps1 -Local`. Add `--no-build` or `-NoBuild` to reuse an existing local build. Redis remains in Docker.

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

The default run executes `lobby`, `hit`, `wall`, `input`, `reconnect`, and `update` in that order. The runner checks the structure and 46 game plus 19 API node boundaries before starting nodes, assigns a unique Redis key prefix to each scenario, starts play nodes (match) before the session node and the API node, and waits for public runtime readiness. Listeners bind port zero and publish their actual endpoints. The runner removes only processes and the Compose project that it created; cleanup failures produce a nonzero exit code.

| Scenario    | Client observations                                                                                                                                                                                                                                                                                      |
| ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `lobby`     | An omitted mode defaults to duel; modes receive separate Rooms; each pair shares a Room and Session endpoint; an outsider is rejected from an empty Room; client `PrepareRoom` is rejected before joining and during a match; a Lobby restart preserves the existing match and provisions a new Room ID. |
| `hit`       | A hit changes HP by 25; upward and out-of-range rays miss; the fourth hit finishes the match and later actions are rejected.                                                                                                                                                                             |
| `wall`      | An intervening wall blocks a ray; the wall behind the target does not block a hit.                                                                                                                                                                                                                       |
| `input`     | Shared sequences, movement bounds, wall contact, movement budget, fire interval, rejection reasons, unchanged state, and server-issued retry delays are checked.                                                                                                                                         |
| `reconnect` | A new Session retains the Actor identity, Room, position, HP, and sequence; a finished match retains its winner.                                                                                                                                                                                         |
| `update`    | Relocation preserves Session bindings and Room state; an outsider is rejected after relocation; safe shutdown is observed, and the replacement accepts a new Room.                                                                                                                                       |

`e2e/domain-checks.ts` runs 46 game boundary checks and `e2e/lobby-domain-checks.ts` runs 19 API node boundary checks before any scenario starts. The game checks cover closed ray/AABB faces, tangents, range limits, wall/player ties, movement and fire timing, rejected state, and snapshots restored under a different monotonic clock origin. The Lobby checks cover roster preparation and restoration, mode pairing, Room ID uniqueness, and HTTP response timing.

## FPS rules and extension points

`MatchRequest` sends `{playerId}` or `{playerId, mode}` to `POST /match`; an omitted mode defaults to `duel`. The example accepts a claimed player ID; replace this boundary with authenticated identity before deployment. A Session binds an Actor only on the first allowed `JoinRoomRequest`. It relays only `JoinRoomRequest`, `RoomViewRequest`, `MoveRequest`, `ShootRequest`, and `SessionInfoRequest`. `PrepareRoom`, `RoomPrepared`, `HttpNodeReady`, and other client packets receive `ActionRejected` with `packet-not-allowed`. Application messages use the default typed Framework serializer.

The API node generates `RoomPrefix + UUID` once in `server/lobby/matchmaker.ts`. It creates the dynamic Room, sends `PrepareRoom(matchId, members)`, and returns the HTTP assignment only after receiving `RoomPrepared(matchId)`. `server/match/roster.ts` copies and validates the roster, admits only those players, accepts an identical preparation again, and rejects a changed roster. Room relocation stores `{Game, Roster}` together.

Coordinates use `Vec3(x, y, z)` with Y up. `ArenaMap` defines bounds X/Z=0..100 and Y=0..10, two static AABBs, and ordered server spawn points. A player box is 1 wide, 2 high, and 1 deep from its feet, with eye height 1.6. Feet remain at Y=0. Vertical movement is rejected as `vertical-move`; jumping and gravity are not implemented. Movement tests the feet-position segment against closed wall boxes, including contact at the endpoint. It does not implement swept player-volume collision.

`MoveRequest(delta, sequence)` has a maximum step of 2 and a speed budget of 10 units per second, capped at 2 since the last accepted movement. The first movement has the full budget. Nonfinite deltas, nonzero `delta.y`, oversized steps, map exits, wall intersections, and budget overruns are rejected. `ShootRequest(aimDirection, sequence)` normalizes finite nonzero aim, casts from the authoritative eye position up to 80 units, and chooses the closest wall or living player AABB. Wall ties and box contact block the ray. A hit causes 25 damage; accepted misses and wall-blocked shots still consume their sequence and 250 ms fire interval. There is no ammunition model. The Framework message model carries only finite JSON numbers, so nonfinite input reaches only the domain checks.

Move and Shoot share one increasing sequence in `1..9007199254740991`. Values above the upper bound are rejected as `integer-out-of-range`; values at or below the last accepted sequence, including zero and negative values, are rejected as `stale-sequence`. The shared domain check evaluates the upper bound before stale sequence and match phase. Rejections change neither position, HP, sequence, nor movement/fire timing. `retryAfterMs` is positive only for movement-budget or fire-interval rejection; clients wait for the reported duration and resend the same input. `MoveResult` and `ShotResult` acknowledge accepted actions; `ActionRejected` and `ShotRejected` report rejected inputs. `RoomView` includes positions, HP, last sequences, phase, and winner so a fresh client can continue with its last sequence plus one. At zero HP, the match finishes and later Move/Shoot inputs are rejected.

Hit detection uses the current server state. The template has no lag compensation, projectile simulation, or client prediction. Extend `RoomState` for weapons and match rules, `ArenaMap` for level geometry and spawn selection, and the shared contracts for client actions. Keep cooldown, movement budget, and hit decisions in `RoomState`. `RoomSnapshot` stores remaining milliseconds for timing and never transfers a process's absolute clock reading.

Message Follow is configured explicitly to 30 seconds. After `RELOCATED`, the safe shutdown observation budget is the follow duration plus the existing relocation observation budget; the relocation operation deadline is unchanged.

`--check-domain` (`-CheckDomain` in PowerShell) runs 46 game domain checks and one separate wire golden check for four samples. The wire check runs in the Framework execution image; the runner does not load Framework. It briefly starts one local typed Session node and observes its public connector payloads; Redis and game scenario nodes are not started.

## Framework behavior to know

- **Waiting for DB or file I/O in a handler** — An ordinary async terminal retains the turn, while `Yield` lets other handlers on the same Spot proceed. [Guide](https://docs.zlink.systems/node/guide/server/32-execution-model/#io-worker-wait).
- **Creation that misses its deadline** — `DeadlineExceeded` and caller cancellation have distinct outcomes. [Guide](https://docs.zlink.systems/node/guide/server/22-actor/#creation-deadline). [server/session/session.ts:41](server/session/session.ts).
- **Many logins at once** — Each creation for a different ID requires a Location Store round trip, so measure expected load when setting the deadline. [Guide](https://docs.zlink.systems/node/guide/server/25-location/#creation-load).
- **Sending under load** — Send waits for capacity, and the application sees the final result. [Guide](https://docs.zlink.systems/node/guide/server/33-backpressure/#send-backpressure). [server/session/session.ts:31](server/session/session.ts).
- **Duplicate login** — Without a callback, a bind from another Session is accepted; reject it in the Actor duplicate-bind callback to prevent this. [Guide](https://docs.zlink.systems/node/guide/server/39-session-binding/#duplicate-session-bind). This template keeps the default (accepts the new Session).
- **ObjectGeneration** — `ObjectGeneration` distinguishes logical incarnations of the same ID; it is not a data version or write authority. [Guide](https://docs.zlink.systems/node/guide/server/34-activation-lifetime/#object-generation). [server/session/session.ts:55](server/session/session.ts).
- **READY and Serving** — The Framework host prints `READY` after observing runtime `Serving`. [Guide](https://docs.zlink.systems/node/guide/server/12-operations/#wait-for-serving). [server/shared/hosting.ts:92](server/shared/hosting.ts); [server/shared/hosting.ts:142](server/shared/hosting.ts).

## Known issues

Validated against the published Framework 0.34.0 packages. The typed push delivery table restores declared DTO classes for the public typed-handler limitation tracked in [#1731](https://github.com/zlink-systems/zlink/issues/1731). Do not replace it with a manual codec.

## Deployment sequence

Start Redis, then play nodes A and B (match), Session, and the API node. The API node requires the Session endpoint; each role reports `READY` with its actual listener endpoint. The API node creates dynamic Room Spots through the RouteMesh and waits for Room preparation before returning an HTTP response.

Configure Redis, unique key prefixes, and listener addresses in `server/config/`. Start `dist/server/match/main.js` with `server/config/match-a.json`, then the same entrypoint with `server/config/match-b.json`. Start `dist/server/session/main.js` with `server/config/session.json`, then `dist/server/lobby/main.js` with `server/config/lobby.json` containing the actual Session endpoint. All roles use the common hosting module. To update a play node (match), write `drain` to its standard input and wait for `RELOCATION_REQUESTED`, `RELOCATED`, and `SAFE_TO_SHUTDOWN`. Write `shutdown`, verify `STOPPED` with outcome `Stopped` and reason `None`, and wait for exit. Start a replacement with the same node configuration and wait for `READY`. Redis must remain available during relocation and the API node restart.
