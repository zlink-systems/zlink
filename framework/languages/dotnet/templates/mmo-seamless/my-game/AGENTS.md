# MyGame development guide

## What this is

This C#/.NET 8 project implements a seamless MMO world with a Session gateway,
two Zone hosts, a Dungeon host, and Redis Location and Relocation stores.
`Directory.Packages.props` pins the ZLink Framework and connector packages to `0.30.0`.
Read [README.md](README.md) for prerequisites, execution, deployment, and Known issues.
Read [zlink.structure.json](zlink.structure.json) for the initial composition and scenario list.
Runtime placement belongs to Framework and Redis; the descriptor records initial composition.

## Commands

Run from this directory with the prerequisites listed in README installed.
Build before executing the compiled client. The solution includes every server role
and the e2e client. There is no separate unit-test project or formatter configuration.

PowerShell 7 on Windows:

```powershell
dotnet build server/MyGame.sln
# Run all scenarios, including a build.
pwsh -NoProfile -File ./run-e2e.ps1
# Select scenarios in canonical order.
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario cross,border,reconnect
# Reuse the existing build.
pwsh -NoProfile -File ./run-e2e.ps1 -NoBuild
# Check structure without Docker or server startup.
dotnet e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll --root . --check-structure
```

Bash on Linux or WSL:

```bash
dotnet build server/MyGame.sln
./run-e2e.sh
./run-e2e.sh --scenario cross,border,reconnect
./run-e2e.sh --no-build
dotnet e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll --root . --check-structure
```

