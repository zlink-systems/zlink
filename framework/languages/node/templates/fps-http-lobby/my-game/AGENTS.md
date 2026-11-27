# MyGame FPS development guide

## Rules for AI

### Use this template as a reference

This template is a reference. To build a new game, create a new project and design its nodes, Spots, Actors and messages for that game, using this template's structure and rules as reference; do not rename or edit this template in place into the game.

### Server design

`server/lobby` is an API node that holds no game state; the session node manages client connections, and the play nodes host the game Spots. Before writing code, decide which kind you are editing.

- A play node hosts Spots. A session-bound Actor owns the player's state, and a Spot owns shared state. Load it once on connect; do not read the database while handling a request.
- Write only results to the database, one transaction per round. Check duplicate requests in Actor or Spot memory.
- While a game is running, a Spot handler never awaits a remote request or a database reply: the await holds the Spot's turn and stops every message and timer in it. When a round ends, the Spot writes the results of all its Actors in one transaction, then tells each Actor after the commit.
- Values many players see live in node memory or one owning Spot; values many requests update are applied by that one owning Spot. A process that does not own a value sends a request to its owner instead of writing it.
- Spots and Actors already process one message at a time. Do not guard their state with locks, semaphores or process-wide static stores, and do not update memory from database commit hooks; the writer updates its owner's memory after commit, others send a message to the owner.
- Split roles so the game can scale out from day one: play nodes host lobby and room Spots and player Actors and are the only processes you add; a dedicated play node hosts only game-wide Instance Spots (matchmaking, game-wide values), and the API node and room hosts omit those factories. Send and request by Spot ID only; placement is configuration. Keep shared-Spot values recoverable from room resends or a database read at startup.
- Never have two Actors or Spots wait on requests to each other inside a turn; notify with send and let the state owner decide.
- Actors move between nodes when they join a Spot or relocate. Keep all mutable Actor fields in one state record, register stateful Actors with the state-preserving relocation policy, and test that save and restore preserve every field.
- Player data reaches a Spot with the Actor that joins it; do not request it per player at Spot entry. If a Spot truly needs extra data, fetch it for all players in one request before the game starts.
- Keep game rules as synchronous domain code with no ZLink or database calls. Reach the database only through capability ports named after game events (`loadPlayer`, `commitRound`), never a generic CRUD repository. Keep the domain model (rounds, hands, wallets) separate from message DTOs and database rows, converting once at the handler and adapter boundary. Do not add pass-through layers or a service layer outside Spots and Actors.
- Decide what remains in the database if the process dies now, and how startup cleans it up. Retry only on the "being removed" reply; return other failures to the caller.
- Do not block, sleep, retry or raise timeouts in handlers; ZLink waits for remote connections. Report ZLink defects with a minimal repro instead of compensating in game code.
- Send money or reward results only after the database commit.
- One decision has one owner; find the existing one before adding another. Take test expectations from the spec, not from the implementation.

The API node may read and write the database per request, but it holds no game state and changes game state only by sending requests to play nodes.

