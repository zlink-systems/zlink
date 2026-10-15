# MyGame development guide

## Project overview

This C#/.NET 8 project is an MMO server example in which players move across Zone boundaries. It runs a Session gateway for connections, Zone hosts A and B, a Dungeon host, and Redis for location and relocation state. `Directory.Packages.props` pins the ZLink Framework and connector packages to `0.30.0`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [.NET guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in this project.

## Development commands

### Windows PowerShell 7

Run these commands from this directory after installing the prerequisites in the README. The solution includes every server role and the e2e client, so build it before invoking the compiled client directly.

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

`-NoBuild` uses the existing build, while `--check-structure` validates composition without starting Docker or servers. After changes, run the structure check and affected scenarios, and compare failures with [Known issues](README.md#known-issues--알려진-문제).

### Linux and WSL Bash

Bash uses the same solution and e2e client. When combining build reuse with scenario selection, put `--no-build` first because the runner checks its first argument to decide whether to build.

```bash
dotnet build server/MyGame.sln
./run-e2e.sh
./run-e2e.sh --scenario cross,border,reconnect
./run-e2e.sh --no-build
dotnet e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll --root . --check-structure
```

The default scenario order is `cross,border,reconnect,update`; keep the lists in `e2e/MyGame.E2E/Scenarios.cs` and the descriptor consistent. [Local e2e](README.md#local-e2e) explains runner startup and cleanup.

## Project layout and extension points

### Session gateway

`server/MyGame.Session/Program.cs` registers connection handling, and `server/MyGame.Session/PlayerSession.cs` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Zones and players

An Actor is a game state object addressed by ID; `PlayerActor` in `server/MyGame.Zone/PlayerActor.cs` manages the player's coordinate and sequence. A Spot is a state object that accepts Actors and executes shared game rules; `ZoneSpot` in `server/MyGame.Zone/ZoneSpot.cs` manages Zone visibility and pending arrivals. Visibility values are copies of Actor movement state used for observation, so apply player state changes to the Actor.

`server/MyGame.Zone/Program.cs` registers types and handlers, and `server/MyGame.Zone/WorldEntry.cs` handles player Actor entry into the world. Add handlers that combine player rules with shared Zone rules in `server/MyGame.Zone/Handlers.cs`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/MyGame.Instance/Program.cs` and `server/MyGame.Instance/DungeonSpot.cs` create Dungeons on demand and return their information. `DungeonSpot` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract. Add party admission rules, encounters, rewards, and Dungeon lifecycle behavior in this project.

### Shared contracts and hosting

`server/MyGame.Shared/Contracts.cs` defines messages and game constants shared by processes, while `server/MyGame.Shared/NodeSettings.cs` defines process settings. `server/MyGame.Shared/GameHosting.cs` configures hosting and diagnostics, and `server/MyGame.Shared/NodeControl.cs` handles runtime control commands. Add contracts used by several processes here, and keep player state in `PlayerActor` so separate processes do not independently modify the same fact.

### E2E client

`e2e/MyGame.E2E/GameClient.cs` connects through the public connector, and `e2e/MyGame.E2E/Scenarios.cs` checks results observable by the client. `e2e/MyGame.E2E/WorldRun.cs` manages processes and infrastructure for each run, while `e2e/MyGame.E2E/StructureCheck.cs` validates composition. Verify new gameplay through received client results; the project includes no separate unit-test project or formatter configuration.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `GameNames.WorldMesh`. Channels distinguish messages by purpose and name, and the `border` channel carries typed publications between Zones. [Channel messaging](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`WorldEntry` handles Actor entry, `ZoneSpot` represents a geographical Zone, and `DungeonSpot` represents a dynamically created Dungeon. The [Spot guide](https://docs.zlink.systems/dotnet/guide/server/21-spot/) describes registration and calls for these objects. `PlayerActor` retains movement state when its host Spot changes; the [Actor guide](https://docs.zlink.systems/dotnet/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/dotnet/guide/server/23-stream/), which processes packets on client connections, `PlayerSession` obtains the Player Actor, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/dotnet/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/dotnet/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `PlayerActor.Move` requests deferred admission to the next Zone, and `OnJoinCompletedAsync` observes the completion result. [Actor membership](https://docs.zlink.systems/dotnet/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `NodeControl` requests public host relocation and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/dotnet/guide/server/37-relocation/) explains the procedure.

### Border notifications and timers

A timer runs a handler periodically; `BorderTick` uses one to publish information about players near a border. In this pub/sub exchange, `BorderHandler` receives subscribed information, and `BorderDelivery` forwards notifications through the receiving Actor's bound Session. [Timers and workers](https://docs.zlink.systems/dotnet/guide/server/36-timer-worker/) explains registration and execution of periodic work.

## Adding item pickup

### Messages and handlers

For item pickup within a Zone, begin with request and result contracts and a Zone handler. Define new application records `PickupRequest` and `ItemPickedUp` in `Contracts.cs` for the item ID and game result, and obtain the requester's identity from the handler's Actor argument rather than a self-reported packet value.

When adding `PickupHandler` in `Handlers.cs`, follow the `ZLinkSpotActorSendHandler` attribute, `IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, MoveRequest>` interface, and `HandleAsync` signature of `MoveHandler`. Change the attribute's packet name and the interface's message type to match the new request; `Program.cs` already registers assembly scanning with this call.

```csharp
options.AddHandlersFromAssemblyOf<ZoneSpot>();
```

This call registers handlers from the assembly containing `ZoneSpot`, so place the new handler in that assembly. `PickupRequest`, `ItemPickedUp`, and `PickupHandler` are game types to add for this feature.

### State changes and result delivery

A turn is the execution interval in which a handler or callback can modify state; an execution gate limits concurrent turns over that state. The project's `SpotWide` setting gives Spot handlers, member Actor handlers, timers, and lifecycle callbacks in the same Zone a shared gate. The pickup handler changes ground items in `ZoneSpot` and inventory in `PlayerActor` within the same turn to determine whether pickup is duplicated.

If the item exists and its distance from `PlayerActor.X` is allowed, remove it from the ground and add it to inventory. If it is absent or out of range, leave both states unchanged and send a rejection result. For result delivery, follow the typed Session send used in `BorderDelivery`.

```csharp
await actor.Context.BoundSession.Send(presence).Async();
```

`presence` is the message in the existing code; the pickup handler sends its new pickup result instead. For notifications to other players, store IDs, send through the public Actor client as in `ZoneSpot.ShowBorderAsync`, and forward to the Session in a receiving handler such as `BorderDelivery`.

### State preservation and verification

Add inventory to `PlayerSnapshot`, `Capture`, and `Restore` in `PlayerActor.cs`, and remaining ground items to `ZoneSnapshot`, `Capture`, and `Restore` in `ZoneSpot.cs`. `PlayerRelocation` and `ZoneRelocation` serialize these snapshots, so include application state only. Framework manages routes, leases, membership, and queues; do not duplicate them in application snapshots.

If extracting pure game rules into unit tests, test successful pickup, absent items, and duplicate pickup. In e2e, register the typed result wait before sending, as `MoveAsync` does in `GameClient.cs`, so fast replies are also observed. Extend the scenario and selection case in `Scenarios.cs` and `run.scenarios` in the descriptor together, retaining the existing scenarios and assertions.

### Verification runs

Verify the received result, duplicate rejection, and inventory preservation after reconnect and relocation. Build and check structure before running affected scenarios, then preserve the first failure to distinguish application usage errors from the package defects documented in the README.

## Development considerations

### State and message contracts

Use public NuGet packages at the centrally pinned versions, and keep project references between this project's own modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

Manage each game fact, such as a player's coordinate, in the object responsible for changing it. Store player IDs and observational copies in Spots, without assuming the same Actor object reference remains usable after movement. Sessions forward packets after authentication and binding; the Actor or Spot responsible for the state makes movement and gameplay decisions.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and avoid `.Wait()`, `.Result`, or synchronous I/O that blocks handler execution. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/dotnet/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Apply the User Spot contract to `DungeonSpot` even though its process is named Instance; [Activation and lifetime](https://docs.zlink.systems/dotnet/guide/server/34-activation-lifetime/) explains creation and initialization. Use `JoinSpot(...).Defer()` and the public completion callback for Zone changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/dotnet/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `bin/`, and `obj/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `WorldRun.cs` at node startup and logs for each node.

Settings files are `zone-a.json`, `zone-b.json`, `instance.json`, and `session.json`; a replaced A uses the prefix `zone-a-replacement`. `ManagedProcess.cs` records diagnostic stderr in `<node>.err.log` and readiness/control stdout in `<node>.out.log`. Redis logs are `redis-start.log`, `redis-port.log`, `redis.log`, and `redis-stop.log`; shell runner build output is in `.logs/build.log`.

### Diagnostic levels

`GameHosting.cs` enables message flow, which records the stages from message receipt to completion, with this setting.

```csharp
options.ConfigureDispatch().Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
```

`Normal` records receive, dispatch, and completion stages and errors. Use `Detailed` for timing detail, and also enable `IncludeMessageSizes(true)` for size recording. Detailed recording adds cost, so use it for the required investigation scope.

`NodeSettings.cs` defines no diagnostics JSON key, so change the hosting call, rebuild, and restart to apply startup settings. The package exposes `IZLinkDiagnosticsRuntime.Level` and `SetLevelAsync` for runtime changes, but this project's stdin controls do not expose them. The property setter blocks, so use the asynchronous method inside Framework handlers and callbacks. [Monitoring](https://docs.zlink.systems/dotnet/guide/server/26-monitoring/) and [Observability diagnostics](https://docs.zlink.systems/dotnet/reference/08-observability-diagnostics/) describe diagnostic records and settings.

### Message flow investigation

`GameHosting` writes console logs to stderr, so inspect message flow in node `.err.log` files; the template creates no separate `.flow` file. A record's `flow` value identifies a message across nodes, and `corr` relates request and reply records. Records begin with `zlink flow:`; failures have `outcome=failed`, `error_type`, and `error_message`.

Read the failure fields and preceding stage together without filtering categories out. Flow IDs differ between runs, so compare passing and failing runs by logical operation, Actor ID, and stage order to find the first missing or failed stage. If startup fails before message flow recording, preserve the startup exception and stack.

### Supplementary tracing and cleanup

Set supplementary Spot discovery tracing in the parent environment before starting nodes. In PowerShell, use this environment variable.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = "1"
```

The value applies to child processes started from that environment; preserve stderr alongside message flow. In Bash, set the same variable as follows.

```bash
export ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1
```

`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY` is a version-specific diagnostic switch in the public `0.30.0` package, so do not use it as a routing API. Let the runner clean up its own processes and Compose project, preserve cleanup failures, and avoid stopping unrelated processes or containers.

## Reporting ZLink defects

Compare API usage with the public guides and samples linked above to distinguish application usage errors. If Framework behavior blocks a scenario, report a minimal public API reproduction and the expected observable result in [ZLink issues](https://github.com/zlink-systems/zlink/issues). Link an existing issue when it describes the same defect.

Include package versions from `Directory.Packages.props`, OS and runtime details, the first failing node-log excerpt, and available `flow` and `corr` records. State when startup failed before flow recording. Retain scenario assertions and report the failing public operation without hiding the defect through an application workaround.
