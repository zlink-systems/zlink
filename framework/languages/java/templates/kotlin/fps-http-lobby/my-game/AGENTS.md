# MyGame FPS development guide

## Project and development

This Kotlin project runs an HTTP Lobby, a Session gateway, Match hosts A and B, and a Redis Location Store. The Lobby pairs `POST /match` requests by mode, assigns a UUID Room ID, and prepares the member list of a dynamic Room User Spot before returning the assignment. Players connect to Session over STREAM and join the assigned Room. `build.gradle.kts` pins the public Framework and connector packages. The [README](README.md) owns execution, deployment, FPS rules, and known issues; [zlink.structure.json](zlink.structure.json) describes initial topology and ordered scenarios.

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

For host-process debugging, install JDK 25 and add `--local` or `-Local`. After a local build, `--no-build` or `-NoBuild` skips compilation. Keep scenario assertions and inspect the first failure before rerunning.

## Modules and extension points

- `server/shared/src/main/kotlin/MyGame/Contracts.kt`: typed game contracts, action limits, and protocol names. `HttpLobbyContracts.kt`: HTTP lobby and server-only preparation contracts. `Geometry.kt`: `Vec3`, closed slab ray/AABB intersection, static map, spawn points, and player boxes.
- `server/match/src/main/kotlin/MyGame/match/domain/RoomState.kt`: authoritative positions, HP, phase, winner, shared sequences, movement budgets, cooldowns, and hit selection. Production time is monotonic; only the domain constructor accepts a test clock. Snapshot DTOs store remaining milliseconds rather than process clock readings.
- `server/lobby/src/main/kotlin/MyGame/lobby/application/Matchmaker.kt`: mode queues, pairing, and the single Room ID issuance point. `server/lobby/src/main/kotlin/MyGame/lobby/infrastructure/HttpLobby.kt` creates the Room and waits for its preparation acknowledgement before returning the HTTP assignment. `server/match/src/main/kotlin/MyGame/match/domain/RoomRoster.kt` owns membership and admission. Keep Framework callbacks out of the domain.
- `server/session/src/main/kotlin/MyGame/session/Program.kt`: connection callbacks, Actor binding, and client packet allowlist. Authenticate the claimed player identity before binding. Relay only `JoinRoomRequest`, `RoomViewRequest`, `MoveRequest`, `ShootRequest`, and `SessionInfoRequest`; reject preparation, assignment, and result packets from clients.
- `server/shared/src/main/kotlin/MyGame/Hosting.kt`, `NodeConfig` and `Control.kt`: shared setup, diagnostics, configuration, drain, and shutdown observation. Retain public Framework ownership of routing, membership, binding, and relocation.
- `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt`: received client results for lobby, hit, wall, input, reconnect, and update. `server/match/src/test/.../RoomStateTest.kt` and `server/lobby/src/test/.../LobbyDomainTest.kt` cover game and HTTP lobby boundaries. `Structure.kt` checks descriptor, configuration, and contracts before infrastructure starts. `Nodes.kt` and `DockerRuntime.kt` manage the runner-owned process lifecycle.

## Domain ownership and asynchronous execution

`RoomState` is the only owner of movement budget, fire interval, and closest-hit decisions. Y is up; map bounds and fixed wall AABBs are shared server constants. Movement uses the feet-position segment. Feet remain at Y=0; nonzero `delta.y` is rejected as `vertical-move`. Jumping and gravity are not implemented. Ray contact with a box face counts as intersection, and a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejected inputs leave all state and timing unchanged. `RoomView` exposes last sequences so new clients continue after reconnect.

Current server state determines hits. The template includes no lag compensation, projectiles, or client prediction. Extend the existing geometry, rules, and typed contracts instead of adding a second decision owner in handlers or clients.

Keep game state in the Spot domain and use public Actor/Spot APIs. Framework execution serializes handler turns; do not add a second lock, queue, or generation check for the same execution permission. Do not synchronously wait for infrastructure inside a handler turn. Public async calls from application handlers remain permitted. Relocation adapters serialize game state; Framework owns host transfer, location, and route continuity. Validate relocation completion and safe shutdown observations, not command acceptance alone.

## Public API references

Use the [ZLink documentation](https://docs.zlink.systems/), [Kotlin server guide](https://docs.zlink.systems/kotlin/guide/server/), [API reference](https://docs.zlink.systems/kotlin/reference/), and [agent index](https://docs.zlink.systems/llms.txt). Check current documentation against the pinned package before using an API. Entry/Spot, Actor, Session, Location, and relocation operations use public Framework APIs and the default typed serializer. Do not use reflection, private APIs, raw frames, or an application routing workaround.

## Diagnostics and defects

Dispatch diagnostics are Normal in `Hosting.kt`; node stderr contains message-flow records. Preserve the first `.logs/run-<id>/` settings, stdout, and stderr before modifying or rerunning a failing case. Compare operation, Actor, flow/corr, and transition order between passing and failing traces. A single isolated failure remains a defect even if a later run passes. Preserve startup exceptions when no flow record exists yet.

For local process discovery diagnostics, set `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` before launching nodes. Docker nodes do not inherit arbitrary host environment variables. Do not change timeout, fixed waits, or assertions to hide failures. The runner cleans up its own processes and Compose project; retain cleanup errors and do not stop other workloads.

If a documented public API fails, preserve package versions, OS, command or minimal public API reproduction, expected result, first node exception and stack, and relevant flow/corr records. Check [existing issues](https://github.com/zlink-systems/zlink/issues) before proposing a report. Remove credentials and private data. Show the draft to the developer before publishing an issue; document confirmed affected versions in the README's Known issues.
