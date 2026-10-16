# MyGame development guide

## Project overview

This C#/.NET 8 project is an MMORPG server example with independent worlds, each with its own Session process, channel servers, route mesh, and Redis data, and with channels that a player can switch while the connection stays open. It runs an accounts service that lists worlds, a Session gateway per world, channel hosts, and one Redis store per world. `Directory.Packages.props` pins the ZLink Framework and connector packages to `0.30.1`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [.NET guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in this project.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host SDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario worlds,channel,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 --check-structure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario worlds,channel,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install the .NET 8 SDK and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only with an existing local Debug build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Accounts and Session gateway

`server/MyGame.Accounts/Program.cs` serves `GET /worlds`, which lists each world and its Session endpoint; it is a stateless host that uses no Redis or mesh and only answers the drain and shutdown controls. `server/MyGame.Session/Program.cs` registers connection handling, and `server/MyGame.Session/PlayerSession.cs` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. The first STREAM packet must be `EnterWorldRequest`, and the example trusts the player ID in it; add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Channels and players

An Actor is a game state object addressed by ID; `PlayerActor` in `server/MyGame.Channel/Infrastructure/ZLink/PlayerActor.cs` owns the player's gold, the only game fact in this template. A Spot is a state object that accepts Actors and executes shared game rules; `ZoneSpot` in `server/MyGame.Channel/Infrastructure/ZLink/ZoneSpot.cs` represents one channel of a world (`field@1`, `field@2`) and owns no application state. The gold rule is the small class `GoldBalance` in `server/MyGame.Channel/Domain/GoldBalance.cs`, and `PlayerData` in `server/MyGame.Channel/Infrastructure/Redis/PlayerData.cs` persists it under `player:<id>` keys of the world's Redis.

`server/MyGame.Channel/Program.cs` registers types and handlers and creates the fixed Zones listed in `ZoneIds` of the node settings when the host starts. `server/MyGame.Channel/Infrastructure/ZLink/Handlers.cs` holds the Zone Entry, the Zone handlers, and the delivery handler that forwards pushes to the client. Add handlers that combine player rules with Zone rules there. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Shared contracts and hosting

`server/MyGame.Shared/Contracts.cs` defines messages and game constants shared by processes, while `server/MyGame.Shared/NodeSettings.cs` defines process settings, including `WorldId`, `ZoneIds`, and `ZoneCapacity`. `server/MyGame.Shared/GameHosting.cs` configures hosting and diagnostics, and `server/MyGame.Shared/NodeControl.cs` handles runtime control commands. Add contracts used by several processes here, and keep player state in `PlayerActor` so separate processes do not independently modify the same fact.

### E2E client

`e2e/MyGame.E2E/GameClient.cs` connects through the public connector, and `e2e/MyGame.E2E/Scenarios.cs` checks results observable by the client. `e2e/MyGame.E2E/WorldRun.cs` manages processes and infrastructure for each run, while `e2e/MyGame.E2E/StructureCheck.cs` validates composition. Verify new gameplay through received client results; the project includes no separate unit-test project or formatter configuration.

## ZLink concepts in this project

### RouteMesh

