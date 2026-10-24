# MyGame FPS development guide

## Project and development

Extend this template's domain to build an FPS server. This .NET 8 project runs a Session gateway, Match hosts A and B, and a Redis Location Store. Entry Spots forward players to a fixed Matchmaking User Spot; same-mode players join dynamic Room User Spots. Public Framework and connector packages are pinned to 0.30.2 in `Directory.Packages.props`. The [README](README.md) owns execution, deployment and known issues; [zlink.structure.json](zlink.structure.json) describes initial topology and ordered scenarios.

Run from this project root with Docker Compose v2 and Linux containers:

```sh
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario matchmaking,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

PowerShell 7 provides the same commands:

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario matchmaking,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

For host process debugging with the .NET 8 SDK, add `--local` or `-Local`. After a local build, `--no-build` or `-NoBuild` skips compilation. Keep scenario assertions and inspect the first failure before rerunning.

## Modules and extension points

- `server/MyGame.Shared/Contracts.cs`: typed client and internal contracts, action limits and protocol names. `Geometry.cs`: Vec3, closed slab ray/AABB intersection, static map, spawn points and player boxes.
- `server/MyGame.Match/Domain/RoomState.cs`: authoritative positions, HP, phase, winner, shared sequences, movement budgets, cooldowns and hit selection. Extend this domain for weapons, scoring and match rules. Production time is monotonic; only the domain constructor permits a test clock. Snapshot DTOs contain remaining milliseconds rather than process clock readings.
- `server/MyGame.Match/Domain/MatchmakingState.cs`: mode queues and pairing. `Infrastructure/ZLink/`: Entry, Matchmaking, Room, Player Actor and their handlers and relocation adapters. Keep Framework callbacks out of the domain.
- `server/MyGame.Session/PlayerSession.cs`: connection callbacks, Actor binding and client packet allowlist. Authenticate the claimed QueueRequest identity before binding. Never relay internal RoomAssignment or result packets from clients.
- `server/MyGame.Shared/GameHosting.cs`, `NodeSettings.cs`, `NodeControl.cs`: shared setup, diagnostics, configuration, drain and shutdown observation. Retain public Framework ownership of routing, membership, binding and relocation.
- `e2e/MyGame.E2E/Scenarios.cs`: received client results for matchmaking, hit, wall, input, reconnect and update. `DomainChecks.cs`: domain boundaries in the same executable. `StructureCheck.cs`: descriptor/config/contract consistency before infrastructure starts. `MatchRun.cs` and `ManagedProcess.cs`: owned process lifecycle.

## Domain ownership and asynchronous execution

RoomState is the only owner of movement budget, fire interval and closest-hit decisions. Y is up; map bounds and fixed wall AABBs are shared server constants. Movement uses the feet-position segment. Feet remain at Y=0; nonzero Delta.Y is rejected as `vertical-move`. Jumping and gravity are not implemented. Ray contact with a box face counts as intersection, and a wall wins equal-distance ties. Accepted misses and wall-blocked shots consume sequence and cooldown. Rejected inputs leave all state and timing unchanged. RoomView exposes last sequences so new clients continue after reconnect.

Current server state determines hits. The template includes no lag compensation, projectiles or client prediction. Extend the existing geometry, rules and typed contracts instead of adding a second decision owner in handlers or clients.

Keep game state in the Spot domain and use public Actor/Spot APIs. Framework execution serializes handler turns; do not add a second lock, queue or generation check for the same execution permission. Do not synchronously wait for infrastructure inside a handler turn. Public async calls from application handlers remain permitted. Relocation adapters serialize game state; Framework owns host transfer, location and route continuity. Validate relocation completion and safe shutdown observations, not command acceptance alone.

## Public API references

Use the [ZLink documentation](https://docs.zlink.systems/), [.NET server guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/) and [agent index](https://docs.zlink.systems/llms.txt). Check current documentation against the pinned package before using an API. Entry/Spot, Actor, Session, Location and relocation operations use public Framework APIs and default typed serialization. Do not use reflection, private APIs, raw frames or an application routing workaround.

## Diagnostics and defects

Dispatch diagnostics are Normal in `GameHosting.cs`; node stderr contains message-flow records. Preserve the first `.logs/run-<id>/` settings, stdout and stderr before modifying or rerunning a failing case. Compare operation, Actor, flow/corr and transition order between passing and failing traces. A single isolated failure remains a defect even if a later run passes. Preserve startup exceptions when no flow record exists yet.

For local process discovery diagnostics, set `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` before launching nodes. Docker nodes do not inherit arbitrary host environment variables. Do not change timeout, fixed waits or assertions to hide failures. The runner cleans up its own processes and Compose project; retain cleanup errors and do not stop other workloads.

If a documented public API fails, preserve package versions, OS, command or minimal public API reproduction, expected result, first node exception/stack and relevant flow/corr records. Check [existing issues](https://github.com/zlink-systems/zlink/issues) before proposing a report. Remove credentials and private data. Show the draft to the developer before publishing an issue; document confirmed affected versions in the README's Known issues.
