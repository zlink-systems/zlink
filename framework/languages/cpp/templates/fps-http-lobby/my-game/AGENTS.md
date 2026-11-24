# MyGame FPS development guide

## Client connection

[README Client connection](README.md#client-connection) owns the client endpoint, first flow, connector choices and C++ example. Keep it consistent with this template's `server/config`, startup readiness, `e2e/client.hpp`, `e2e/scenarios.hpp` and DTOs. Use `bootstrap.cmake` key `ZLINK_FRAMEWORK_CPP_VERSION` for release matching; do not hardcode a release in these documents. Preserve the first operation and send/push semantics, and verify examples against the public connector API.

## Rules for AI

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

Details: [Responsibilities of Play and API Nodes](https://docs.zlink.systems/cpp/guide/server/03-concepts/#10-stateful-game-nodes-and-the-http-lobby)

## New game projects

This template is a reference. To build a new game, create a new project and design its nodes, Spots, Actors and messages for that game, using this template's structure and rules as reference. Keep this template intact as you build the new project.

## Server and e2e commands

Run one scenario with `./run-e2e.sh --scenario lobby`. Close the connection only after receiving the reply to the last request. The server may miss a send that is immediately followed by close.

Use `./run-server.sh` in Bash or `.\run-server.cmd` on Windows to build and start Redis and the nodes. The foreground process prints the client address and stops its children on Ctrl+C. In another terminal, use `./run-e2e.sh` or `.\run-e2e.cmd` to attach through `.logs/server.json` or start an isolated Docker run. The runner reports which scenarios require isolation. Each run writes node stdout, stderr, Framework message-flow and failure details under `.logs/run-<id>/`. See [README](README.md) for prerequisites and options.

## Project and development

Extend this C++20 FPS template with the game rules in this project. It runs an API node, a Session node, play nodes A and B (match), and a Redis Location Store. The API node pairs `POST /match` requests by mode, creates a Room ID from the configured prefix and a UUID, and waits for `RoomPrepared` before returning the Room ID and Session endpoint. Session accepts `JoinRoomRequest` as the first game packet; `RoomRoster` owns admission. Public Framework and connector packages are pinned to 0.34.0 in `bootstrap.cmake`. The [README](README.md) owns execution, deployment and known issues; [zlink.structure.json](zlink.structure.json) describes topology and scenario order.

Run from this project root with Docker Compose v2 and Linux containers:

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
```

PowerShell 7 provides the same commands:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

For host-process debugging, install a C++20 compiler (GCC 13 or later, or Visual Studio 2022 17.4 or later in a developer shell on Windows), CMake 3.24 or later and Ninja. Add `--local` or `-Local`; after a local build, `--no-build` or `-NoBuild` skips compilation. The runner executes 46 game-domain checks and 19 lobby-domain checks before scenarios. After a local build, `ctest --test-dir build --output-on-failure` runs both domain targets.

## Modules and extension points

1. When creating a typed message, declare nullability and wire types in the shared contract.
   - Use `std::optional<T>` in the DTO for nullable fields. To allow omission, separately mark the field `zlink::json::defaulted` in `dto_traits`.
   - Encode 64-bit integers as decimal strings in JSON. Browser clients also send strings, for example `"handNumber": "42"`. [Message value rules](https://docs.zlink.systems/cpp/guide/server/20-channel-messaging/#12-message-value-rules)

- `server/shared/contracts.hpp` and `server/match/domain.hpp`: game contracts, limits, geometry, map, spawns, player boxes and `RoomState`. Keep the entry and HTTP template copies byte-identical.
- `server/shared/http_lobby_contracts.hpp`: HTTP and server-only preparation contracts. `server/shared/hosting.hpp` publishes HTTP readiness without adding it to the client packet allowlist.
- `server/lobby/matchmaker.hpp`: mode queues, pairing and the single Room ID issuance point. IDs use `RoomPrefix` plus a generated UUID with no counter.
- `server/lobby/http.hpp`: HTTP endpoint and Room provisioning. The API node sends `PrepareRoom(MatchId, Members)` and responds only after matching `RoomPrepared(MatchId)` arrives.
- `server/match/roster.hpp`: Room roster validation, idempotent preparation, admission and relocation snapshot state. A different roster cannot replace a prepared roster.
- `server/match/room.hpp` and `server/match/player.hpp`: Room handlers, Player Actor, reconnect and relocation. Keep Framework callbacks out of the game domain.
- `server/session/session.hpp`: connection callbacks, Actor binding and client packet allowlist. Authenticate `JoinRoomRequest.PlayerId` before binding. Do not relay `PrepareRoom`, `RoomPrepared`, `HttpNodeReady` or internal result packets from clients.
- `server/shared/config.hpp` and `server/shared/hosting.hpp`: process settings, diagnostics, drain and shutdown observation.
- `e2e/scenarios.hpp`: client observations for lobby, hit, wall, input, reconnect and update. `e2e/domain_test.cpp` and `e2e/lobby_domain_test.cpp` run before scenarios and as CTest targets. `e2e/structure.hpp` checks the descriptor and configuration before infrastructure starts. `e2e/client.hpp`, `process.hpp` and `world.hpp` own client, process and infrastructure lifecycle.

## Domain ownership and asynchronous execution

`RoomState` is the only owner of movement budget, fire interval, hit selection, HP, input sequences and match completion. Y is up; the map bounds and two wall AABBs are shared server constants. Movement checks the feet-position segment against closed wall boxes. Feet remain at Y=0; nonzero `Delta.Y` is `vertical-move`. Jumping and gravity are not implemented. Ray contact with a box face counts as an intersection, and a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejected inputs leave game state and timing unchanged. Snapshots store remaining milliseconds and no absolute process clock readings.

`RoomRoster` owns the prepared member list and admission decision. The API node sends one roster for a paired Room. The same preparation is idempotent; a different roster is rejected. Relocation captures `{Game, Roster}` so a moved Room keeps its members and combat state. `Matchmaker` owns mode separation and Room ID issuance. Pending HTTP requests remain in Lobby process memory and do not move with a play node (match).

Keep game state in the Spot domain and use public Actor, Spot and RouteMesh APIs. Framework execution serializes handler turns; do not add another lock, queue or generation check for the same execution permission. Do not synchronously wait for infrastructure inside a handler turn. Relocation adapters serialize application state; Framework owns host transfer, membership, location and route continuity. Validate relocation completion and safe-shutdown observations, not command acceptance alone.

## Public API references

Use the [ZLink documentation](https://docs.zlink.systems/), [C++ server guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/) and [agent index](https://docs.zlink.systems/llms.txt). Check documentation against the package pinned by `bootstrap.cmake`. Entry, Spot, Actor, Session, Location and relocation operations use public Framework APIs and default typed serialization. Do not add a private API, raw-frame parser, manual JSON conversion or application routing workaround.

The pinned 0.34.0 C++ public headers currently do not match the documented typed `SessionInfoRequest` reply example: STREAM write/reply calls accept `zlink::message_t`, `fx::message_t::from(T)` returns a typed wrapper, and `submit()` returns `void`. Preserve request/reply semantics and record this as a build blocker until the public contract is aligned; do not turn the reply into a push or add a serializer workaround.

## Diagnostics and defects

Dispatch diagnostics use Normal mode in `server/shared/hosting.hpp`; node stderr contains message-flow records. Preserve the first `.logs/run-<id>/` settings, stdout and stderr before changing a failing case. Compare operation, Actor, flow/corr and transition order. A single isolated failure remains a defect even if a later run passes. Preserve startup exceptions when no flow record exists yet.

For local Spot discovery diagnostics, set `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` before launching nodes. Docker nodes do not inherit arbitrary host environment variables. Do not change timeouts, fixed waits or assertions to hide failures. The runner cleans up its own processes and Compose project; retain cleanup errors and do not stop other workloads.

If a documented public API fails, preserve package version, OS, command or minimal public API reproduction, expected result, first node exception and relevant flow/corr records. Compare the public guide and header signatures before making a change. Do not add an application workaround for a public contract mismatch.
