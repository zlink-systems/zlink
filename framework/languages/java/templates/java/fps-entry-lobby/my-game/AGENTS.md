# MyGame FPS development guide

## Client connection

Use [Client connection in README](README.md#client-connection) for this template's endpoint, first application flow, connector choices and Java example. Keep the endpoint and packet order consistent with `server/config/`, Session startup, shared DTOs and the e2e client. Register push handlers before sending; preserve the documented join and HTTP preconditions. Use the published WebSocket endpoint for browser, WebGL and Cocos Creator web targets. Use the release pinned by `build.gradle.kts` extra `frameworkVersion` for connectors and `bindingVersion` for the binding; `<version>` and `<binding-version>` in this file refer to those keys. Do not add literal release versions or copy engine sample packets into this template.

## Use this template as a reference

This template is a reference. To build a new game, create a new project and design its nodes, Spots, Actors and messages for that game, using this template's structure and rules as reference; do not rename or edit this template in place into the game.

## Rules for AI

Every server process in this project is a stateful ZLink node. Before writing code, decide which kind you are editing.

- A play node hosts Spots. A session-bound Actor owns the player's state, and a Spot owns shared state. Load it once on connect; do not read the database while handling a request.
- Write only results to the database, one transaction per round. Check duplicate requests in Actor or Spot memory.
- While a game is running, a Spot handler never awaits a remote request or a database reply: the await holds the Spot's turn and stops every message and timer in it. When a round ends, the Spot writes the results of all its Actors in one transaction, then tells each Actor after the commit.
- Values many players see live in node memory or one owning Spot; values many requests update are applied by that one owning Spot. A process that does not own a value sends a request to its owner instead of writing it.
- Spots and Actors already process one message at a time. Do not guard their state with locks, semaphores or process-wide static stores, and do not update memory from database commit hooks; the writer updates its owner's memory after commit, others send a message to the owner.
- Split roles so the game can scale out from day one: play nodes host lobby and room Spots and player Actors and are the only processes you add; a dedicated play node hosts only game-wide Instance Spots (matchmaking, game-wide values), and the lobby and room hosts omit those factories. Send and request by Spot ID only; placement is configuration. Keep shared-Spot values recoverable from room resends or a database read at startup.
- Never have two Actors or Spots wait on requests to each other inside a turn; notify with send and let the state owner decide.
- Actors move between nodes when they join a Spot or relocate. Keep all mutable Actor fields in one state record, register stateful Actors with the state-preserving relocation policy, and test that save and restore preserve every field.
- Player data reaches a Spot with the Actor that joins it; do not request it per player at Spot entry. If a Spot truly needs extra data, fetch it for all players in one request before the game starts.
- Keep game rules as synchronous domain code with no ZLink or database calls. Reach the database only through capability ports named after game events (`loadPlayer`, `commitRound`), never a generic CRUD repository. Keep the domain model (rounds, hands, wallets) separate from message DTOs and database rows, converting once at the handler and adapter boundary. Do not add pass-through layers or a service layer outside Spots and Actors.
- Decide what remains in the database if the process dies now, and how startup cleans it up. Retry only on the "being removed" reply; return other failures to the caller.
- Do not block, sleep, retry or raise timeouts in handlers; ZLink waits for remote connections. Report ZLink defects with a minimal repro instead of compensating in game code.
- Send money or reward results only after the database commit.
- One decision has one owner; find the existing one before adding another. Take test expectations from the spec, not from the implementation.

Details: [Responsibilities of Play and API Nodes](https://docs.zlink.systems/java/guide/server/03-concepts/#10-stateful-game-nodes-and-the-http-lobby)

## Local server and E2E

Run one scenario with `./run-e2e.sh --scenario matchmaking`. Close the connection only after receiving the reply to the last request. The server may miss a send that is immediately followed by close.

Start the server with `./run-server.sh` or `.\run-server.cmd` on Windows. Use its reported `ws://` endpoint and press Ctrl+C in its terminal to stop the nodes and Redis.

Run `./run-e2e.sh` or `.\run-e2e.cmd` in another terminal. The runner attaches using `.logs/server.json` while the server is running and otherwise starts an isolated Docker stack. The [README](README.md#local-server) describes the checks that require isolation. Use `.logs/run-<id>/` for stdout, stderr, Framework message-flow logs and failure details. Docker build and cleanup output is under `.logs/docker-*/`.

## Project and commands

This Java 25 project runs a Session node, play nodes A and B (match), and a Redis Location Store. Entry Spots forward players to the fixed Matchmaking User Spot; same-mode players enter dynamic Room User Spots. Public Framework and connector versions are pinned to 0.34.2 in `gradle/libs.versions.toml`. The [README](README.md) owns execution and deployment instructions. [zlink.structure.json](zlink.structure.json) describes the topology and scenario order.

Run commands from this project root with Docker Compose v2 and Linux containers:

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario matchmaking,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
./gradlew --no-daemon --max-workers=4 :server:match:test
```

PowerShell 7 provides the same runner commands:

```powershell
.\run-e2e.cmd -BuildOnly
.\run-e2e.cmd
.\run-e2e.cmd -Scenario matchmaking,hit,wall,input,reconnect,update
.\run-e2e.cmd -CheckStructure
```

For host-process debugging, install JDK 25 and add `--local` or `-Local`. The scripts preserve logs under `.logs/`; inspect the first failing run before rerunning it.

## Modules and ownership

1. When creating a typed message, declare nullability and wire types in the shared contract.
   - Mark a nullable record component with `@Nullable`. Use `@ZLinkJsonOptional` separately to allow omission.
   - Encode 64-bit integers as decimal strings in JSON. Browser clients also send strings, for example `"handNumber": "42"`. [Message value rules](https://docs.zlink.systems/java/guide/server/20-channel-messaging/#12-message-value-rules)

- `server/shared/src/main/java/MyGame/Geometry.java` owns Vec3, closed slab ray/AABB intersection, map bounds, walls and spawns. The adjacent typed records and `Rules.java` own shared message names and limits.
- `server/match/src/main/java/MyGame/match/domain/RoomState.java` is the sole owner of position, HP, phase, winner, shared sequences, movement budget, fire interval and hit selection. Production uses a monotonic clock; tests inject a clock through the domain constructor. `RoomSnapshot` stores remaining milliseconds, not process clock readings.
- `server/match/src/main/java/MyGame/match/MatchmakingState.java` owns mode queues and pairing. `match/Actions.java`, `match/Room.java` and `match/RoomTransfer.java` adapt the domain to public Framework callbacks and relocation.
- `server/session/src/main/java/MyGame/session/GameSession.java` owns the client packet allowlist and Actor binding. Authenticate a claimed QueueRequest identity before deployment. Do not relay internal assignment or result packets.
- `e2e/src/main/java/MyGame/e2e/Scenarios.java` checks matchmaking, hit, wall, input, reconnect and update observations. `server/match/src/test/java/MyGame/match/domain/RoomStateTest.java` covers the 46 domain boundaries; `DomainTest.java` checks matchmaking-state serialization.

## Domain rules

Y is up, map bounds are X/Z=0..100 and Y=0..10, and feet remain at Y=0. The player AABB is 1 by 2 by 1 with eye height 1.6. Vertical movement is rejected as `vertical-move`; jumping and gravity are not implemented. Movement tests the feet-position segment against closed wall boxes, including endpoint contact.

Movement is limited to a step of 2 and a budget of 10 units per second, capped at 2 since the last accepted movement. Shooting normalizes finite nonzero aim from the authoritative eye position, with an 80-unit range, 25 damage and a 250 ms interval. The closest wall or living player AABB wins; a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejections preserve position, HP, sequence and timing. Clients may resend the same input after the server-provided `RetryAfterMs`.

Current server state determines hits. There is no lag compensation, projectile simulation or client prediction. Extend `RoomState`, `Geometry` and typed contracts for game changes; keep each decision in its existing owner. The default typed Framework serializer handles messages. Use public Actor and Spot APIs and leave routing, binding, membership, completion and relocation decisions to Framework.

## Diagnostics and defects

Keep the first `.logs/run-<id>/` settings and node logs for a failing run. Compare message-flow operation, Actor, flow/corr and transition order. One isolated failure remains a defect even if a later run passes. Do not increase timeouts, add fixed waits or weaken scenario assertions to hide failures.

The Framework message model carries only finite JSON numbers. Nonfinite inputs are exercised by `RoomStateTest` rather than a typed-stream scenario.

If a documented public API behaves differently from its contract, preserve the package versions, OS, command or minimal public API reproduction, expected result, first node exception and relevant flow records. Check existing ZLink issues before proposing a report. Remove credentials and private data from the report.
