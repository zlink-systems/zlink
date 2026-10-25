# MyGame FPS development guide

## Project and development

Extend this template's domain to build an FPS server. This C++20 project runs a Session gateway, Match hosts A and B, and a Redis Location Store. Entry Spots forward players to a fixed Matchmaking User Spot; same-mode players join dynamic Room User Spots. Public Framework and connector packages are pinned to 0.30.2 in `bootstrap.cmake`. The [README](README.md) owns execution, deployment and known issues; [zlink.structure.json](zlink.structure.json) describes the initial topology and ordered scenarios.

Run from this project root with Docker Compose v2 and Linux containers:

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario matchmaking,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
```

PowerShell 7 provides the same commands:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario matchmaking,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

For host process debugging, install a C++20 compiler (GCC 13 or later, or Visual Studio 2022 17.4 or later in a Visual Studio developer shell on Windows), CMake 3.24 or later, and Ninja. Add `--local` or `-Local`; after a local build, `--no-build` or `-NoBuild` skips compilation. Keep scenario assertions and inspect the first failure before rerunning.

## Modules and extension points

- `server/shared/contracts.hpp`: typed client and internal contracts, action limits and protocol names. `server/match/domain.hpp`: Vec3, closed slab ray/AABB intersection, static map, spawn points, player boxes and RoomState. `server/match/matchmaking.hpp`: mode queues and Room ID sequence state used by the Entry Matchmaking Spot.
- `server/match/domain.hpp` (`RoomState`): authoritative positions, HP, phase, winner, shared sequences, movement budgets, fire cooldowns and hit selection. Extend this domain for weapons, scoring and match rules. Production time is monotonic; only the domain constructor accepts a test clock. Snapshot values contain remaining milliseconds instead of process clock readings.
- `server/match/player.hpp` and `server/match/spots.hpp`: Entry, Matchmaking, Room, Player Actor, handlers and relocation adapters. Keep Framework callbacks out of the domain.
- `server/session/session.hpp`: connection callbacks, Actor binding and client packet allowlist. Treat the claimed `QueueRequest.playerId` as untrusted and authenticate it before binding. Do not relay internal assignment or result packets from clients.
- `server/shared/config.hpp` and `server/shared/hosting.hpp`: process settings, hosting, diagnostics, drain and shutdown observation. Keep Framework ownership of routing, membership, binding and relocation.
- `e2e/scenarios.hpp`: client observations for matchmaking, hit, wall, input, reconnect and update. `e2e/domain_test.cpp`: domain boundaries, run before the scenarios and as `my-game-domain-test`. `e2e/structure.hpp`: descriptor and configuration checks before infrastructure starts. `e2e/client.hpp`, `process.hpp` and `world.hpp`: client, process and infrastructure lifecycle.

## Domain ownership and asynchronous execution

RoomState is the only owner of movement budget, fire interval and closest-hit decisions. Y is up; map bounds and fixed wall AABBs are shared server constants. Movement uses the feet-position segment. Feet remain at Y=0; nonzero `delta.y` is rejected as `vertical-move`. Jumping and gravity are not implemented. Contact with a box face counts as intersection, and a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejected inputs leave state and timing unchanged. RoomView exposes last sequences so clients continue after reconnect.

Current server state determines hits. The template includes no lag compensation, projectiles or client prediction. Extend the existing geometry, rules and typed contracts instead of adding another decision owner in handlers or clients.

Keep game state in the Spot domain and use public Actor and Spot APIs. Framework execution serializes handler turns; do not add another lock, queue or generation check for the same execution permission. Do not synchronously wait for infrastructure inside a handler turn. Public async calls from application handlers remain permitted. Relocation adapters serialize game state; Framework owns host transfer, location and route continuity. Validate relocation completion and safe shutdown observations, not command acceptance alone.

## Public API references

Use the [ZLink documentation](https://docs.zlink.systems/), [C++ server guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/) and [agent index](https://docs.zlink.systems/llms.txt). Check current documentation against the pinned package before using an API. Entry, Spot, Actor, Session, Location and relocation operations use public Framework APIs and default typed serialization. Do not use private APIs, raw frames or an application routing workaround.

The pinned 0.30.2 headers do not match the documented typed `SessionInfoRequest` reply example: STREAM write/reply calls accept `zlink::message_t`, `fx::message_t::from(T)` returns a typed wrapper, and `submit()` returns `void`. Preserve request/reply semantics and default typed serialization; do not turn the reply into a push or add manual encoding to work around the public contract mismatch.

## Diagnostics and defects

Dispatch diagnostics use Normal mode in `server/shared/hosting.hpp`; node stderr contains message-flow records. Preserve the first `.logs/run-<id>/` settings, stdout and stderr before modifying or rerunning a failing case. Compare operation, Actor, flow/corr and transition order between passing and failing traces. A single isolated failure remains a defect even if a later run passes. Preserve startup exceptions when no flow record exists yet.

For local Spot discovery diagnostics, set `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` before launching nodes. Docker nodes do not inherit arbitrary host environment variables. Do not change timeout, fixed waits or assertions to hide failures. The runner cleans up its own processes and Compose project; retain cleanup errors and do not stop other workloads.

If a documented public API fails, preserve package version, OS, command or minimal public API reproduction, expected result, first node exception and relevant flow/corr records. Check [existing issues](https://github.com/zlink-systems/zlink/issues) before proposing a report. Remove credentials and private data. Show the draft to the developer before publishing an issue; document confirmed affected versions in the README's Known issues.
