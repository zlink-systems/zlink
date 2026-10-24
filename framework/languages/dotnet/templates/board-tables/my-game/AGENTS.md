# MyGame development guide

## Project overview

This C#/.NET 8 project is a board table server example in which players sit at a shared Table and play hands against each other. It runs a Session gateway for connections, Table hosts A and B, and Redis for location and relocation state. `Directory.Packages.props` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [.NET guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in this project.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host SDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario table,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario table,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install the .NET 8 SDK and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only with an existing local Debug build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/MyGame.Session/Program.cs` registers connection handling, and `server/MyGame.Session/PlayerSession.cs` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Entry and tables

An Actor is a game state object addressed by ID; `PlayerActor` in `server/MyGame.Table/PlayerActor.cs` holds only the player's identity and the bound Session, with no game state. A Spot is a state object that accepts Actors and executes shared game rules; `TableSpot` in `server/MyGame.Table/TableSpot.cs` owns seats, chips, the deck, cards, turns, and the pot, and decides admission in `OnActorJoinAsync`. Game rules and settlement belong in `TableSpot`, not in the Actor.

`server/MyGame.Table/Program.cs` registers the Entry Spot, the Actor factory, and the Table factory. `server/MyGame.Table/TableEntry.cs` receives the first packet and joins the Actor to a Table, and `server/MyGame.Table/Handlers.cs` holds the Table handlers and the delivery handlers that forward pushes to the client. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Shared contracts and hosting

`server/MyGame.Shared/Contracts.cs` defines messages and game constants shared by processes, while `server/MyGame.Shared/NodeSettings.cs` defines process settings, including the optional `TableCapacity`. `server/MyGame.Shared/GameHosting.cs` configures hosting and diagnostics, and `server/MyGame.Shared/NodeControl.cs` handles runtime control commands. Add contracts used by several processes here, and keep table state in `TableSpot` so separate processes do not independently modify the same fact.

### E2E client

`e2e/MyGame.E2E/GameClient.cs` connects through the public connector, and `e2e/MyGame.E2E/Scenarios.cs` checks results observable by the client. `e2e/MyGame.E2E/TableRun.cs` manages processes and infrastructure for each run, while `e2e/MyGame.E2E/StructureCheck.cs` validates composition. Verify new gameplay through received client results; the project includes no separate unit-test project or formatter configuration.

## ZLink concepts in this project

### RouteMesh

