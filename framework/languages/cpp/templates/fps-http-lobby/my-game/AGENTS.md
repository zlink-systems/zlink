# MyGame FPS development guide

## Project and development

Extend this C++20 FPS template with the game rules in this project. It runs an HTTP Lobby, a Session gateway, Match hosts A and B, and a Redis Location Store. The Lobby pairs `POST /match` requests by mode, creates a Room ID from the configured prefix and a UUID, and waits for `RoomPrepared` before returning the Room ID and Session endpoint. Session accepts `JoinRoomRequest` as the first game packet; `RoomRoster` owns admission. Public Framework and connector packages are pinned to 0.30.2 in `bootstrap.cmake`. The [README](README.md) owns execution, deployment and known issues; [zlink.structure.json](zlink.structure.json) describes topology and scenario order.

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

- `server/shared/contracts.hpp` and `server/match/domain.hpp`: game contracts, limits, geometry, map, spawns, player boxes and `RoomState`. Keep the entry and HTTP template copies byte-identical.
- `server/shared/http_lobby_contracts.hpp`: HTTP and server-only preparation contracts. `server/shared/hosting.hpp` publishes HTTP readiness without adding it to the client packet allowlist.
- `server/lobby/matchmaker.hpp`: mode queues, pairing and the single Room ID issuance point. IDs use `RoomPrefix` plus a generated UUID with no counter.
- `server/lobby/http.hpp`: HTTP endpoint and Room provisioning. The Lobby sends `PrepareRoom(MatchId, Members)` and responds only after matching `RoomPrepared(MatchId)` arrives.
- `server/match/roster.hpp`: Room roster validation, idempotent preparation, admission and relocation snapshot state. A different roster cannot replace a prepared roster.
- `server/match/room.hpp` and `server/match/player.hpp`: Room handlers, Player Actor, reconnect and relocation. Keep Framework callbacks out of the game domain.
- `server/session/session.hpp`: connection callbacks, Actor binding and client packet allowlist. Authenticate `JoinRoomRequest.PlayerId` before binding. Do not relay `PrepareRoom`, `RoomPrepared`, `HttpNodeReady` or internal result packets from clients.
- `server/shared/config.hpp` and `server/shared/hosting.hpp`: process settings, diagnostics, drain and shutdown observation.
- `e2e/scenarios.hpp`: client observations for lobby, hit, wall, input, reconnect and update. `e2e/domain_test.cpp` and `e2e/lobby_domain_test.cpp` run before scenarios and as CTest targets. `e2e/structure.hpp` checks the descriptor and configuration before infrastructure starts. `e2e/client.hpp`, `process.hpp` and `world.hpp` own client, process and infrastructure lifecycle.

## Domain ownership and asynchronous execution

`RoomState` is the only owner of movement budget, fire interval, hit selection, HP, input sequences and match completion. Y is up; the map bounds and two wall AABBs are shared server constants. Movement checks the feet-position segment against closed wall boxes. Feet remain at Y=0; nonzero `Delta.Y` is `vertical-move`. Jumping and gravity are not implemented. Ray contact with a box face counts as an intersection, and a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejected inputs leave game state and timing unchanged. Snapshots store remaining milliseconds and no absolute process clock readings.

`RoomRoster` owns the prepared member list and admission decision. The HTTP Lobby sends one roster for a paired Room. The same preparation is idempotent; a different roster is rejected. Relocation captures `{Game, Roster}` so a moved Room keeps its members and combat state. `Matchmaker` owns mode separation and Room ID issuance. Pending HTTP requests remain in Lobby process memory and do not move with a Match host.

Keep game state in the Spot domain and use public Actor, Spot and RouteMesh APIs. Framework execution serializes handler turns; do not add another lock, queue or generation check for the same execution permission. Do not synchronously wait for infrastructure inside a handler turn. Relocation adapters serialize application state; Framework owns host transfer, membership, location and route continuity. Validate relocation completion and safe-shutdown observations, not command acceptance alone.

## Public API references

Use the [ZLink documentation](https://docs.zlink.systems/), [C++ server guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/) and [agent index](https://docs.zlink.systems/llms.txt). Check documentation against the package pinned by `bootstrap.cmake`. Entry, Spot, Actor, Session, Location and relocation operations use public Framework APIs and default typed serialization. Do not add a private API, raw-frame parser, manual JSON conversion or application routing workaround.

The pinned 0.30.2 C++ public headers currently do not match the documented typed `SessionInfoRequest` reply example: STREAM write/reply calls accept `zlink::message_t`, `fx::message_t::from(T)` returns a typed wrapper, and `submit()` returns `void`. Preserve request/reply semantics and record this as a build blocker until the public contract is aligned; do not turn the reply into a push or add a serializer workaround.

## Diagnostics and defects

Dispatch diagnostics use Normal mode in `server/shared/hosting.hpp`; node stderr contains message-flow records. Preserve the first `.logs/run-<id>/` settings, stdout and stderr before changing a failing case. Compare operation, Actor, flow/corr and transition order. A single isolated failure remains a defect even if a later run passes. Preserve startup exceptions when no flow record exists yet.

For local Spot discovery diagnostics, set `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` before launching nodes. Docker nodes do not inherit arbitrary host environment variables. Do not change timeouts, fixed waits or assertions to hide failures. The runner cleans up its own processes and Compose project; retain cleanup errors and do not stop other workloads.

If a documented public API fails, preserve package version, OS, command or minimal public API reproduction, expected result, first node exception and relevant flow/corr records. Compare the public guide and header signatures before making a change. Do not add an application workaround for a public contract mismatch.
