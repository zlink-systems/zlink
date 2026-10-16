# MyGame development guide

## Project overview

This Java 25 project is a MORPG example in which players gather in town channels, form parties, and enter a private dungeon per party. It runs a Session gateway for connections, Town hosts A and B that hold the fixed town channels, Instance hosts A and B that create Dungeons on demand, and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework to `0.30.1` and the binding to `1.19.1`.

The project layout below describes where to add gameplay, and "Developing from this sample" describes how to carry its features into your own project. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Java guide](https://docs.zlink.systems/java/guide/server/), [API reference](https://docs.zlink.systems/java/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the version pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker, so they need Docker with Compose v2 and no host JDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario channel,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario channel,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/session/src/main/java/MyGame/session/Program.java` registers STREAM and the mesh listener, and `server/session/src/main/java/MyGame/session/GameSession.java` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession.onDispatch` requires `JoinTownRequest` as the first packet, gets or creates the Player Actor by `playerId`, binds it, and relays the original packet. Add authentication before binding and retain the typed relay so Framework can identify the connection for replies to that player.

### Towns and players

An Actor is a game state object addressed by ID; `Player` in `server/shared/src/main/java/MyGame/Player.java` is the shared player Actor and has no application state. A Spot is a state object that accepts Actors and executes shared game rules; `Town` in `server/town/src/main/java/MyGame/town/Town.java` is one fixed town channel (`town-<channel>`) and owns its players and parties (`server/town/src/main/java/MyGame/town/TownState.java`). The shared application Entry Spot (`server/shared/src/main/java/MyGame/admission/Entry.java`, `server/shared/src/main/java/MyGame/admission/EntryActions.java`) sends a player's first entry to the requested channel.

`server/town/src/main/java/MyGame/town/Program.java` registers the Entry Spot, the Town factory (`townCapacity`), the Player factory, and the shared entry handlers. `server/town/src/main/java/MyGame/town/TownActions.java` holds the handlers for login, channel change, parties, dungeon entry, and delivery. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/instance/src/main/java/MyGame/instance/Program.java` registers the Dungeon factory (`dungeonCapacity` from `server/config/instanceA.json`) and the Entry Spot. `Dungeon` in `server/instance/src/main/java/MyGame/instance/Dungeon.java` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract; it owns the party roster and the stage (`DungeonState`). `server/instance/src/main/java/MyGame/instance/Prepare.java` answers the party's preparation request, and `server/instance/src/main/java/MyGame/instance/DungeonActions.java` holds the handlers for login, views, and stage advance. Add encounters, rewards, and Dungeon lifecycle behavior in this project.

### Shared contracts and hosting

`server/shared/src/main/java/MyGame/` holds the messages and game constants shared by processes (`JoinTownRequest`, `ChangeChannelRequest`, `CreatePartyRequest`, `JoinPartyRequest`, `EnterDungeonRequest`, `AdvanceRequest`, `ViewRequest`, `TownView`, `PartyView`, `PrepareDungeon`, `DungeonPrepared`, `JoinDungeon`, `DungeonView`, `ActionRejected`, `SessionInfoRequest`, `SessionInfo`), `NodeConfig.java` defines process settings, and `Names.java` defines the mesh and type names. `Hosting.java` configures hosting and diagnostics and runs the stdin control loop (`drain`, `shutdown`), `Control.java` defines the control markers and deadlines, and `Stores.java` creates the Redis Location and Relocation stores. `NodeRole.java` holds the node roles, `Rules.java` the party and rejection rules, `Player.java`, `PlayerFactory.java`, and `PlayerTransfer.java` the shared Player Actor, and `Transfer.java` the cancellation check of the relocation adapters. Add contracts used by several processes here, and keep party and roster state in `Town` and `Dungeon` so separate processes do not independently modify the same fact.

### E2E client

`e2e/src/main/java/MyGame/e2e/Client.java` connects through the public connector, and `e2e/src/main/java/MyGame/e2e/Scenarios.java` checks results observable by the client. `e2e/src/main/java/MyGame/e2e/Nodes.java` starts the nodes, keeps their logs, and cleans up the resources of each run; `e2e/src/main/java/MyGame/e2e/Program.java` reports cleanup failures after scenario failures, and either one makes the exit code 1. `e2e/src/main/java/MyGame/e2e/Structure.java` validates the actual files before startup. Keep the scenario order `channel,dungeon,reconnect,update`. Verify new gameplay through received client results; the unit tests `server/town/src/test/java/MyGame/town/TownStateTest.java` and `server/instance/src/test/java/MyGame/instance/DungeonStateTest.java` cover the town and dungeon rules without Framework. The project includes no formatter configuration.

## ZLink concepts in this project

### RouteMesh

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-world`). [Channel messaging](https://docs.zlink.systems/java/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` accepts a player's first entry, `Town` represents one fixed town channel, and `Dungeon` represents a dungeon created for one party. The [Spot guide](https://docs.zlink.systems/java/guide/server/21-spot/) describes registration and calls for these objects. `Player` retains the player's connection access when its host Spot changes; the [Actor guide](https://docs.zlink.systems/java/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/java/guide/server/23-stream/), which processes packets on client connections, `GameSession` gets or creates the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/java/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation, created in `server/shared/src/main/java/MyGame/Stores.java`, and `docker-compose.yml` defines the Redis service of the e2e runs. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/java/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. The shared `EntryActions.join`, `TownActions.change`, and `TownActions.travel` request deferred admission to a Town or a Dungeon, `Town.onActorJoin` and `Dungeon.onActorJoin` accept or reject it, and `Player.onJoinCompleted` observes the completion result. [Actor membership](https://docs.zlink.systems/java/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters (`TownTransfer`, `DungeonTransfer`, `PlayerTransfer`), registered with `preserveStateWith(...)`, capture and restore game state, while Framework transfers workload and routes. `Hosting.run` handles the `drain` control by requesting a public planned-maintenance relocation and observing safe shutdown; the [Relocation guide](https://docs.zlink.systems/java/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinTownRequest` (`playerId`, `channel`). `GameSession` checks the first packet type, calls `getOrCreate` for the Player Actor by `playerId`, binds it with `bindOrGet`, and relays the original packet.
2. A new Actor starts in the Entry Spot. The shared `EntryActions.join` checks the identity and requests `joinSpot(Names.townId(channel), request).defer()`. `Town.onActorJoin` checks the player and channel and adds the player, and `Town.onJoinedActor` broadcasts `TownView` to the channel.
3. `ChangeChannelRequest` is delivered to `TownActions.change`, which requests the other town channel with `joinSpot(...).defer()`. The client connection does not change, even when that channel is on another node. `CreatePartyRequest` and `JoinPartyRequest` are delivered to `Town.party` and update the channel's parties.
4. `EnterDungeonRequest` is delivered to `Town.enter`, which accepts it from the party leader only. It creates `dungeon-<partyId>` with `getOrCreate(id, Names.DUNGEON)`, asks the Dungeon to prepare with `requestToSpot(id, new PrepareDungeon(...))` (answered by `Prepare.prepare`), and then sends `JoinDungeon` to every member by player ID.
5. Each member's `travel` handler requests `joinSpot(dungeonId, ...).defer()`. `Dungeon.onActorJoin` admits only members of the prepared party, and `Dungeon.onJoinedActor` sends `DungeonView`. `AdvanceRequest` is delivered to `Dungeon.advance`, which increments the stage and sends the view to every member.
6. To replace a node, the `drain` control in `Hosting.run` requests relocation. Framework takes the game state from `capture` in `TownTransfer`, `DungeonTransfer`, and `PlayerTransfer` and calls `restore` on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the Java starter: choose Java in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/java/templates/java/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `morpg-channel-town`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature | File in this sample | What to transfer | What to grow in the starter alongside |
| --- | --- | --- | --- |
| Typed messages | `server/shared/src/main/java/MyGame/JoinTownRequest.java` | The records of the features you adopt: `JoinTownRequest`, `ChangeChannelRequest`, `CreatePartyRequest`, `JoinPartyRequest`, `EnterDungeonRequest`, `AdvanceRequest`, `ViewRequest`, `TownView`, `PartyView`, `PrepareDungeon`, `DungeonPrepared`, `JoinDungeon`, `DungeonView`, `ActionRejected`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The registration names in `Names.java`; the starter already has `Control.java` and `Json.java` |
| Actor | `server/shared/src/main/java/MyGame/Player.java`, `server/shared/src/main/java/MyGame/PlayerFactory.java` | The stateless shared `Player` with its identity check and join-completion handling, and its factory | A new `server/town/` module (`build.gradle.kts`, `Program.java`) with `include("server:town")` in `settings.gradle.kts`, its `server/config/` files, and the Actor factory registration in `Program.java`; the node declaration in `zlink.structure.json` and the check in `Structure.java`. The starter's `Nodes.java` starts only the Session role, so extend `Nodes.java` and `Roles.java` for the new role |
| Spot | `server/town/src/main/java/MyGame/town/Town.java`, `server/town/src/main/java/MyGame/town/TownState.java` | `Town` with its players and parties; keep the rules in `TownState` | The Spot factory registration in `Program.java`; the `spots` and `nodes` declarations in `zlink.structure.json`, and a `Structure.java` check that compares them with the registrations (the starter declares no Spots) |
| Entry and first admission | `server/shared/src/main/java/MyGame/admission/Entry.java`, `server/shared/src/main/java/MyGame/admission/EntryActions.java`, `login` in `server/town/src/main/java/MyGame/town/TownActions.java` | `EntryActions.join`, the first-entry path to the requested channel, and the identity check and view resend of `TownActions.login` | The Entry Spot registration and its `entry` declaration; a first-entry scenario and a `reconnect` scenario that compares state after reconnection; the Entry declaration of every node that registers the shared Entry |
| Session binding | `server/session/src/main/java/MyGame/session/GameSession.java` | The first-packet check, then find or create the Actor, bind, and relay the original typed packet | The same first-packet type as `EntryActions.join`; transfer it into the starter's `PlayerSession.java` |
| Host relocation | `server/town/src/main/java/MyGame/town/TownTransfer.java`, `server/instance/src/main/java/MyGame/instance/DungeonTransfer.java`, `server/shared/src/main/java/MyGame/PlayerTransfer.java`, `server/shared/src/main/java/MyGame/Transfer.java`, `server/shared/src/main/java/MyGame/Hosting.java` | The snapshot adapters with `capture` and `restore` for the Town, the Dungeon, and the Player | The `preserveStateWith(...)` registrations; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node accepts new work |
| Dynamic dungeon | `enter` in `server/town/src/main/java/MyGame/town/Town.java`, `server/instance/src/main/java/MyGame/instance/Dungeon.java`, `server/instance/src/main/java/MyGame/instance/DungeonState.java`, `server/instance/src/main/java/MyGame/instance/Prepare.java`, `server/instance/src/main/java/MyGame/instance/DungeonActions.java`, `server/instance/src/main/java/MyGame/instance/Program.java` | The dynamic User Spot creation pattern: `getOrCreate`, the preparation request to the Spot, the `JoinDungeon` push to every member, and the roster and stage rules | A new `server/instance/` module and its config; the dynamic `dungeon` Spot, its Instance Entry Spots, and the `GetOrCreate` edge in `zlink.structure.json` |
| World/channel selection | `change` in `server/town/src/main/java/MyGame/town/TownActions.java`, `townId` in `server/shared/src/main/java/MyGame/Names.java` | `change`, which uses a deferred Spot join to the other fixed town channel | A second fixed channel Spot on the same or another node (`townIds`, `townCapacity`), and a `channel` scenario that checks the connection survives the change |

The channels in this sample are player-selected town channels. They differ from messaging subscription channels, and this sample registers no pub/sub subscriptions.

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the `Scenario` enum in `server/shared/src/main/java/MyGame/Scenario.java`, to the dispatch in `e2e/src/main/java/MyGame/e2e/Scenarios.java`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the typed records in `server/shared/src/main/java/MyGame/` first, then move each handler that fails to compile to the new messages. When you change the first-entry packet, update the first-packet check in `GameSession`, the shared `EntryActions.join`, and `TownActions.login` to receive the same type.

Keep each piece of state in the object that decides it. Channel players and parties belong in `Town` (`TownState`), the party roster and stage in `Dungeon` (`DungeonState`), and the shared `Player` keeps no application state. When you change state fields, update that state class and its `capture` and `restore` in `TownTransfer` or `DungeonTransfer` together. A missing field disappears after relocation, and the `update` scenario detects the loss.

Town and Instance nodes both register the shared application Entry and its first-admission handlers, so declare one Entry Spot for each node that registers them and none for roles without it; Framework default Entries are not part of the structure. Keep the declaration of every Dungeon-owning Instance complete. The e2e configuration gives each Instance exactly one Dungeon slot (`dungeonCapacity` in `server/config/instanceA.json`), which makes replacement placement deterministic; keep it. Set each Town provider's `townCapacity` to exactly its configured fixed town count: the structure checker enforces the exact capacity, and Town hosts start one at a time, each after the previous `READY`.

To name roles differently from this sample, change the type names, `Names.java`, `server/config/`, the module and package names with `settings.gradle.kts`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use public Maven Central packages at the versions pinned centrally in `build.gradle.kts`, and keep project dependencies between this project's own modules; do not commit local package feeds or dependencies on repository sources. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

`Town` owns the channel players and parties, `Dungeon` owns the party roster and stage, and Session binding owns identity. Store player IDs in Spot state, never Actor references, because host relocation does not call `onJoinedActor` again on the target. Framework owns routes, memberships, leases, queues, scheduling, placement, and relocation. Reply to the requester through the handler Actor's bound Session (`actor.context().boundSession().send(...)`). Send other pushes to the Actor by player ID with `ZLinkActorClient.sendToActor`, and let a Spot Actor handler such as `TownActions.deliver` forward the message to the bound Session. `SessionInfo` reads identity from the Session binding, and game-state messages contain no binding identity.

### Asynchronous execution and lifecycle

Compose public asynchronous calls as `CompletionStage` chains as the template does, and avoid `.join()`, `.get()`, `Thread.sleep`, or blocking I/O in handlers and callbacks; the `.join()` calls in `Hosting.java` run on the startup and control thread, outside any handler. Framework's execution gate limits concurrent turns over the same state, so do not protect that state again with another lock or queue. `Program.java` sets `SPOT_WIDE` execution mode on `Town` and `Dungeon`, so handlers, timers, and callbacks of that Spot and its member Actors share one gate. Awaiting can retain the turn, so do not wait for work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/java/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. `Town` channels are fixed User Spots that `Hosting.run` creates at startup from `townIds` with `getOrCreate`, and `Dungeon` is a User Spot even though its process is named Instance; [Activation and lifetime](https://docs.zlink.systems/java/guide/server/34-activation-lifetime/) explains creation and initialization. Keep the Message Follow duration and the derived safe-shutdown observation budget in the shared constants of `Control.java`. Use `joinSpot(...).defer()` and the public completion callback `onJoinCompleted` for Spot changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. Handlers are registered by `options.addHandlersFromPackageOf(...)` in each `Program.java`; a new handler class outside the packages those calls cover is not registered unless you register it explicitly (`town` also registers the shared `MyGame.admission` handlers).

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/java/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, `.gradle/`, and `zlink.license` files from commits.

`Town.onDisconnectActor` keeps town and party membership until an explicit move, so a reconnect with the same Actor ID returns to the same channel.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains the effective settings that `Nodes.java` writes at node startup and the logs of each node.

Settings files are `instanceA.json`, `instanceB.json`, `session.json`, `townA.json`, `townB.json`. Each node start has its own directory `<node>-<uuid>/` with `<node>-stderr.log` for diagnostic stderr and `<node>-stdout.log` for readiness and control stdout; a replaced node (the `update` scenario) logs to a new directory. In local mode `compose.log` records the Redis Compose output of the run and `.logs/build.log` records the local build. In Docker mode `.logs/docker-<project>/` holds `build.log`, `redis-start.log`, `redis.log`, and `cleanup.log`.

### Diagnostic levels

`server/shared/src/main/java/MyGame/Hosting.java` enables message flow, which records the stages from message receipt to completion, with this setting in `configure`.

```java
options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
```

`NORMAL` records receive, dispatch, and completion stages and errors. `ERRORS` (the Framework default) records failures and backpressure only, and `OFF` records nothing. Use `DETAILED` for byte sizes and elapsed time; `includeMessageSizes(true)` on the same dispatch options also records message sizes. Detailed recording adds cost, so use it for the required investigation scope.

`NodeConfig.java` defines no diagnostics key, so change the hosting call, rebuild, and restart to apply startup settings. `ZLinkFrameworkRuntime` exposes `messageFlowMode()` and `setMessageFlowModeAsync` for runtime changes, but this project's stdin controls do not expose them. Use the asynchronous method inside Framework handlers and callbacks. The [Monitoring](https://docs.zlink.systems/java/guide/server/26-monitoring/) and [Observability diagnostics](https://docs.zlink.systems/java/reference/08-observability-diagnostics/) describe diagnostic records and settings.

### Message flow investigation

The logback configuration in `server/shared/src/main/resources/logback.xml` writes console logs to stderr, so inspect message flow in the node `<node>-stderr.log` files; the template creates no separate flow file. `Hosting.java` also writes `RUNTIME state=...` lines to stderr while message flow is enabled. A record's `flow` value identifies a message across nodes, and `corr` relates request and reply records. Records begin with `zlink flow:`; failures have `outcome=failed`, `error_type`, and `error_message`.

Read the failure fields and preceding stage together without filtering categories out. Flow IDs differ between runs, so compare passing and failing runs by logical operation, Actor ID, and stage order to find the first missing or failed stage. If startup fails before message flow recording, preserve the startup exception and stack.

### Supplementary tracing and cleanup

To trace the STREAM connector used by the e2e client, set `ZLINK_JAVA_STREAM_TRACE=1` in the parent process environment before a `--local` run. The Docker runner and nodes do not inherit host variables, so add the variable to the Compose `runner` service to use it there. Trace lines carry the `[zlink-java-stream-trace]` prefix and are logged at `java.util.logging` level `FINE`, so enable that level for the runner to see them. In PowerShell, use this environment variable.

```powershell
$env:ZLINK_JAVA_STREAM_TRACE = "1"
```

In Bash, set the same variable as follows.

```bash
export ZLINK_JAVA_STREAM_TRACE=1
```

`ZLINK_JAVA_STREAM_TRACE` is a version-specific diagnostic switch of the connector, so do not use it as a routing API. Let the runner clean up its own processes and Compose project, preserve cleanup failures, and avoid stopping unrelated processes or containers.

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
* zlink version: <Framework and binding versions from build.gradle.kts>
* OS: <OS, JDK version>

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

Put the language and the failing behavior in the title (for example, `framework(java): Spot startup fails with empty SpotId`). Record the filed issue number in the README Known issues so the project keeps the place to re-verify when a fixed package is released.