A RouteMesh connects nodes that exchange messages between processes. This project runs one mesh per world, named by `GameNames.Mesh(WorldId)`, and never shares a mesh name or a world store across worlds. The project registers no application channels, so `channels` in `zlink.structure.json` is empty; the channels of this template are player-selected Zones, not messaging channels. [Channel messaging](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`ZoneEntry` is the Entry Spot that receives the first packet, and `ZoneSpot` is a fixed User Spot created at host startup for each channel. The [Spot guide](https://docs.zlink.systems/dotnet/guide/server/21-spot/) describes registration and calls for these objects. `PlayerActor` is requested by the Session node with `GetOrCreate`, loads its gold from Redis in `PlayerFactory`, and is hosted on a channel node; the [Actor guide](https://docs.zlink.systems/dotnet/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/dotnet/guide/server/23-stream/), which processes packets on client connections, `PlayerSession` obtains the Player Actor, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/dotnet/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation, one per world. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. Framework state keys and the application keys `player:<id>` share only their world's namespace. The [Location guide](https://docs.zlink.systems/dotnet/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `EnterPlayer` requests `JoinSpot(...).Defer()` into the first channel, `ChangeChannel` requests the join into another Zone, and `ZoneSpot.OnActorJoinAsync` accepts it; the client connection does not change. [Actor membership](https://docs.zlink.systems/dotnet/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `PlayerRelocation` captures the gold, `ZoneRelocation` carries no payload because a Zone owns no application state, and `NodeControl` requests public host relocation and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/dotnet/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client calls `GET /worlds` on the accounts service and receives the Session endpoint of each world. It connects to the chosen Session over STREAM and sends the first packet, `EnterWorldRequest`. `PlayerSession` checks the first packet type, calls `GetOrCreate` for the Player Actor by `playerId` in the world's mesh, binds it to the Session, and relays the original packet. `PlayerFactory` loads the player's gold from Redis when the Actor is created.
2. A new Actor starts in the Entry Spot `ZoneEntry`. `EnterPlayer` checks the identity and requests `JoinSpot(...).Defer()` into the first channel (`field@1`).
3. `ZoneSpot.OnActorJoinAsync` accepts the join, and `OnJoinedActorAsync` sends `PlayerView` to the Actor; `DeliverView` forwards it through the bound Session.
4. `EarnRequest` reaches `EarnGold`, which calls `PlayerActor.EarnAsync`. `GoldBalance` accepts a positive amount with checked arithmetic, `PlayerData` writes the new value, and the handler replies with `PlayerView`.
5. `ChangeChannelRequest` reaches `ChangeChannel`, which requests `JoinSpot(...).Defer()` into the other Zone. The connection, the Actor, and the gold stay the same; only the hosting Zone changes.
6. When the same player reconnects, `PlayerSession` binds the existing Actor to the new Session, and `EnterZone` checks the identity and resends the current `PlayerView`. The channel does not change.
7. To replace a node, `drain` in `NodeControl` requests relocation. Framework takes the gold from `PlayerRelocation` and restores it on another node; the Zones keep their logical IDs.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the .NET starter: choose .NET in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/dotnet/templates/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `mmo-world-channels`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature | File in this sample | What to transfer | What to grow in the starter alongside |
| --- | --- | --- | --- |
| Typed messages | `server/MyGame.Shared/Contracts.cs` | The records of the features you adopt: `EnterWorldRequest`, `ChangeChannelRequest`, `EarnRequest`, `ViewRequest`, `PlayerView`, `WorldInfo`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The registration names and the `Mesh`, `ZoneId`, and `Channel` helpers in `GameNames`; the starter already has `NodeReady` and `NodeProtocol` |
| Spot | `server/MyGame.Channel/Infrastructure/ZLink/ZoneSpot.cs` | `ZoneSpot` with admission, the player view, and the handler registrations in `Configure()` | A new `server/MyGame.Channel/` project, its `server/config/` files with `WorldId`, `ZoneIds`, and `ZoneCapacity`, and the `AddSpotFactory<ZoneSpot>` registration in `Program.cs`; the startup callback that creates the fixed Zones; the `spots` and `nodes` declarations in `zlink.structure.json` and the structure check that compares them with the registrations |
| Actor | `server/MyGame.Channel/Infrastructure/ZLink/PlayerActor.cs`, `server/MyGame.Channel/Domain/GoldBalance.cs`, `server/MyGame.Channel/Infrastructure/Redis/PlayerData.cs` | `PlayerActor` with the gold rule and its persistence, and `PlayerFactory` | The `AddActorFactory<PlayerActor, PlayerFactory>` registration in `Program.cs` and the `PlayerData` service; a Redis store per world in `docker-compose.yml` |
| Entry and first admission | `ZoneEntry` and `EnterZone` in `server/MyGame.Channel/Infrastructure/ZLink/Handlers.cs` | `EnterPlayer` and the deferred join into the first channel, and the identity check and state resend of `EnterZone` | The Entry Spot registration and its `entry` declarations; the `worlds` scenario for first entry and a `reconnect` scenario that compares state after reconnection |
| Session binding | `server/MyGame.Session/PlayerSession.cs` | The first-packet check, then find or create the Actor in the world's mesh, bind, and relay the original typed packet | The same first-packet type as `EnterPlayer`; transfer it into the starter's `PlayerSession.cs` |
| Host relocation | `PlayerRelocation` in `server/MyGame.Channel/Infrastructure/ZLink/PlayerActor.cs`, `ZoneRelocation` in `server/MyGame.Channel/Infrastructure/ZLink/ZoneSpot.cs`, `server/MyGame.Shared/NodeControl.cs` | The relocation adapters with `Capture` and `Restore`, and the `drain` and `shutdown` controls | The `PreserveStateWith` relocation registrations; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node accepts new work |
| World and channel selection | `ChangeChannel` in `server/MyGame.Channel/Infrastructure/ZLink/Handlers.cs`, `server/MyGame.Accounts/Program.cs` | The deferred membership change between Zones, and the `GET /worlds` listing | A new `server/MyGame.Accounts/` project and `server/config/accounts.json` with the `Worlds` list; the `accounts` node and its `HTTP` edge in `zlink.structure.json`; a `channel` scenario that checks the connection, `SessionInfo`, and gold survive the move |

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the list in `e2e/MyGame.E2E/Scenarios.cs`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the typed records in `Contracts.cs` first, then move each handler that fails to compile to the new messages. When you change the first-entry packet, update the first-packet check in `PlayerSession`, `EnterPlayer`, and `EnterZone` to receive the same type.

Keep each piece of state in the object that decides it. `PlayerActor` is the only owner of gold, and a Zone takes its identity from the Framework Spot context, so do not cache the channel or the membership elsewhere. When you change player state fields, update `PlayerRelocation` and the persisted value in `PlayerData` together; a state kept outside them disappears after relocation, and the `update` scenario detects the loss. Write player data inside the Actor's execution turn.

Keep worlds separate. Redis application keys use `player:<id>` under the world's key prefix, and logical Spot IDs are unique within a world mesh while composition IDs are globally unique, so `zlink.structure.json` declares a `mesh` on the fixed Zones. When you extend the topology, update the structure and the configuration together; the structure check rejects a mismatch before startup.

Keep the checks of the `worlds` scenario: the same player earns gold in world A and starts with none in world B.

To name roles such as Channel or Zone differently from this sample, change the type names, `GameNames`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the player state is missing a field, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use public NuGet packages at the centrally pinned versions, and keep project references between this project's own modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application state, keeping management of Framework execution state separate from game state.

Manage each game fact in the object responsible for changing it: gold in `PlayerActor`, never in a Zone. Store player IDs, not Actor references, if a Spot needs to address players. Deliver replies inside a request handler through the `BoundSession` of the Actor the handler receives, and deliver other pushes by player ID through the public Actor client to a delivery handler that forwards them to the bound Session. Sessions forward packets after authentication and binding; the Actor decides gold changes.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and avoid `.Wait()`, `.Result`, or synchronous I/O that blocks handler execution. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/dotnet/guide/server/32-execution-model/) explains these execution boundaries.

Move Actors between channels with `JoinSpot(...).Defer()` and the public completion callback, and use relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. `ZoneSpot` is an application-created User Spot created at startup by the node; [Activation and lifetime](https://docs.zlink.systems/dotnet/guide/server/34-activation-lifetime/) explains creation and initialization. `ZoneCapacity` sets the stable-type limit per channel node: the runner starts channelA1 with room for one Zone and channelA2 with room for two, so channelA2 can receive channelA1 during maintenance. Do not rely on a fixed Zone-to-node assignment.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using the existing gold and channel. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/dotnet/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `bin/`, and `obj/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `WorldRun.cs` at node startup and logs for each node.

Settings files are `accounts.json`, `sessionA.json`, `sessionB.json`, `channelA1.json`, `channelA2.json`, and `channelB.json`; a replaced node reuses its source configuration, and its logs use the `-replacement` suffix, such as `channelA1-replacement.err.log`. `ManagedProcess.cs` records diagnostic stderr in `<node>.err.log` and readiness/control stdout in `<node>.out.log`. Redis logs are `redis-start.log`, `dbA-port.log`, `dbB-port.log`, `redis.log`, and `redis-stop.log`; local build output is in `.logs/build.log`. Docker build, Redis, and outer cleanup logs are in `.logs/docker-<project>/`.

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

`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY` is a version-specific diagnostic switch in the public `0.30.1` package, so do not use it as a routing API. Let the runner clean up its own processes and Compose project, preserve cleanup failures, and avoid stopping unrelated processes or containers.

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