A RouteMesh connects nodes that exchange messages between processes; this project uses `GameNames.WorldMesh`. The project registers no application channels, so `channels` in `zlink.structure.json` is empty. [Channel messaging](https://docs.zlink.systems/dotnet/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`TableEntry` is the Entry Spot that receives the first packet, and `TableSpot` is a User Spot created on demand for each table ID. The [Spot guide](https://docs.zlink.systems/dotnet/guide/server/21-spot/) describes registration and calls for these objects. `PlayerActor` is requested by the Session node with `GetOrCreate`, hosted on a Table node, and joins a table; the [Actor guide](https://docs.zlink.systems/dotnet/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/dotnet/guide/server/23-stream/), which processes packets on client connections, `PlayerSession` obtains the Player Actor, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/dotnet/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/dotnet/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `EnterTableHandler` requests deferred admission with `JoinSpot(...).Defer()`, `TableSpot.OnActorJoinAsync` accepts a seat or rejects a full table, and `PlayerActor.OnJoinCompletedAsync` observes the completion result. [Actor membership](https://docs.zlink.systems/dotnet/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `TableRelocation` serializes the table state, and `NodeControl` requests public host relocation and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/dotnet/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinTableRequest`. `PlayerSession` checks the first packet type, calls `GetOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `TableEntry`. `EnterTableHandler` checks the identity, calls `GetOrCreate` for the Table, and requests `JoinSpot(...).Defer()`.
3. `TableSpot.OnActorJoinAsync` assigns a seat or rejects a full table. `OnJoinedActorAsync` sends `TableView` to every seated player by player ID, and `TableViewDelivery` forwards it through each Actor's bound Session. A rejected join reaches the client as `ActionRejected` from `PlayerActor.OnJoinCompletedAsync`.
4. `StartHandRequest` and `BetRequest` reach the Table's handlers (`StartHandler`, `BetHandler`). Rejections go through the requester's bound Session; deals, views, and the `HandResult` settlement go to each player by player ID and are forwarded by the delivery handlers.
5. When the same player reconnects, `PlayerSession` binds the existing Actor to the new Session, and `ReconnectHandler` checks the identity and resends the current table state and the player's own cards.
6. To replace a node, `drain` in `NodeControl` requests relocation. Framework takes the table state from `TableRelocation` and restores it on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the .NET starter: choose .NET in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/dotnet/templates/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `board-tables`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature | File in this sample | What to transfer | What to grow in the starter alongside |
| --- | --- | --- | --- |
| Typed messages | `server/MyGame.Shared/Contracts.cs` | The records of the features you adopt: `JoinTableRequest`, `TableViewRequest`, `StartHandRequest`, `BetRequest`, `SeatView`, `TableView`, `HandDealt`, `RevealedHand`, `HandResult`, `ActionRejected`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The registration names in `GameNames` and the `Rejection` reasons; the starter already has `NodeReady` and `NodeProtocol` |
| Spot | `server/MyGame.Table/TableSpot.cs` | `TableSpot` with seats, chips, deck, turns, the pot, admission in `OnActorJoinAsync`, and the handler registrations in `Configure()` | A new `server/MyGame.Table/` project, its `server/config/` files, and the `AddSpotFactory<TableSpot>` registration in `Program.cs`; the `spots` and `nodes` declarations in `zlink.structure.json` and the structure check that compares them with the registrations |
| Actor | `server/MyGame.Table/PlayerActor.cs` | `PlayerActor` with the join-completion rejection reply, and `PlayerFactory` | The `AddActorFactory<PlayerActor, PlayerFactory>` registration in `Program.cs` |
| Entry and first admission | `server/MyGame.Table/TableEntry.cs`, `ReconnectHandler` in `server/MyGame.Table/Handlers.cs` | `EnterTableHandler`, the table creation and deferred join, and the identity check and state resend of `ReconnectHandler` | The Entry Spot registration and its `entry` declarations; the `table` scenario for first entry and a `reconnect` scenario that compares state after reconnection |
| Session binding | `server/MyGame.Session/PlayerSession.cs` | The first-packet check, then find or create the Actor, bind, and relay the original typed packet | The same first-packet type as `EnterTableHandler`; transfer it into the starter's `PlayerSession.cs` |
| Host relocation | `TableRelocation` in `server/MyGame.Table/TableSpot.cs`, `PlayerRelocation` in `server/MyGame.Table/PlayerActor.cs`, `server/MyGame.Shared/NodeControl.cs` | The relocation adapters with `Capture` and `Restore`, and the `drain` and `shutdown` controls | The `PreserveStateWith` relocation registrations; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node accepts a new table |

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the list in `e2e/MyGame.E2E/Scenarios.cs`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the typed records in `Contracts.cs` first, then move each handler that fails to compile to the new messages. When you change the first-entry packet, update the first-packet check in `PlayerSession`, `EnterTableHandler`, and `ReconnectHandler` to receive the same type.

Keep each piece of state in the object that decides it. Seats, chips, the deck, cards, turns, and the pot belong in `TableSpot`, and the Actor stays free of game state. `TableRelocation` serializes the whole `TableState`, so adding a field to `TableState` or `PlayerSeat` is enough for relocation; a state kept outside it disappears after relocation, and the `update` scenario detects the loss.

Keep the checks of the `table` scenario when changing the rules: private cards reach only their owner and the public view shows card counts only, the sum of chips is conserved through settlement, and out-of-turn actions are rejected. The `reconnect` and `update` scenarios compare the Actor generation in `SessionInfo` and the connection continuity, so retain those assertions.

To name roles such as Table differently from this sample, change the type names, `GameNames`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the table state is missing anything, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use public NuGet packages at the centrally pinned versions, and keep project references between this project's own modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application state, keeping management of Framework execution state separate from game state.

Manage each game fact in the object responsible for changing it. Seats, chips, cards, the turn, and the pot belong to `TableSpot`; `PlayerActor` holds only identity and the binding. Send to the player ID of a Table seat, and keep no copy of Actor references. Deliver replies inside a request handler through the `BoundSession` of the Actor the handler receives, and deliver other pushes by player ID through the public Actor client to a delivery handler that forwards them to the bound Session. Sessions forward packets after authentication and binding; the Table decides game outcomes.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and avoid `.Wait()`, `.Result`, or synchronous I/O that blocks handler execution. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/dotnet/guide/server/32-execution-model/) explains these execution boundaries.

Create tables with `GetOrCreate` and move Actors with `JoinSpot(...).Defer()` and the public completion callback, and use relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. `TableSpot` is an application-created User Spot; [Activation and lifetime](https://docs.zlink.systems/dotnet/guide/server/34-activation-lifetime/) explains creation and initialization. The runner sets `TableCapacity` through the public stable-type limit; a node with the limit reached accepts no further tables, so do not rely on a fixed table-to-node assignment.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using the existing seat and game state. Framework manages current placement, so do not add an application route cache or reset game state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/dotnet/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `bin/`, and `obj/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `TableRun.cs` at node startup and logs for each node.

Settings files are `session.json`, `table-a.json`, and `table-b.json`; a replaced node uses the `-replacement` suffix, such as `table-a-replacement`. `ManagedProcess.cs` records diagnostic stderr in `<node>.err.log` and readiness/control stdout in `<node>.out.log`. Redis logs are `redis-start.log`, `redis-port.log`, `redis.log`, and `redis-stop.log`; local build output is in `.logs/build.log`. Docker build, Redis, and outer cleanup logs are in `.logs/docker-<project>/`.

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
