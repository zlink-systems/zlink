# MyGame development guide

## Project overview

This Kotlin project (Kotlin 2.3.21 on a Java 25 toolchain, built with Gradle) is a MORPG example in which players gather in town channels, form parties, and enter private dungeons. It runs a Session gateway for connections, two Town hosts (`townA` and `townB`), two Instance hosts (`instanceA` and `instanceB`), and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework packages to `0.30.2` (`frameworkVersion`) and the ZLink binding to `1.20.0` (`bindingVersion`).

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Kotlin guide](https://docs.zlink.systems/kotlin/guide/server/), [API reference](https://docs.zlink.systems/kotlin/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the packages pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host JDK. Run these commands from this directory.

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

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only together with `--local`/`-Local` and an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README. Run the build and e2e on both Windows and Linux. The runner validates the structure before it starts nodes, and each run uses its own Redis key prefix, Compose project, and OS-assigned ports; keep that isolation when changing the runner.

## Project layout and extension points

### Session gateway

`server/session/src/main/kotlin/MyGame/session/Program.kt` registers the STREAM node and the RouteMesh client, and `GameSession` (in the same file) handles incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession` answers `SessionInfoRequest` from the binding, finds or creates the Player Actor and binds it when it receives `JoinTownRequest`, and relays every other packet to the bound Actor. Add authentication before binding and retain the typed packet relay so Framework can identify the connection for replies to that player.

### Towns and players

An Actor is a game state object addressed by ID; `Player` in `server/shared/src/main/kotlin/MyGame/Player.kt` keeps no application state and is shared by the Town and Instance nodes. A Spot is a state object that accepts Actors and executes shared game rules; `Town` in `server/town/src/main/kotlin/MyGame/town/Town.kt` is a fixed User Spot (one per channel: `town-1`, `town-2`) that owns the channel's players and parties in `TownState`. `Entry` and `EntryActions` in `server/shared/src/main/kotlin/MyGame/admission/Entry.kt` receive new Actors and send them to the town of the requested channel.

`server/town/src/main/kotlin/MyGame/town/Program.kt` registers the handlers, the Entry Spot, the `Town` factory (`stableTypeLimit(config.townCapacity)`, `SPOT_WIDE`), and the `Player` factory. `Actions` in `server/town/src/main/kotlin/MyGame/town/Handlers.kt` connects player requests (channel change, party, dungeon entry) and pushes to the Town. `Hosting.kt` creates the fixed Towns listed in `townIds` when a node starts. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/instance/src/main/kotlin/MyGame/instance/Program.kt` registers the Entry Spot, the `Dungeon` factory (`stableTypeLimit(config.dungeonCapacity)`, `SPOT_WIDE`), and the `Player` factory. `Dungeon` in `server/instance/src/main/kotlin/MyGame/instance/Dungeon.kt` is a User Spot that owns the party members and the stage in `DungeonState`, admits only the members named by `prepare`, and pushes `DungeonView`. `Preparation` and `Actions` in `server/instance/src/main/kotlin/MyGame/instance/Handlers.kt` handle the Town's `PrepareDungeon` request and the player requests. Add encounters, rewards, and Dungeon lifecycle behavior in this module.

### Shared contracts and hosting

`server/shared/src/main/kotlin/MyGame/Contracts.kt` defines names, game constants, and the messages shared by processes. `Hosting.kt` defines `NodeConfig` (process settings), the Redis stores, `configure` for hosting and diagnostics, and `runNode`, which starts a node and handles the stdin control commands; `Control.kt` defines the control and readiness strings. `Names` holds the names, `townId`, and `dungeonId`, `Rejection` the rejection reasons; `Player.kt` defines `Player`, its factory, and its adapter for both node types, and `Transfer.kt` defines `ensureActive`. Keep hosting and the control protocol here instead of duplicating them per role. Add contracts used by several processes in `Contracts.kt`, and keep town state in `Town` and dungeon state in `Dungeon` so separate processes do not independently modify the same fact.

### E2E client

`e2e/src/main/kotlin/MyGame/e2e/Client.kt` connects through the public stream connector, and `Scenarios.kt` checks results observable by the client. `Nodes.kt` manages the processes and the Compose project for each run, `Structure.kt` validates the composition, and `Program.kt` selects scenarios and sets the exit code (a failed scenario or a failed cleanup exits with 1). `DockerRuntime.kt` runs nodes as containers in Docker mode, and `Cleanup.kt` collects cleanup failures. Verify new gameplay through received client results. The project includes no separate unit-test module.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-world`). Channels distinguish messages by purpose and name; this project declares no application channels (`channels` is empty in `zlink.structure.json`). The town channels of the game design (`town-1`, `town-2`) are Town Spots, not messaging channels. [Channel messaging](https://docs.zlink.systems/kotlin/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles Actor entry, `Town` is a fixed User Spot for each channel, and `Dungeon` is a User Spot that a Town creates for a party. The [Spot guide](https://docs.zlink.systems/kotlin/guide/server/21-spot/) describes registration and calls for these objects. `Player` is addressed by player ID and keeps no application state; the [Actor guide](https://docs.zlink.systems/kotlin/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/kotlin/guide/server/23-stream/), which processes packets on client connections, `GameSession` obtains the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/kotlin/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation (`ZLinkRedisLocationStore` in `Hosting.kt`). Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/kotlin/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `EntryActions.join` requests deferred admission to the town of the requested channel with `joinSpot(...).defer()`, and `Actions.change` requests admission to another town the same way. `Town.onActorJoinSuspending` accepts when the player and channel match. After `Town.enter` prepares the Dungeon, `Actions.travel` requests deferred admission to it, and `Dungeon.onActorJoinSuspending` accepts only the prepared members. [Actor membership](https://docs.zlink.systems/kotlin/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `runNode` in `Hosting.kt` requests host relocation when it reads `drain` and reports `SAFE_TO_SHUTDOWN` after observing it; `TownTransfer` and `DungeonTransfer` store the Spot state as JSON and `PlayerTransfer` stores nothing. The [Relocation guide](https://docs.zlink.systems/kotlin/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinTownRequest` (player ID and channel). `GameSession` calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session with `bindOrGet`, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. `EntryActions.join` checks the identity and requests `joinSpot(Names.townId(channel), request).defer()`. `Town.onActorJoinSuspending` adds the player to `TownState`, and `Town.onJoinedActorSuspending` broadcasts `TownView`.
3. `ChangeChannelRequest` reaches `Actions.change`, which requests `joinSpot` to the town of the new channel. The client connection and `SessionInfo` do not change, even when that town is on another node.
4. `CreatePartyRequest` and `JoinPartyRequest` reach `Actions.create` and `Actions.joinParty`, which call `Town.party`; an invalid request is answered with `ActionRejected`. `EnterDungeonRequest` reaches `Actions.enter` and `Town.enter`: a non-leader receives `NOT_LEADER`; otherwise `Town.enter` creates `dungeon-<partyId>` with `getOrCreate`, sends `PrepareDungeon` with `requestToSpot` to `Preparation.prepare`, and pushes `JoinDungeon` to every member.
5. `Actions.travel` receives `JoinDungeon` and requests `joinSpot(dungeonId, message).defer()`. `Dungeon.onJoinedActorSuspending` pushes `DungeonView`, and `AdvanceRequest` reaches `Actions.advance`, which raises the stage and pushes `DungeonView` to every member through the `push` handler.
6. A reconnecting client sends `JoinTownRequest` on a new Session. The same Actor is bound, and `Actions.reconnect` in the Spot that holds the Actor (`Town` or `Dungeon`) resends the current view. A Session disconnect does not remove the Actor from its Spot.
7. To replace a node, `drain` in `runNode` requests relocation. Framework takes the application state from `capture` in `TownTransfer`, `DungeonTransfer`, and `PlayerTransfer` and calls `restore` on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the Kotlin starter: choose Kotlin in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/java/templates/kotlin/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `morpg-channel-town`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature | File in this sample | What to transfer | What to grow in the starter alongside |
| --- | --- | --- | --- |
| Typed messages | `server/shared/src/main/kotlin/MyGame/Contracts.kt` | The classes of the features you adopt: `JoinTownRequest`, `ChangeChannelRequest`, `CreatePartyRequest`, `JoinPartyRequest`, `EnterDungeonRequest`, `AdvanceRequest`, `ViewRequest`, `PartyView`, `TownView`, `DungeonView`, `PrepareDungeon`, `DungeonPrepared`, `JoinDungeon`, `ActionRejected`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The names, `townId`, and `dungeonId` in `Names` and the reasons in `Rejection`; the starter already has the node `Roles` and the control strings |
| Actor | `server/shared/src/main/kotlin/MyGame/Player.kt` | `Player` with `onJoinCompletedSuspending` (rejection reported as `ActionRejected`), `PlayerFactory`, and `PlayerTransfer` | A new `server/town/` Gradle module (included in `settings.gradle.kts`) with its `Program.kt` and the `server/config/townA.json`, `server/config/townB.json` files, its start-up in `e2e/.../Nodes.kt`, and a `Roles` entry; the `addActorFactory` registration in `Program.kt`; the node declarations in `zlink.structure.json` and the matching `Structure.kt` check (the starter's check accepts only the `session` role) |
| Spot | `server/town/src/main/kotlin/MyGame/town/Town.kt` | `Town` with `TownState` (players and parties), admission, the party rules, and the `Actions` handlers | The `addSpotFactory` registration in `Program.kt` (`stableTypeLimit`, `SPOT_WIDE`) and `townCapacity` and `townIds` in `NodeConfig`, with the Town creation in `Hosting.kt`; the `spots` declarations in `zlink.structure.json` and the `Structure.kt` check that compares them with the registrations (the starter's check requires no Spots) |
| Entry and first admission | `server/shared/src/main/kotlin/MyGame/admission/Entry.kt` (`Entry`, `EntryActions`), `Actions.reconnect` in `server/town/src/main/kotlin/MyGame/town/Handlers.kt` | `EntryActions.join`: the identity check and the deferred join to the town of the requested channel, and the identity check and view resend of `Actions.reconnect` | The `addEntrySpot` registration, `addHandlersFromPackageOf<EntryActions>()`, and the `entry` declaration; the `channel` scenario for first entry and the `reconnect` scenario that compares state after reconnection |
| Session binding | `server/session/src/main/kotlin/MyGame/session/Program.kt` (`GameSession`) | The first-packet branch: find or create the Actor, `bindOrGet`, and relay the original typed packet | The same first-packet type as `EntryActions.join`; transfer it into the starter's `PlayerSession.kt` |
| Host relocation | `TownTransfer` in `server/town/src/main/kotlin/MyGame/town/Town.kt`, `DungeonTransfer` in `server/instance/src/main/kotlin/MyGame/instance/Dungeon.kt`, `PlayerTransfer` in `server/shared/src/main/kotlin/MyGame/Player.kt`, and `server/shared/src/main/kotlin/MyGame/Transfer.kt` | `TownState` and `DungeonState` stored as JSON with `capture` and `restore`, the empty Player adapter, and `ensureActive` | The `preserveStateWith` registrations; the `update` scenario, which checks that connections and dungeon state survive a server replacement and that the replacement node hosts a new dungeon |
| Dynamic dungeon | `Town.enter` in `server/town/src/main/kotlin/MyGame/town/Town.kt`; `server/instance/src/main/kotlin/MyGame/instance/Program.kt`, `server/instance/src/main/kotlin/MyGame/instance/Dungeon.kt`, `Preparation` in `server/instance/src/main/kotlin/MyGame/instance/Handlers.kt` | The dynamic Dungeon creation with `getOrCreate` and `requestToSpot` preparation, the prepared-member admission, and the stage advance | A new `server/instance/` Gradle module with `instanceA.json` and `instanceB.json`, its start-up, and role; `dungeonCapacity` in `NodeConfig`; the dynamic `dungeon` Spot and the `GetOrCreate` edge in `zlink.structure.json`; the `dungeon` scenario |
| World/channel selection | `Actions.change` in `server/town/src/main/kotlin/MyGame/town/Handlers.kt`, `Names.townId` in `server/shared/src/main/kotlin/MyGame/Contracts.kt` | The channel change as a deferred Spot join to the town of the new channel | A second Town node and its fixed Town in `townIds`; the `channel` scenario, which checks that the connection and `SessionInfo` survive the change |

The channels of this sample are player-selected towns (Town Spots). They differ from messaging subscription channels, and this sample registers no pub/sub subscriptions.

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to `Names.SCENARIOS` in `Contracts.kt`, to the `scenario` function in `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the typed classes in `Contracts.kt` first, then move each handler that fails to compile to the new messages. When you change the first packet (`JoinTownRequest`), update the first-packet branch in `GameSession`, `EntryActions.join`, and the `reconnect` handlers of `Town` and `Dungeon` to receive the same type.

Keep each piece of state in the object that decides it. The players and parties belong in `Town`, the members and stage in `Dungeon`, and `Player` keeps no state. When you change `TownState` or `DungeonState` fields, update `TownTransfer` or `DungeonTransfer` together. A missing field disappears after relocation, and the `update` scenario detects the loss.

Prepare the Dungeon before any member joins it: `Town.enter` waits for `Preparation.prepare` to answer and only then pushes `JoinDungeon`, because `Dungeon.onActorJoinSuspending` admits only the prepared members. Capacity is one Town per Town node (`townCapacity`) and one Dungeon per Instance node (`dungeonCapacity`) so that placement is observable; keep that arrangement when you add a scenario that creates them.

Declare one Entry in the structure for each Town and Instance node, which register the shared `Entry`, and none for other roles; the Framework's default Entries are not part of the structure. Declare the dynamic `Dungeon` for every Instance node that owns it. Start the Town nodes one at a time and wait for each `READY` before starting the next, with `townCapacity` equal to the node's fixed Town count (`townIds`), so that earlier nodes are full and later nodes are absent during bootstrap and only the declared owner can create each fixed Town. The structure check enforces the exact Town capacity and one Dungeon per Instance node.

To name roles differently from this sample, change the type names, `Names`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use the public packages from Maven Central at the versions pinned in `build.gradle.kts`, and keep project dependencies between this project's own Gradle modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters (the `*Transfer` classes) to application state, keeping management of Framework execution state separate from game state. Framework owns routing, leases, membership, completion, queues, and execution gates; do not duplicate those decisions in application state.

Manage each game fact, such as the party roster, in the object responsible for changing it (`Town`). Replies inside an Actor handler go through that Actor's bound Session; other pushes address the Actor by player ID with the public Actor client, and the receiving Actor handler forwards them through its bound Session. A Spot must not retain Actor references for later delivery because host relocation does not repeat `onJoinedActor` on the target. Session binding owns Actor identity, so game messages do not duplicate it (`SessionInfo` reads the current binding). Sessions forward packets after binding; the Actor or Spot responsible for the state makes gameplay decisions.

### Asynchronous execution and lifecycle

Await public calls with `await()` as the template does, and avoid blocking calls such as `.join()`, `.get()`, `runBlocking`, or `Thread.sleep` in handlers and callbacks. `Town` and `Dungeon` are registered with `SPOT_WIDE`, which gives Spot handlers, member Actor handlers, and lifecycle callbacks of the same Spot a shared execution gate. Do not protect the same state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/kotlin/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. The fixed `Town` Spots are created at node startup and `Dungeon` Spots are created on demand by `Town.enter`, both with `getOrCreate`; [Activation and lifetime](https://docs.zlink.systems/kotlin/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback (`onJoinCompletedSuspending`) for membership changes, and relocation with runtime observations (`drain`, then `SAFE_TO_SHUTDOWN`) for host shutdown, so request acceptance is not mistaken for completion. `Control.kt` derives the safe-shutdown wait (`SAFE_DEADLINE`) from the Message Follow duration (`MESSAGE_FOLLOW`) and the host deadline (`HOST_DEADLINE`); change them there only.

### Reconnection and failure handling

Reconnect with the same player ID and bind the new Session to the same Actor to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/kotlin/guide/server/33-backpressure/). A disconnected Session keeps the Actor in its Town or Dungeon until an explicit move, so reconnecting needs no new admission.

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, `.gradle/`, and `.kotlin/` and license files (`zlink.license`) from commits. The project includes no formatter configuration; format Kotlin sources with ktfmt `--kotlinlang-style` from this directory only.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. The runner prints `Logs: <path>` at start. `.logs/run-<id>/` contains the effective settings that `Nodes.kt` writes at node startup (`session.json`, `townA.json`, `townB.json`, `instanceA.json`, `instanceB.json`) and one directory per node start, `<node>-<uuid>/`, with `<node>-stdout.log` (readiness and control lines) and `<node>-stderr.log` (Framework logs and message flow). A replaced node reuses its settings file and starts a new directory.

In local mode, `compose.log` records the Compose commands, and the local build output is `.logs/build.log`. In Docker mode, the build, Redis, and outer cleanup logs are in `.logs/docker-<project>/` (`build.log`, `redis-start.log`, `redis.log`, `cleanup.log`).

### Diagnostic levels

`configure` in `server/shared/src/main/kotlin/MyGame/Hosting.kt` enables message flow, which records the stages from message receipt to completion, with this setting.

```kotlin
options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL)
```

`NORMAL` records receive, dispatch, and completion stages and errors; `ERRORS` limits the records to errors, and `OFF` disables them. Use `DETAILED` for timing detail, and also call `includeMessageSizes(true)` on the same dispatch options for size recording. Detailed recording adds cost, so use it for the required investigation scope.

`NodeConfig` defines no diagnostics JSON key, so change the `configure` call, rebuild, and restart to apply startup settings. `ZLinkFrameworkRuntime.setMessageFlowModeAsync` changes the mode at runtime, but this project's stdin controls do not call it. [Monitoring](https://docs.zlink.systems/kotlin/guide/server/26-monitoring/) and [Observability diagnostics](https://docs.zlink.systems/kotlin/reference/08-observability-diagnostics/) describe diagnostic records and settings.

### Message flow investigation

Framework logs, including message flow, go to stderr (`server/shared/src/main/resources/logback.xml` configures the appender), so inspect message flow in the `<node>-stderr.log` files; the template creates no separate flow file. A record's `flow` value identifies a message across nodes, and `corr` relates request and reply records. Flow records contain `zlink flow:`; failures have `outcome=failed`, `error_type`, and `error_message`.

Read the failure fields and preceding stage together without filtering categories out. Flow IDs differ between runs, so compare passing and failing runs by logical operation, Actor ID, and stage order to find the first missing or failed stage. If startup fails before message flow recording, preserve the startup exception and stack.

### Supplementary tracing and cleanup

For supplementary STREAM connector tracing in the e2e runner, set the environment variable in the parent process before a `--local` run. The Docker runner and nodes do not inherit host variables; add diagnostic environment values explicitly to the Compose service or node creation when required. In PowerShell, use this environment variable.

```powershell
$env:ZLINK_JAVA_STREAM_TRACE = "1"
```

The value applies to child processes started from that environment. In Bash, set the same variable as follows.

```bash
export ZLINK_JAVA_STREAM_TRACE=1
```

`ZLINK_JAVA_STREAM_TRACE` is a version-specific diagnostic switch in the public `0.30.2` package, so do not use it as a routing API. Let the runner clean up its own processes and Compose project, keep its Docker socket control path, preserve cleanup failures, and avoid stopping unrelated processes or containers.

## Reporting ZLink defects

### Identifying a defect

A ZLink defect is a case where a public API called as documented produces a result that differs from the documentation. First compare your API usage with the public guides and samples linked above to rule out an application usage error. If the usage is correct, report an issue instead of adding a workaround, and keep the scenario assertions unchanged. A workaround hides the defect and stays in the code after you upgrade to a fixed package, where it changes behavior.

### Checking existing issues

Before filing, search both open and closed [ZLink issues](https://github.com/zlink-systems/zlink/issues?q=is%3Aissue) by error message, exception type, and API name. If the same defect is already reported, add your environment and logs to that issue instead of opening a new one. If it matches a closed issue, find the package version that contains the fix in that issue, raise `frameworkVersion` in `build.gradle.kts`, and run again.

### Issue body

The body follows the issue template of the ZLink repository. Copy the following form and fill it in.

```markdown
# Issue description
<scenario and the public operation that fails>

# Environment
* zlink version: <frameworkVersion and bindingVersion from build.gradle.kts>
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

Put the language and the failing behavior in the title (for example, `framework(kotlin): Spot startup fails with empty SpotId`). Record the filed issue number in the README Known issues so the project keeps the place to re-verify when a fixed package is released.
