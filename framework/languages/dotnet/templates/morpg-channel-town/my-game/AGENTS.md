# MyGame development guide

## Project overview

This C#/.NET 8 project is a MORPG server example in which players meet in a town channel, form parties of up to four, and enter a dedicated Dungeon for their party. It runs a Session gateway for connections, Town hosts A and B for the fixed town channels, Instance hosts A and B for Dungeons, and Redis for location and relocation state. `Directory.Packages.props` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [.NET guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in this project.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host SDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario channel,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario channel,dungeon,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

For host process debugging, install the .NET 8 SDK and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only with an existing local Debug build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/MyGame.Session/Program.cs` registers connection handling, and `server/MyGame.Session/PlayerSession.cs` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. The first packet must be `JoinTownRequest`, and the example trusts the player ID in it; add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Towns and players

An Actor is a game state object addressed by ID; `PlayerActor` in `server/MyGame.Shared/PlayerActor.cs` holds no game state, and Framework keeps its Session binding through Spot joins and host relocation. A Spot is a state object that accepts Actors and executes shared game rules; `TownSpot` in `server/MyGame.Town/TownSpot.cs` owns the channel population and the parties of one town (`town-1`, `town-2`). `server/MyGame.Town/Handlers.cs` holds the Town handlers, including channel change, party creation and joining, dungeon entry, and the delivery handler that forwards pushes to the client.

`server/MyGame.Town/Program.cs` registers types and handlers and creates the fixed towns listed in `TownIds` of the node settings when the host starts. `server/MyGame.Shared/TownEntry.cs` receives the first packet and joins the Actor to the requested town channel; it is registered on Town and Instance nodes alike. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/MyGame.Instance/Program.cs` registers the Dungeon factory, and `server/MyGame.Instance/DungeonSpot.cs` owns the party roster and the stage of one Dungeon, accepts only roster members, and answers the typed preparation request. `server/MyGame.Instance/Handlers.cs` holds the Dungeon handlers, including `Advance` and the delivery handler. `DungeonSpot` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract. Add encounters, rewards, and Dungeon lifecycle behavior in this project.

### Shared contracts and hosting

`server/MyGame.Shared/Contracts.cs` defines messages and game constants shared by processes, while `server/MyGame.Shared/NodeSettings.cs` defines process settings, including `TownIds`, `TownCapacity`, and `DungeonCapacity`. `server/MyGame.Shared/GameHosting.cs` configures hosting and diagnostics, and `server/MyGame.Shared/NodeControl.cs` handles runtime control commands. Add contracts used by several processes here, and keep town and Dungeon state in their Spots so separate processes do not independently modify the same fact.

### E2E client

`e2e/MyGame.E2E/GameClient.cs` connects through the public connector, and `e2e/MyGame.E2E/Scenarios.cs` checks results observable by the client. `e2e/MyGame.E2E/WorldRun.cs` manages processes and infrastructure for each run, while `e2e/MyGame.E2E/StructureCheck.cs` validates composition. Verify new gameplay through received client results; the project includes no separate unit-test project or formatter configuration.

## ZLink concepts in this project

### RouteMesh

