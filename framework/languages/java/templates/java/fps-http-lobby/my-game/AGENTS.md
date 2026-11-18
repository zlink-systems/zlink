# MyGame FPS development guide

## Rules for AI

`server/lobby` is an HTTP lobby that holds no game state; every other server process is a stateful ZLink node. Before writing code, decide which kind you are editing.

- A game node is not a REST server. A player's state is owned by one session-bound Actor (or the Spot that owns a shared value). Load it once on connect; do not read the database while handling a request.
- Write only results to the database, one transaction per round. Check duplicate requests in Actor or Spot memory.
- While a game is running, a Spot handler never awaits a remote request or a database reply: the await holds the Spot's turn and stops every message and timer in it. When a round ends, the Spot writes the results of all its Actors in one transaction, then tells each Actor after the commit.
- Values many players see live in node memory or one owning Spot; values many requests update are applied by that one owning Spot. A process that does not own a value sends a request to its owner instead of writing it.
- Spots and Actors already process one message at a time. Do not guard their state with locks, semaphores or process-wide static stores, and do not update memory from database commit hooks; the writer updates its owner's memory after commit, others send a message to the owner.
- Split roles so the game can scale out from day one: game nodes host lobby and room Spots and player Actors and are the only processes you add; a shared node hosts only game-wide Instance Spots (matchmaking, game-wide values), and game nodes do not register those factories. Send and request by Spot ID only; placement is configuration. Keep shared-Spot values recoverable from room resends or a database read at startup.
- Never have two Actors or Spots wait on requests to each other inside a turn; notify with send and let the state owner decide.
- Actors move between nodes when they join a Spot or relocate. Keep all mutable Actor fields in one state record, register stateful Actors with the state-preserving relocation policy, and test that save and restore preserve every field.
- Player data reaches a Spot with the Actor that joins it; do not request it per player at Spot entry. If a Spot truly needs extra data, fetch it for all players in one request before the game starts.
- Keep game rules as synchronous domain code with no ZLink or database calls. Reach the database only through capability ports named after game events (`loadPlayer`, `commitRound`), never a generic CRUD repository. Keep the domain model (rounds, hands, wallets) separate from message DTOs and database rows, converting once at the handler and adapter boundary. Do not add pass-through layers or a service layer outside Spots and Actors.
- Decide what remains in the database if the process dies now, and how startup cleans it up. Retry only on the "being removed" reply; return other failures to the caller.
- Do not block, sleep, retry or raise timeouts in handlers; ZLink waits for remote connections. Report ZLink defects with a minimal repro instead of compensating in game code.
- Send money or reward results only after the database commit.
- One decision has one owner; find the existing one before adding another. Take test expectations from the spec, not from the implementation.

The HTTP lobby may read and write the database per request, but it holds no game state and changes game state only by sending requests to game nodes.

Details: [Stateful game nodes and the HTTP lobby](https://docs.zlink.systems/java/guide/server/03-concepts/#10-stateful-game-nodes-and-the-http-lobby)

## Project and commands

This Java 25 project runs an HTTP Lobby, a Session gateway, Match hosts A and B, and a Redis Location Store. The Lobby pairs `POST /match` requests by mode, assigns a GUID Room ID and prepares the member list for a dynamic Room User Spot. Players connect to Session over STREAM and join the assigned Room. Public Framework and connector versions are pinned to 0.32.0 in `gradle/libs.versions.toml`. The [README](README.md) owns execution and deployment instructions. [zlink.structure.json](zlink.structure.json) describes the topology and scenario order.

Run commands from this project root with Docker Compose v2 and Linux containers:

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
./gradlew --no-daemon :server:match:test :server:lobby:test
```

PowerShell 7 provides the same runner commands:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

For host-process debugging, install JDK 25 and add `--local` or `-Local`. The scripts preserve logs under `.logs/`; inspect the first failing run before rerunning it.

## Modules and ownership

- `server/shared/src/main/java/MyGame/` contains typed game contracts, HTTP request/response contracts, limits and protocol names. `Geometry.java` owns Vec3, closed slab ray/AABB intersection, map bounds, walls and spawns.
- `server/match/src/main/java/MyGame/match/domain/RoomState.java` is the sole owner of position, HP, phase, winner, shared sequences, movement budget, fire interval and hit selection. Production uses a monotonic clock; tests inject a clock through the domain constructor. `RoomSnapshot` stores remaining milliseconds, not process clock readings.
- `server/lobby/src/main/java/MyGame/lobby/application/Matchmaker.java` owns mode queues and pairing. It is the single Room ID issuer and creates a RoomPrefix plus a hyphen-free UUID. `RoomProvisioner.java` sends `PrepareRoom` and waits for `RoomPrepared` before returning the HTTP assignment.
- `server/match/src/main/java/MyGame/match/domain/RoomRoster.java` is the sole owner of the prepared member list and admission decision. Room relocation captures `{Game, Roster}` together. Keep Framework callbacks out of this domain.
- `server/session/src/main/java/MyGame/session/GameSession.java` owns the client packet allowlist and Actor binding. The first normal client packet is JoinRoomRequest. Authenticate its claimed player ID before deployment. Do not relay preparation, readiness, assignment or result packets from clients.
- `e2e/src/main/java/MyGame/e2e/Scenarios.java` checks lobby, hit, wall, input, reconnect and update observations. `server/match/src/test/java/MyGame/match/domain/RoomStateTest.java` covers 46 game-domain boundaries; `server/lobby/src/test/java/MyGame/lobby/application/LobbyDomainTest.java` covers 19 lobby-domain boundaries.

The HTTP handler injects the public Java `ZLinkRouteClient` and uses `requestToSpot` for Room preparation. `ZLinkSpotOutbound` is limited to Spot callbacks.

## Domain rules

Y is up, map bounds are X/Z=0..100 and Y=0..10, and feet remain at Y=0. The player AABB is 1 by 2 by 1 with eye height 1.6. Vertical movement is rejected as `vertical-move`; jumping and gravity are not implemented. Movement tests the feet-position segment against closed wall boxes, including endpoint contact.

Movement is limited to a step of 2 and a budget of 10 units per second, capped at 2 since the last accepted movement. Shooting normalizes finite nonzero aim from the authoritative eye position, with an 80-unit range, 25 damage and a 250 ms interval. The closest wall or living player AABB wins; a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejections preserve position, HP, sequence and timing. Clients may resend the same input after the server-provided `RetryAfterMs`.

`POST /match` defaults an omitted mode to `duel`. The response contains RoomId and SessionEndpoint. The Matchmaker issues IDs; no process counter is used. The Lobby returns only after RoomPrepared. Repeating preparation with the same member set is idempotent; a different set is rejected. The Room snapshot includes game and roster state so relocation preserves admission.

Current server state determines hits. There is no lag compensation, projectile simulation or client prediction. Extend `RoomState`, `RoomRoster`, `Geometry` and typed contracts for game changes; keep each decision in its existing owner. The default typed Framework serializer handles messages. Use public Actor and Spot APIs and leave routing, binding, membership, completion and relocation decisions to Framework.

## Diagnostics and defects

Keep the first `.logs/run-<id>/` settings and node logs for a failing run. Compare message-flow operation, Actor, flow/corr and transition order. One isolated failure remains a defect even if a later run passes. Do not increase timeouts, add fixed waits or weaken scenario assertions to hide failures.

If a documented public API behaves differently from its contract, preserve the package versions, OS, command or minimal public API reproduction, expected result, first node exception and relevant flow records. Check existing ZLink issues before proposing a report. Remove credentials and private data from the report.
