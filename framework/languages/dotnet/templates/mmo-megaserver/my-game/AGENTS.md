# MyGame development guide

## Project overview

This C#/.NET 8 project is an MMO server example with one world whose regions are capacity-limited instances: when a Region is full, the next instance of the same region is created. It runs a Session gateway for connections, Region hosts A and B, an Instance host for Dungeons, and Redis for location and relocation state. `Directory.Packages.props` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [.NET guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in this project.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host SDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario region,movement,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario region,movement,dungeon,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

For host process debugging, install the .NET 8 SDK and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only with an existing local Debug build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/MyGame.Session/Program.cs` registers connection handling, and `server/MyGame.Session/Infrastructure/ZLink/PlayerSession.cs` checks client packet names against `AllowedPackets`, binds the Player Actor, and relays allowed packets. The first packet must be `EnterRegionRequest`, and the example trusts the player ID in it; add authentication before binding. Internal messages stay outside the client allowlist.

### Regions and players

An Actor is a game state object addressed by ID; `PlayerActor` in `server/MyGame.Shared/Infrastructure/ZLink/PlayerActor.cs` holds no game state and forwards a Region's rejection reply to the Entry. A Spot accepts Actors and runs shared Region behavior; `RegionSpot` in `server/MyGame.Region/Infrastructure/ZLink/RegionSpot.cs` owns membership, capacity admission, and broadcasts. `Positions` in `server/MyGame.Region/Domain/Positions.cs` owns positions, sequences, movement limits, and time-based budgets.

`server/MyGame.Region/Program.cs` registers types and handlers. `server/MyGame.Region/Infrastructure/ZLink/RegionEntry.cs` receives the first packet and chooses the instance to join, and `server/MyGame.Region/Infrastructure/ZLink/Handlers.cs` holds the Region handlers, including the delivery handler that forwards pushes to the client. Add handlers that combine player rules with shared Region rules there. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/MyGame.Instance/Program.cs` and `server/MyGame.Instance/Infrastructure/ZLink/DungeonSpot.cs` create Dungeons on demand and answer a typed information request. `DungeonSpot` is an application-created User Spot that rejects every join, keeps no state, and is recreated on relocation. Add party admission rules, encounters, rewards, and Dungeon lifecycle behavior in this project.

### Shared contracts and hosting

`server/MyGame.Shared/Contracts.cs` defines messages, allowed region names, replica and movement limits, and rejection reasons. `server/MyGame.Shared/NodeSettings.cs` defines process settings, including `Capacity` and the Framework `RegionLimit`. `RegionEntry` owns replica selection; `Positions` owns movement state. `GameHosting.cs` configures hosting and diagnostics, and `NodeControl.cs` handles runtime control commands.

### E2E client

`e2e/MyGame.E2E/GameClient.cs` connects through the public connector. `Scenarios.cs` checks client-visible behavior, and `DomainChecks.cs` checks rule boundaries before scenario nodes start. `WorldRun.cs` manages each run, while `StructureCheck.cs` validates composition. Run domain checks with `--check-domain`; the project has no separate domain-test project.

## ZLink concepts in this project

### RouteMesh