Details: [Responsibilities of Play and API Nodes](https://docs.zlink.systems/node/guide/server/03-concepts/#10-stateful-game-nodes-and-the-http-lobby)

## Project and development

Run one scenario with `./run-e2e.sh --scenario lobby`. Close the connection only after receiving the reply to the last request. The server may miss a send that is immediately followed by close.

This Node.js 22 project uses TypeScript and NestJS to run an API node, a Session node, play nodes A and B (match), and a Redis Location Store. The API node pairs `POST /match` requests by mode, assigns a UUID Room ID, and waits for a dynamic Room's `RoomPrepared` reply before returning the Session endpoint. Public Framework and connector packages are pinned to `0.34.0` in `package.json`. The [README](README.md) owns execution, deployment, and known issues; [zlink.structure.json](zlink.structure.json) describes initial topology and ordered scenarios.

Run from this project root with Docker Compose v2 and Linux containers:

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

PowerShell 7 provides the same commands:

```powershell
.\run-e2e.cmd -BuildOnly
.\run-e2e.cmd
.\run-e2e.cmd -Scenario lobby,hit,wall,input,reconnect,update
.\run-e2e.cmd -CheckStructure
.\run-e2e.cmd -CheckDomain
```

For host process debugging, install Node.js 22 or later and npm, then add `--local` or `-Local`. After a local build, `--no-build` or `-NoBuild` skips compilation. Keep scenario assertions and inspect the first failure before rerunning.

`--check-domain` (`-CheckDomain` in PowerShell) runs 46 game domain checks and one separate wire golden check for four samples. The wire check runs in the Framework execution image; the runner does not load Framework. It briefly starts one local typed Session node and observes its public connector payloads; Redis and game scenario nodes are not started.

## Modules and extension points

1. When creating a typed message, declare nullability and wire types in the shared contract.
   - Declare nullable fields as `T | null`. The template schema generator emits `{ type: "nullable", value }` for the packet. Use `?` separately to allow omission.
   - Encode 64-bit integers as decimal strings in JSON. Browser clients also send strings, for example `"handNumber": "42"`. [Message value rules](https://docs.zlink.systems/node/guide/server/20-channel-messaging/#12-message-value-rules)

- `server/shared/messages.ts`: game client contracts. `lobby-messages.ts`: HTTP and Room preparation contracts. `constants.ts`: game limits, protocol names, and rejection reasons. `lobby-constants.ts`: HTTP endpoints, preparation packet names, and scenario order. `geometry.ts`: `Vec3`, closed slab ray/AABB intersection, static map, spawn points, and player boxes.
- `server/match/domain.ts`: authoritative positions, HP, phase, winner, shared sequences, movement budgets, cooldowns, and hit selection. Extend this domain for weapons, scoring, and match rules. Production time is monotonic; only the `RoomState` constructor accepts a test clock. Snapshot objects contain remaining milliseconds rather than process clock readings.
- `server/lobby/matchmaker.ts`: mode queues and the sole Room ID generator. `server/lobby/http.ts` creates the dynamic Room and waits for `PrepareRoom` to return `RoomPrepared` before answering.
- `server/match/roster.ts`: owns the prepared member list, admission decision, and roster snapshot. `server/match/spots.ts` and `handlers.ts` connect Room preparation, actor admission, game decisions, and relocation.
- `server/session/session.ts`: connection callbacks, Actor binding, and client packet allowlist. `JoinRoomRequest` is the first allowed packet; `PrepareRoom`, `RoomPrepared`, `HttpNodeReady`, and other server contracts are rejected as `packet-not-allowed`.
- `server/shared/hosting.ts`, `config.ts`, and `constants.ts`: shared setup, diagnostics, configuration, drain, and shutdown observation. Retain public Framework ownership of routing, membership, binding, and relocation.
- `e2e/client.mts`: received client results for lobby, hit, wall, input, reconnect, and update. `domain-checks.ts` and `lobby-domain-checks.ts`: 46 game and 19 API node boundaries in the same executable. `structure.ts`: descriptor/config/contract consistency before infrastructure starts. `runner.ts`: owned process lifecycle.

## Domain ownership and asynchronous execution

`RoomState` is the only owner of movement budget, fire interval, and closest-hit decisions. Y is up; map bounds and fixed wall AABBs are server constants in `ArenaMap`. Movement tests the feet-position segment. Feet remain at Y=0; nonzero `delta.y` is rejected as `vertical-move`. Jumping and gravity are not implemented. Ray contact with a box face counts as intersection, and a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejected inputs leave all state and timing unchanged. `RoomView` exposes last sequences so new clients continue after reconnect.

Current server state determines hits. The template includes no lag compensation, projectiles, or client prediction. Extend the existing geometry, rules, and typed contracts instead of adding a second decision owner in handlers or clients.

Keep game state in the Spot domain and use public Actor/Spot APIs. Framework execution serializes handler turns; do not add a second lock, queue, or generation check for the same execution permission. Do not synchronously wait for infrastructure inside a handler turn. Public async calls from application handlers remain permitted. Relocation adapters serialize game state; Framework owns host transfer, location, and route continuity. Validate relocation completion and safe shutdown observations, not command acceptance alone.

## Public API references

Use the [ZLink documentation](https://docs.zlink.systems/), [Node server guide](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), and [agent index](https://docs.zlink.systems/llms.txt). Check current documentation against the package pinned in `package.json`. Entry/Spot, Actor, Session, Location, and relocation operations use public Framework APIs and default typed serialization. Do not use reflection, private APIs, raw frames, or an application routing workaround.

## Diagnostics and defects

Dispatch diagnostics are `normal` in `server/shared/hosting.ts`; node stderr contains message-flow records. Preserve the first `.logs/run-<id>/` settings, stdout, and stderr before modifying or rerunning a failing case. Compare operation, Actor, flow/corr, and transition order between passing and failing traces. A single isolated failure remains a defect even if a later run passes. Preserve startup exceptions when no flow record exists yet.

For local process discovery diagnostics, set `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` before launching nodes. Docker nodes do not inherit arbitrary host environment variables. Do not change timeout, fixed waits, or assertions to hide failures. The runner cleans up its own processes and Compose project; retain cleanup errors and do not stop other workloads.

If a documented public API fails, preserve package versions, OS, command or minimal public API reproduction, expected result, first node exception/stack, and relevant flow/corr records. Check [existing issues](https://github.com/zlink-systems/zlink/issues) before proposing a report. Remove credentials and private data. Show the draft to the developer before publishing an issue; document confirmed affected versions in the README's Known issues.

## Client connection

The [README client connection section](README.md#client-connection) owns the shared client installation table and minimal TypeScript example. The Session `players` listener defaults to `ws://127.0.0.1:0` in `server/config/session.json` (WebSocket root path `/`, OS-assigned port). Use its actual STREAM address from `READY`; mesh and Redis are internal endpoints. External clients need a running deployment with a reachable interface and exposed port; HTTPS web clients require `wss://`.

Start with two same-mode `POST /match` requests on the API node's HTTP endpoint. Use each returned `sessionEndpoint` and assigned `roomId` for the first STREAM packet, `JoinRoomRequest`; successful entry produces `MatchFound` and `RoomView` pushes. Keep the API node's advertised Session address consistent with Session READY. Register push handlers before sending with `send(...).submit()`; do not turn admission into request/reply. Request `SessionInfoRequest` only after Actor binding. Retain the default typed JSON path and validate identity before binding when adding authentication.

Match connector versions to `package.json` dependencies `@zlink-systems/framework` and `@zlink-systems/stream-connector`; check examples against the public connector source and pinned package. Preserve the endpoint, HTTP preconditions, packet names, and push/reply distinction when updating the README.

## Run the server and e2e

Run `./run-server.sh` or `.\run-server.cmd` to build and start the local nodes and Redis. Use the WebSocket endpoint printed by `Server ready` and stop the server with Ctrl+C. In another terminal, `./run-e2e.sh` or `.\run-e2e.cmd` detects that server and runs compatible scenarios. Stop the server for full Docker isolation. On PowerShell 7 the corresponding scripts are `run-server.ps1` and `run-e2e.ps1`.

Node stdout, stderr, Framework message-flow files and failure details are under `.logs/run-<id>/`. Docker build and cleanup logs are under `.logs/docker-<project>/`. Use the log paths printed by each command.