A RouteMesh connects nodes that exchange messages between processes; this project uses `GameNames.WorldMesh`. The project registers no application channels, so `channels` in `zlink.structure.json` is empty; the town channels of this template are fixed Spots that players choose, not messaging channels. [Channel messaging](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`TownEntry` is the Entry Spot that receives the first packet, `TownSpot` is a fixed User Spot created at host startup for each town, and `DungeonSpot` is a User Spot created on demand for each party. The [Spot guide](https://docs.zlink.systems/dotnet/guide/server/21-spot/) describes registration and calls for these objects, including the request that `TownSpot.EnterAsync` sends to prepare a Dungeon. `PlayerActor` is requested by the Session node with `GetOrCreate` and hosted on a Town or Instance node; the [Actor guide](https://docs.zlink.systems/dotnet/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/dotnet/guide/server/23-stream/), which processes packets on client connections, `PlayerSession` obtains the Player Actor, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/dotnet/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/dotnet/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `EnterTownHandler` and `ChangeChannel` request `JoinSpot(...).Defer()` into a town, `DungeonTravel` requests the join into a Dungeon, and `TownSpot.OnActorJoinAsync` and `DungeonSpot.OnActorJoinAsync` decide admission; `PlayerActor.OnJoinCompletedAsync` observes a rejection. [Actor membership](https://docs.zlink.systems/dotnet/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `TownRelocation` and `DungeonRelocation` serialize the town and Dungeon state, and `NodeControl` requests public host relocation and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/dotnet/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinTownRequest`. `PlayerSession` checks the first packet type, calls `GetOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `TownEntry`. `EnterTownHandler` checks the identity and requests `JoinSpot(...).Defer()` into the requested town channel.
3. `TownSpot.OnActorJoinAsync` admits the player when the identity and the channel match. `OnJoinedActorAsync` then sends `TownView` to every player in the town by player ID, and `TownDelivery` forwards it through each Actor's bound Session.
4. `ChangeChannelRequest` reaches `ChangeChannel`, which requests the join into the other town; the connection and the Actor do not change. `CreatePartyRequest` and `JoinPartyRequest` reach `CreateParty` and `JoinParty`, which call `TownSpot.PartyAsync`. An invalid request is rejected through the requester's bound Session as `ActionRejected`; a valid one broadcasts the new `TownView`.
5. `EnterDungeonRequest` from the party leader reaches `EnterDungeon`, which calls `TownSpot.EnterAsync`. It calls `GetOrCreate` for the party's Dungeon, sends a typed `PrepareDungeon` request that fixes the roster, and sends `JoinDungeon` to each member by player ID. `DungeonTravel` then requests the join into the Dungeon, where `DungeonSpot.OnActorJoinAsync` admits only roster members.
6. `AdvanceRequest` reaches `Advance`, which raises the Dungeon stage and broadcasts `DungeonView` to every member; `DungeonDelivery` forwards it.
7. When the same player reconnects, `PlayerSession` binds the existing Actor to the new Session, and `TownReconnect` or `DungeonReconnect` in the Spot that holds the Actor checks the identity and resends the current view.
8. To replace a node, `drain` in `NodeControl` requests relocation. Framework takes the state from `TownRelocation` and `DungeonRelocation` and restores it on another node.

## Extending the template domain

Extend this template's domain to build a MORPG server. Follow the [README](README.md) for execution and deployment.

1. Extend channel population and party rules in `server/MyGame.Town/TownSpot.cs`, and Dungeon roster and stage rules in `server/MyGame.Instance/DungeonSpot.cs`. Keep each decision in its owning domain. `server/MyGame.Shared/PartyRun.cs` owns run transitions, Dungeon preparation and progression, and channel bounds.
2. Add shared contracts in `server/MyGame.Shared/Contracts.cs`. Distinguish client input from internal messages and use the default typed serializer.
3. Add new client packets to the Session allowlist in `server/MyGame.Session/PlayerSession.cs`. Internal messages must not be accepted from clients.
4. Add scenarios in `e2e/MyGame.E2E/Scenarios.cs` that observe replies and unchanged state after rejection. Update the README scenario table and `run.scenarios` in `zlink.structure.json` together.

When adding state fields, update relocation capture and restore together. Framework owns routing, membership and binding; keep those decisions out of the game domain.

## Development considerations

### State and message contracts

Use public NuGet packages at the centrally pinned versions, and keep project references between this project's own modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application state, keeping management of Framework execution state separate from game state.

Manage each game fact in the object responsible for changing it: channel population and parties in `TownSpot`, and the roster and stage in `DungeonSpot`. `PlayerActor` holds only identity and the binding. Store player IDs in Spot state, not Actor references. Deliver replies inside a request handler through the `BoundSession` of the Actor the handler receives, and deliver other pushes by player ID through the public Actor client to a delivery handler that forwards them to the bound Session. Sessions forward packets after authentication and binding; the Town and the Dungeon decide game outcomes.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and avoid `.Wait()`, `.Result`, or synchronous I/O that blocks handler execution. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/dotnet/guide/server/32-execution-model/) explains these execution boundaries.

Create Dungeons with `GetOrCreate`, move Actors with `JoinSpot(...).Defer()` and the public completion callback, and use relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. `TownSpot` and `DungeonSpot` are application-created User Spots; [Activation and lifetime](https://docs.zlink.systems/dotnet/guide/server/34-activation-lifetime/) explains creation and initialization. Apply the User Spot contract to `DungeonSpot` even though its process is named Instance. Do not rely on a fixed Dungeon-to-node assignment: the runner keeps one Dungeon slot per Instance, so the survivor is full after a drain and only the replacement can host a new Dungeon.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using the existing town or Dungeon state. A disconnected player keeps the channel and party until an explicit move, and Dungeon rosters survive disconnects. Framework manages current placement, so do not add an application route cache or reset game state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/dotnet/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `bin/`, and `obj/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `WorldRun.cs` at node startup and logs for each node.

Settings files are `session.json`, `townA.json`, `townB.json`, `instanceA.json`, and `instanceB.json`; a replaced node uses the `-replacement` suffix, such as `instanceA-replacement`. `ManagedProcess.cs` records diagnostic stderr in `<node>.err.log` and readiness/control stdout in `<node>.out.log`. Redis logs are `redis-start.log`, `redis-port.log`, `redis.log`, and `redis-stop.log`; local build output is in `.logs/build.log`. Docker build, Redis, and outer cleanup logs are in `.logs/docker-<project>/`.

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