A RouteMesh connects nodes that exchange messages between processes; this project uses `GameNames.WorldMesh`. The project registers no application channels, so `channels` in `zlink.structure.json` is empty. [Channel messaging](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`RegionEntry` is the Entry Spot that receives the first packet, `RegionSpot` is a User Spot created on demand for each instance such as `forest-1`, and `DungeonSpot` is a User Spot on the Instance node. The [Spot guide](https://docs.zlink.systems/dotnet/guide/server/21-spot/) describes registration and calls for these objects, including the request that `RegionSpot.OpenAsync` sends to a Dungeon. `PlayerActor` is requested by the Session node with `GetOrCreate` and hosted on a Region node; the [Actor guide](https://docs.zlink.systems/dotnet/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/dotnet/guide/server/23-stream/), which processes packets on client connections, `PlayerSession` checks `AllowedPackets`, obtains the Player Actor, binds it, and forwards allowed typed packets. Internal messages remain outside the client allowlist. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/dotnet/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/dotnet/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `RegionEntry.SelectAsync` requests `JoinSpot(...).Defer()`, `RegionSpot.OnActorJoinAsync` accepts the join or rejects it with a `RegionFull` reply, and `PlayerActor.OnJoinCompletedAsync` observes the rejection. [Actor membership](https://docs.zlink.systems/dotnet/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `RegionRelocation` serializes the player positions, and `NodeControl` requests public host relocation and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/dotnet/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `EnterRegionRequest`. `PlayerSession` checks the first packet type, calls `GetOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `RegionEntry`. `EnterRegion` checks the identity, and `SelectAsync` calls `GetOrCreate` for the first instance (`forest-1`) and requests `JoinSpot(...).Defer()`.
3. `RegionSpot.OnActorJoinAsync` accepts the player while fewer than `Capacity` players are inside. `OnJoinedActorAsync` then sends `RegionView` to every member by player ID, and `DeliverRegion` forwards it through each Actor's bound Session.
4. A full Region rejects a join with `RegionFull`. `PlayerActor.OnJoinCompletedAsync` forwards that reply to the Entry, which selects the next allowed replica. The Entry returns `RegionUnavailable` after `MaxReplicasPerRegion` is full; the Actor never chooses a target.
5. `MoveRequest` reaches `MovePlayer`, which delegates to `Positions`. The domain validates Delta and Sequence, checks region bounds and the movement budget, and changes state only after acceptance. The caller receives the accepted RegionView through its bound Session; other members receive one Actor broadcast. `OpenDungeonRequest` reaches `OpenDungeon`, which calls `GetOrCreate` for the Dungeon on the Instance node, sends a typed request to it, and replies to the client with `DungeonOpened`.
6. When the same player reconnects, `PlayerSession` binds the existing Actor to the new Session, and `ReconnectRegion` checks the identity and resends the current `RegionView`. A disconnected Session keeps the membership and position.
7. To replace a node, `drain` in `NodeControl` requests relocation. Framework takes the positions, sequences, and remaining movement-budget milliseconds from `RegionRelocation` and restores them on another node; Dungeons are recreated.

## Extending the MMO domain

Extend this project to add MMO rules and server behavior. Keep each decision with the module that owns its state, and add a client scenario for every observable change.

### Feature locations

| Concern | Owner | Extension procedure |
| --- | --- | --- |
| Typed messages and shared limits | `server/MyGame.Shared/Contracts.cs` | Add client and internal records, rejection reason strings, allowed region names, and shared numeric limits here. Keep one wire shape for each result. |
| Region positions and movement | `server/MyGame.Region/Domain/Positions.cs` | Add position state and movement decisions here. `Positions` owns coordinates, sequence numbers, movement budget, monotonic time, and boundary checks. `MaxStep` is 10, X is bounded to 0..100, and `MaxSpeed` is `MaxStep * 5` per second. Inject a test `TimeProvider` only through the domain constructor. |
| Region membership and delivery | `server/MyGame.Region/Infrastructure/ZLink/RegionSpot.cs` | Keep membership and Region broadcasts here. Update `Capture` and `Restore` with every domain state field; persist remaining milliseconds instead of process timestamps. Reply to the caller through its bound Session and send the same accepted result to other members through the existing Actor delivery path. |
| Region selection and admission | `server/MyGame.Region/Infrastructure/ZLink/RegionEntry.cs` | Keep allowed-region checks, replica selection, and the final unavailable result in the Entry. `RegionSpot` decides only whether its own membership capacity is available. `PlayerActor` forwards a rejected join to the Entry and does not choose another target. |
| Client packet boundary | `server/MyGame.Session/Infrastructure/ZLink/PlayerSession.cs` | Add client-originated packet names to `AllowedPackets`. Keep internal messages out of the allowlist; the Session replies with `ActionRejected("packet-not-allowed", 0)` for a disallowed packet. |
| Domain and client scenarios | `e2e/MyGame.E2E/DomainChecks.cs`, `e2e/MyGame.E2E/Scenarios.cs` | Add each rule boundary to `DomainChecks` and each public behavior to a named scenario. Domain checks run before scenario nodes and are available through `--check-domain`. |
| Topology and user guidance | `zlink.structure.json`, `README.md` | Keep `run.scenarios`, scenario-selection commands, and the README scenario table in the same order as `Scenarios.All`. Update node declarations and configs together when the topology changes. |

### Change procedure

1. Add typed contracts and shared constants in `Contracts.cs`.
2. Put each game rule and its mutable state in the domain object that decides it. Keep Framework callbacks in the `Infrastructure/ZLink` layer.
3. Add domain boundary checks for accepted limits, the first rejected value, and unchanged state after rejection. Use the domain constructor's test clock for time boundaries.
4. Add only client packet names to `PlayerSession.AllowedPackets`. Verify an internal packet is rejected and the Session remains connected.
5. Add a client-observable scenario in `Scenarios.cs`; then update `zlink.structure.json`, the README scenario table, and the command examples in both scripts.
6. Run `--check-domain`, the related scenario, the build, and the complete e2e suite. Inspect the first failure before another run.

This template keeps domain checks in the e2e executable and does not add a separate domain-test project. Use the public Framework API and default typed JSON serializer. Do not add a JSON number option that accepts NaN or Infinity.

## Development considerations

### State and message contracts

Use public NuGet packages at the centrally pinned versions, and keep project references between this project's own modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application state, keeping management of Framework execution state separate from game state.

Manage each game fact in the object responsible for changing it: population in `RegionSpot`, and positions, sequence numbers, and movement budgets in `Positions`. `PlayerActor` holds only identity and the binding. Store player IDs in Spot state, not Actor references. Deliver replies inside a request handler through the `BoundSession` of the Actor the handler receives, and deliver other pushes by player ID through the public Actor client to a delivery handler that forwards them to the bound Session. Sessions forward only allowlisted client packets after authentication and binding; the Entry decides region placement, and `Positions` decides movement.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and avoid `.Wait()`, `.Result`, or synchronous I/O that blocks handler execution. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/dotnet/guide/server/32-execution-model/) explains these execution boundaries.

Create Regions and Dungeons with `GetOrCreate`, move Actors with `JoinSpot(...).Defer()` and the public completion callback, and use relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. `RegionSpot` and `DungeonSpot` are application-created User Spots; [Activation and lifetime](https://docs.zlink.systems/dotnet/guide/server/34-activation-lifetime/) explains creation and initialization. Apply the User Spot contract to `DungeonSpot` even though its process is named Instance. The runner limits each Region node to two Region Spots through `RegionLimit`, so the survivor is full after a drain and only the replacement can host a new Region; do not rely on a fixed Region-to-node assignment.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using the existing membership and position. A disconnected Session keeps its membership until an explicit leave, and repeating `EnterRegionRequest` for a joined Actor returns its current Region view. Framework manages current placement, so do not add an application route cache or reset game state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/dotnet/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `bin/`, and `obj/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `WorldRun.cs` at node startup and logs for each node.

Settings files are `session.json`, `regionA.json`, `regionB.json`, and `instance.json`; a replaced node uses the `-replacement` suffix, such as `regionA-replacement`. `ManagedProcess.cs` records diagnostic stderr in `<node>.err.log` and readiness/control stdout in `<node>.out.log`. Redis logs are `redis-start.log`, `redis-port.log`, `redis.log`, and `redis-stop.log`; local build output is in `.logs/build.log`. Docker build, Redis, and outer cleanup logs are in `.logs/docker-<project>/`.

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

In local process mode, set supplementary Spot discovery tracing in the parent environment before starting nodes. Docker nodes do not inherit host environment variables. In PowerShell, use this environment variable.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = "1"
```

The value applies to child processes started from that environment; preserve stderr alongside message flow. In Bash, set the same variable as follows.

```bash
export ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1
```

`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY` is a version-specific diagnostic switch in the public `0.30.2` package, so do not use it as a routing API. Let the runner clean up its own processes and Compose project, preserve cleanup failures, and avoid stopping unrelated processes or containers.

## Reporting ZLink defects

### Identifying a defect

A ZLink defect is a case where a public API called as documented produces a result that differs from the documentation. First compare your API usage with the public guides and samples linked above to rule out an application usage error. If the usage is correct, report an issue instead of adding a workaround, and keep the scenario assertions unchanged. A workaround hides the defect and stays in the code after you upgrade to a fixed package, where it changes behavior.

### Checking existing issues

Before filing, search both open and closed [ZLink issues](https://github.com/zlink-systems/zlink/issues?q=is%3Aissue) by error message, exception type, and API name. If the same defect is already reported, add your environment and logs to that issue instead of opening a new one. If it matches a closed issue, find the package version that contains the fix in that issue, raise the pinned version in this project, and run again.

### Issue body

The body follows the issue template of the ZLink repository. Copy the following form and fill it in.

```markdown
# Issue description
<scenario and the public operation that fails>

# Environment
* zlink version: <package versions from Directory.Packages.props>
* OS: <OS, .NET runtime version>

# Minimal test code / Steps to reproduce the issue
1. <commands or minimal code using public APIs only>

# What's the actual result? (include assertion message & call stack if applicable)
<first failing node-log excerpt, exception and stack, flow/corr records>

# What's the expected result?
<observable result the guide or reference documents>
```

Reduce the reproduction to the minimal code or commands that keep only the failing public call, not this whole project. Paste only the first failing log section and the step before it; attach the full log as a file when it is needed. State when startup failed before flow recording.

> **Issues are public.** Remove license keys, tokens, internal host names, and personal data from logs and settings before filing.

### Filing procedure

A coding agent drafts the issue body and shows it to the developer, and the developer confirms it before it is filed. An issue is published in an external repository, so do not file one without the developer's confirmation. With the GitHub CLI, file the confirmed body file as follows.

```bash
gh issue create --repo zlink-systems/zlink --title "<area>: <observed failure>" --body-file issue.md
```

Put the language and the failing behavior in the title (for example, `framework(dotnet): Spot startup fails with empty SpotId`). Record the filed issue number in the README Known issues so the project keeps the place to re-verify when a fixed package is released.