Put Bash `--no-build` first when combining it with `--scenario`.
Keep `cross,border,reconnect,update` in `e2e/MyGame.E2E/Scenarios.cs` and the descriptor consistent.
Run the structure check and affected scenarios after changes.
Check [Known issues](README.md#known-issues--알려진-문제) before interpreting an e2e failure.
For deployment and runner behavior, follow [README.md](README.md#local-e2e).

## Project map

| Project | Main files | Owned state | Game logic extension point |
| --- | --- | --- | --- |
| `MyGame.Session` | `server/MyGame.Session/Program.cs`, `server/MyGame.Session/PlayerSession.cs` | Connection callbacks and access to Framework Session binding. | Authenticate before binding; retain the typed packet relay. |
| `MyGame.Zone` | `server/MyGame.Zone/Program.cs`, `server/MyGame.Zone/WorldEntry.cs`, `server/MyGame.Zone/PlayerActor.cs`, `server/MyGame.Zone/ZoneSpot.cs`, `server/MyGame.Zone/Handlers.cs` | `PlayerActor` owns coordinate and sequence. `ZoneSpot` owns visibility projections and pending arrival intents. | Add player rules to Actor handlers and shared Zone rules to the Spot. |
| `MyGame.Instance` | `server/MyGame.Instance/Program.cs`, `server/MyGame.Instance/DungeonSpot.cs` | Dynamic Dungeon User Spots; currently only creation/info requests. | Add party admission, encounters, rewards, and Dungeon lifecycle. |
| `MyGame.Shared` | `server/MyGame.Shared/Contracts.cs`, `server/MyGame.Shared/NodeSettings.cs`, `server/MyGame.Shared/GameHosting.cs`, `server/MyGame.Shared/NodeControl.cs` | Shared contracts, process configuration, and hosting; no player authority. | Add contracts used by several processes; configure diagnostics here. |
| `MyGame.E2E` | `e2e/MyGame.E2E/GameClient.cs`, `e2e/MyGame.E2E/Scenarios.cs`, `e2e/MyGame.E2E/WorldRun.cs`, `e2e/MyGame.E2E/StructureCheck.cs` | Client observations and infrastructure/process ownership for one run. | Add observable gameplay scenarios through the public connector. |

Keep composition changes consistent with `server/config/` and `zlink.structure.json`.

## ZLink concepts used here

Start with the [documentation home](https://docs.zlink.systems/),
[.NET guide home](https://docs.zlink.systems/dotnet/guide/server/),
[.NET API reference](https://docs.zlink.systems/dotnet/reference/), and
[agent index](https://docs.zlink.systems/llms.txt).
Use the pinned package's API when a current guide describes a newer release.

- **RouteMesh and channels.** `GameNames.WorldMesh` connects the processes.
  The `border` channel carries typed publications between Zones.
  Read [Channel messaging](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/).
- **Spot.** `WorldEntry` admits player Actors; `ZoneSpot` hosts geographical Zones.
  `DungeonSpot` is a dynamic User Spot, not a Framework Instance Spot.
  Read [Spot](https://docs.zlink.systems/dotnet/guide/server/21-spot/).
- **Actor.** `PlayerActor` keeps player movement state while its host Spot changes.
  Read [Actor](https://docs.zlink.systems/dotnet/guide/server/22-actor/).
- **STREAM and Session binding.** `PlayerSession` obtains the Player Actor, binds it,
  and relays the original typed packet. Actor handlers push through the bound Session.
  Read [STREAM](https://docs.zlink.systems/dotnet/guide/server/23-stream/) and
  [Session and Actor](https://docs.zlink.systems/dotnet/guide/server/24-actor-session/).
- **Location Store.** Redis records current runtime locations; application code addresses
  Actors and Spots by ID. Read [Location](https://docs.zlink.systems/dotnet/guide/server/25-location/).
- **Membership changes.** `PlayerActor.Move` requests deferred admission to the next Zone;
  `OnJoinCompletedAsync` observes completion. Read
  [Actor membership](https://docs.zlink.systems/dotnet/guide/server/35-actor-membership/).
- **Relocation and drain.** Application adapters capture game state; Framework transfers
  workload and converges routes. `NodeControl` requests relocation and observes safe shutdown.
  Read [Relocation](https://docs.zlink.systems/dotnet/guide/server/37-relocation/).
- **Pub/sub and timers.** `BorderTick` publishes presence; `BorderHandler` receives it.
  `BorderDelivery` forwards notifications through each receiving Actor's bound Session.
  Read [Timers and workers](https://docs.zlink.systems/dotnet/guide/server/36-timer-worker/)
  and the channel guide above for publication and subscription.

## Adding a feature

Use item pickup within a Zone as one complete feature. The names below describe new
application types to add; the Framework calls shown already exist in this template.

1. Add typed `PickupRequest` and `ItemPickedUp` records to
   `server/MyGame.Shared/Contracts.cs`. Include the item ID and game result;
   obtain the requesting Actor's identity from the handler argument.
2. Add `PickupHandler` to `server/MyGame.Zone/Handlers.cs`. Follow `MoveHandler`:
   apply `ZLinkSpotActorSendHandler(nameof(PickupRequest))`, implement
   `IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, PickupRequest>`, and use its
   existing `HandleAsync` signature. Assembly scanning is already registered in
   `server/MyGame.Zone/Program.cs`:

   ```csharp
   options.AddHandlersFromAssemblyOf<ZoneSpot>();
   ```

3. Let `ZoneSpot` own available ground items and validate proximity using `PlayerActor.X`.
   Let `PlayerActor` own collected inventory. Update both in the same `SpotWide` turn;
   reject an unavailable item before changing either owner. Keep Session code as a relay.
4. Send the result to the supplied Actor's bound Session. Follow `BorderDelivery`'s
   existing typed-send form, replacing its application message with the pickup result:

   ```csharp
   await actor.Context.BoundSession.Send(presence).Async();
   ```

   For pushes to other players, store player IDs and follow `ZoneSpot.ShowBorderAsync`:
   use the public Actor client, then forward in a handler like `BorderDelivery`.
5. Extend `PlayerSnapshot`, `PlayerActor.Capture` and `Restore` in
   `server/MyGame.Zone/PlayerActor.cs` for inventory. Extend `ZoneSnapshot`,
   `ZoneSpot.Capture` and `Restore` in `server/MyGame.Zone/ZoneSpot.cs` for remaining items.
   `PlayerRelocation` and `ZoneRelocation` already serialize those snapshots.
   Transfer only application state; omit route, lease, membership, and queue fields.
6. There are no existing unit tests. If extracting pure pickup rules into unit tests,
   test unavailable items and double pickup as well as success.
7. Add public connector send/wait support in `e2e/MyGame.E2E/GameClient.cs`, following
   `MoveAsync`: register the typed wait before sending. Add the scenario and selection
   case in `e2e/MyGame.E2E/Scenarios.cs`; update `run.scenarios` in `zlink.structure.json`.
   Assert the received result, duplicate rejection, and inventory preservation across
   reconnect and relocation. Retain the existing four scenarios and their assertions.
8. Build, check structure, and run affected scenarios. Preserve the first failure;
   distinguish application usage errors from the package blockers documented in README.

## Development notes

- Use public NuGet packages at the centrally pinned versions. Project references may
  point only to this project's own projects.
- Use typed messages and the default Framework JSON serializer. Relocation adapters may
  serialize application snapshots; they must not serialize Framework runtime state.
- Keep one authoritative owner for each game fact. Treat Zone presence as a projection
  of player movement. Store player IDs and projections in Spots, never Actor references.
- Keep the Session relay visible. Put movement and gameplay decisions in the owning
  Actor or Spot, after authentication and binding.
- Do not block handlers with `.Wait()`, `.Result`, or synchronous I/O. Zone registration
  uses `SpotWide`; its common gate already serializes member Actor handlers, Spot handlers,
  timers, and lifecycle callbacks. Do not add a second lock or queue for the same state.
  Read [Execution model](https://docs.zlink.systems/dotnet/guide/server/32-execution-model/).
- Await short public asynchronous calls as the template does. Suspension can retain the
  current turn; do not wait for work requiring that same turn to finish. Consult the
  execution guide and pinned API before introducing long-running work.
- Keep registration and per-instance callbacks in their defined phases. `DungeonSpot`
  remains a User Spot even though its process is named Instance. Read
  [Activation and lifetime](https://docs.zlink.systems/dotnet/guide/server/34-activation-lifetime/).
- Use `JoinSpot(...).Defer()` for Zone transitions and observe the public completion
  callback. Use public host relocation and runtime observations for drain.
- Reconnect to the same Actor ID and bind the new Session. Do not maintain a route cache
  or reset authoritative player state during reconnect.
- Observe backpressure results instead of adding transport retries or extending deadlines.
  Read [Backpressure](https://docs.zlink.systems/dotnet/guide/server/33-backpressure/).
- Do not compensate for Framework failures with internal APIs, reflection, raw transport
  frames, private codecs, extra pollers, fixed sleeps, or weakened assertions.
  Wait for actual replies, events, or lifecycle observations.
- Write code comments in English. Keep `.logs/`, `bin/`, and `obj/` out of commits.

## Debugging

Preserve and read the first failing run before executing it again. A single intermittent
failure is a defect; a subsequent pass does not invalidate it.

- Find each run under `.logs/run-<id>/`. `e2e/MyGame.E2E/WorldRun.cs` writes effective
  `zone-a.json`, `zone-b.json`, `instance.json`, and `session.json` there as each node
  starts. A replaced A uses `zone-a-replacement` as its filename prefix.
- Read `<node>.err.log` for node diagnostics and `<node>.out.log` for readiness/control
  output. `e2e/MyGame.E2E/ManagedProcess.cs` captures both streams. Infrastructure logs
  include `redis-start.log`, `redis-port.log`, `redis.log`, and `redis-stop.log`;
  `.logs/build.log` contains the shell runner's build output.
- Change diagnostics in `server/MyGame.Shared/GameHosting.cs`:

  ```csharp
  options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
  ```

  `Normal` includes receive, dispatch, and completion transitions plus errors.
  Replace it with `Detailed` for timing detail; size recording also requires
  `IncludeMessageSizes(true)` on the diagnostics options. Detailed recording adds cost.
  `server/MyGame.Shared/NodeSettings.cs` has no diagnostics JSON key: edit the hosting
  call, rebuild, and restart.
- `GameHosting` configures standard console logging to stderr. Read message-flow records
  in node `.err.log` files; this template does not create a separate `.flow` file.
  Use `flow` to follow a message across nodes and `corr` to connect request/reply records.
  The generic failure notation is `message_flow_outcome=error`; the .NET console format
  uses `zlink flow:` with `outcome=failed`, `error_type`, and `error_message` instead.
  Inspect those fields and the preceding transition. Preserve the full category output.
- Compare passing and failing runs by logical operation, Actor ID, and transition order;
  flow IDs differ between runs. Find the first missing or failed transition rather than
  comparing only final timeouts. Startup failures can precede message flow; retain the
  startup exception and stack when no flow exists.
- Consult [Monitoring](https://docs.zlink.systems/dotnet/guide/server/26-monitoring/) and
  [Observability diagnostics](https://docs.zlink.systems/dotnet/reference/08-observability-diagnostics/).
  The pinned package exposes `IZLinkDiagnosticsRuntime.Level` and `SetLevelAsync`.
  Use the asynchronous method inside Framework handlers/callbacks; the property setter
  is a blocking compatibility operation. Neither is wired to this project's stdin controls.
- For supplementary discovery tracing, set `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1` in
  the parent environment before starting nodes. This switch string exists in public
  `Zlink.Framework` 0.30.0; treat it as a version-specific diagnostic, not a routing API.
  In PowerShell use `$env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = "1"`; in Bash use
  `export ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`. Preserve stderr alongside message flow.
- Let the runner clean up its own processes and Compose project. Preserve cleanup
  failures and avoid stopping unrelated processes or containers.

## Reporting ZLink defects

Compare your API usage with the linked public guides and samples first.
If Framework behavior blocks the scenario, report it at
[zlink-systems/zlink issues](https://github.com/zlink-systems/zlink/issues).
Include a minimal public API reproduction, package versions from `Directory.Packages.props`,
OS/runtime details, the first failing node-log excerpt, message-flow records with
`flow`/`corr` when available, and the expected observable result.
State when startup failed before flow recording. Link an existing issue for the same defect.
Preserve scenario assertions and report the blocked public operation instead of adding
an application workaround.
