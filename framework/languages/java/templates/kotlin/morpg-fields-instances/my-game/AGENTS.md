# MyGame development guide

## Project overview

This Kotlin project (Kotlin 2.3.10 on a Java 25 toolchain, built with Gradle) is a MORPG example in which players move across shared fields on several nodes, see each other at field borders, and enter party dungeons. It runs a Session gateway for connections, two Field hosts (`fieldA` and `fieldB`) with four fixed fields, two Instance hosts (`instanceA` and `instanceB`), and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework packages to `0.30.1` (`frameworkVersion`) and the ZLink binding to `1.19.0` (`bindingVersion`).

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Kotlin guide](https://docs.zlink.systems/kotlin/guide/server/), [API reference](https://docs.zlink.systems/kotlin/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the packages pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host JDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario field,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario field,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only together with `--local`/`-Local` and an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README. Run the build and e2e on both Windows and Linux. The runner validates the structure before it starts nodes, and each run uses its own Redis key prefix, Compose project, and OS-assigned ports; keep that isolation when changing the runner.

## Project layout and extension points

### Session gateway

`server/session/src/main/kotlin/MyGame/session/Program.kt` registers the STREAM node and the RouteMesh client, and `GameSession` (in the same file) handles incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession` answers `SessionInfoRequest` from the binding, finds or creates the Player Actor and binds it when it receives `JoinWorldRequest`, and relays every other packet to the bound Actor. Add authentication before binding and retain the typed packet relay so Framework can identify the connection for replies to that player.

### Fields and players

An Actor is a game state object addressed by ID; `Player` in `server/shared/src/main/kotlin/MyGame/Player.kt` manages the player's `position` (`EnterField`: coordinate `x` and `sequence`) and is shared by the Field and Instance nodes. A Spot is a state object that accepts Actors and executes shared game rules; `Field` in `server/field/src/main/kotlin/MyGame/field/Field.kt` is a fixed User Spot that manages the field's visibility, pending arrivals, and parties in `FieldState`. Visibility values are copies of Actor movement state used for observation, so apply player state changes to the Actor. `Entry` and `EnterWorld` in `server/shared/src/main/kotlin/MyGame/admission/Entry.kt` receive new Actors and send them to the field that covers their coordinate.

`server/field/src/main/kotlin/MyGame/field/Program.kt` registers the handlers, the Entry Spot, the `Field` factory (`stableTypeLimit(config.fieldCapacity)`, `SPOT_WIDE`), the `Player` factory, and the `border` channel server. `Actions` in `server/field/src/main/kotlin/MyGame/field/Handlers.kt` connects player requests (movement, party, dungeon entry) and pushes to the Field, and `BorderTick` publishes the border information. `Hosting.kt` creates the fixed Fields listed in `fieldIds` when a node starts. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Border subscriptions

`server/field/src/main/kotlin/MyGame/border/BorderRoutes.kt` declares one subscription handler class per adjacent Field pair, because annotation topics are compile-time constants (`BorderTopics`) and the Kotlin Spot handler registry cannot yet register a runtime `(channelName, topic)` subscription (see Known issues in the README). `Field.configure()` calls `BorderRoutes.register` for each neighbor. A new adjacent pair needs a new handler class and a `when` branch there.

### Dungeons

`server/instance/src/main/kotlin/MyGame/instance/Program.kt` registers the Entry Spot, the `Dungeon` factory (`stableTypeLimit(config.dungeonCapacity)`, `SPOT_WIDE`), and the `Player` factory. `Dungeon` in `server/instance/src/main/kotlin/MyGame/instance/Dungeon.kt` is a User Spot that owns the party members and the stage in `DungeonState`, admits only the members named by `prepare`, and pushes `DungeonView`. `Prepare` and `DungeonActions` in `server/instance/src/main/kotlin/MyGame/instance/Handlers.kt` handle the Field's `PrepareDungeon` request and the player requests. Add encounters, rewards, and Dungeon lifecycle behavior in this module.

### Shared contracts and hosting

`server/shared/src/main/kotlin/MyGame/Contracts.kt` defines names, game constants, and the messages shared by processes. `Hosting.kt` defines `NodeConfig` (process settings), the Redis stores, `configure` for hosting and diagnostics, and `runNode`, which starts a node and handles the stdin control commands; `Control.kt` defines the control and readiness strings. `Names` holds the names and scenario IDs and `Rules` the field width and count, the border radius and tick, the party limit, and the helpers `fieldId`, `adjacentFields`, `borderTopic`, and `dungeonId`; `Player.kt` defines `Player`, its factory, and its adapter; `Transfer.kt` defines `ensureActive`. Keep hosting and the control protocol here instead of duplicating them per role. Add contracts used by several processes in `Contracts.kt`, and keep player state in `Player` so separate processes do not independently modify the same fact.

### E2E client

`e2e/src/main/kotlin/MyGame/e2e/Client.kt` connects through the public stream connector, and `Scenarios.kt` checks results observable by the client. `Nodes.kt` manages the processes and the Compose project for each run, `Structure.kt` validates the composition, and `Program.kt` selects scenarios and sets the exit code (a failed scenario or a failed cleanup exits with 1). `DockerRuntime.kt` runs nodes as containers in Docker mode, and `Cleanup.kt` collects cleanup failures. Verify new gameplay through received client results. The project includes no separate unit-test module.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-world`). Channels distinguish messages by purpose and name, and the `border` channel (`Names.BORDER`) carries typed publications between Fields. [Channel messaging](https://docs.zlink.systems/kotlin/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles Actor entry, `Field` is a fixed User Spot for each field, and `Dungeon` is a User Spot that a Field creates for a party. The [Spot guide](https://docs.zlink.systems/kotlin/guide/server/21-spot/) describes registration and calls for these objects. `Player` retains movement state when its host Spot changes; the [Actor guide](https://docs.zlink.systems/kotlin/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/kotlin/guide/server/23-stream/), which processes packets on client connections, `GameSession` obtains the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/kotlin/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation (`ZLinkRedisLocationStore` in `Hosting.kt`). Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/kotlin/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Player.move` requests deferred admission to the next Field with `joinSpot(...).defer()`, `Field.onActorJoinSuspending` decides, and `onJoinCompletedSuspending` observes the completion result. After `Field.enter` prepares the Dungeon, `Actions.travel` requests deferred admission to it, and `Dungeon.onActorJoinSuspending` accepts only the prepared members. [Actor membership](https://docs.zlink.systems/kotlin/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `runNode` in `Hosting.kt` requests host relocation when it reads `drain` and reports `SAFE_TO_SHUTDOWN` after observing it; `PlayerTransfer` stores the position and `FieldTransfer` and `DungeonTransfer` store the Spot state as JSON. The [Relocation guide](https://docs.zlink.systems/kotlin/guide/server/37-relocation/) explains the procedure.

### Border notifications and timers

A timer runs a handler periodically; `BorderTick` uses one to publish information about players near a border. In this pub/sub exchange, the `Route*` subscription handlers in `BorderRoutes.kt` receive subscribed information and call `Field.showBorder`, and `Actions.deliver` forwards the notification through the receiving Actor's bound Session. [Timers and workers](https://docs.zlink.systems/kotlin/guide/server/36-timer-worker/) explains registration and execution of periodic work.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinWorldRequest`. `GameSession` calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session with `bindOrGet`, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. `EnterWorld.join` checks the identity and calls `Player.move`, which requests `joinSpot(...).defer()` for the Field that covers the coordinate.
3. When the join completes, `Player.onJoinCompletedSuspending` sends `WorldState` through the bound Session. Later packets from the same client are handled by the `Actions` handlers of the Field that holds the Actor (such as `move`), which send results through the same Session.
4. When movement reaches a coordinate in another Field, `Actions.move` calls `Player.move` again. The client connection does not change, even when that Field is on another node.
5. Every `Rules.TICK_MILLIS`, `BorderTick` publishes border presence on the `border` channel. The `Route*` handlers call `Field.showBorder`, which sends `BorderVisible` to nearby observers with `sendToActor`, and `Actions.deliver` forwards it through the observer's bound Session.
6. `CreatePartyRequest` and `JoinPartyRequest` reach `Actions.createParty` and `Actions.joinParty`, which call `Field.party`; an invalid request is answered with `ActionRejected`. `EnterDungeonRequest` reaches `Actions.enterDungeon` and `Field.enter`: a non-leader receives `NOT_LEADER`; otherwise `Field.enter` creates `dungeon-<partyId>` with `getOrCreate`, sends `PrepareDungeon` with `requestToSpot` to `Prepare.prepare`, and pushes `JoinDungeon` to every member. `Actions.travel` then requests the join, and `AdvanceRequest` reaches `DungeonActions.advance`.
7. A reconnecting client sends `JoinWorldRequest` on a new Session. The same Actor is bound, and `Actions.reconnect` in the Spot that holds the Actor (`Field`) or `DungeonActions.reconnect` (`Dungeon`) resends the current state. A Session disconnect removes the player from the field's visibility list but not from its party.
8. To replace a node, `drain` in `runNode` requests relocation. Framework takes the state from `capture` in `PlayerTransfer`, `FieldTransfer`, and `DungeonTransfer` and calls `restore` on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the Kotlin starter: choose Kotlin in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/java/templates/kotlin/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `morpg-fields-instances`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature | File in this sample | What to transfer | What to grow in the starter alongside |
| --- | --- | --- | --- |
| Typed messages | `server/shared/src/main/kotlin/MyGame/Contracts.kt` | The classes of the features you adopt: `JoinWorldRequest`, `MoveRequest`, `EnterField`, `WorldState`, `BorderPresence`, `BorderVisible`, `CreatePartyRequest`, `JoinPartyRequest`, `EnterDungeonRequest`, `ViewRequest`, `AdvanceRequest`, `PartyView`, `FieldView`, `PrepareDungeon`, `DungeonPrepared`, `JoinDungeon`, `DungeonView`, `ActionRejected`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The names in `Names` and the field helpers in `Rules`; the starter already has the node `Roles` and the control strings |
| Actor | `server/shared/src/main/kotlin/MyGame/Player.kt` | `Player` with `position` (`EnterField`), `move`, `apply`, `sendState`, `PlayerFactory`, and `PlayerTransfer` | A new `server/field/` Gradle module (included in `settings.gradle.kts`) with its `Program.kt` and the `server/config/fieldA.json`, `server/config/fieldB.json` files, its start-up in `e2e/.../Nodes.kt`, and a `Roles` entry; the `addActorFactory` registration in `Program.kt`; the node declarations in `zlink.structure.json` and the matching `Structure.kt` check (the starter's check accepts only the `session` role) |
| Spot | `server/field/src/main/kotlin/MyGame/field/Field.kt` | `Field` with `FieldState` (visibility, pending arrivals, parties), the admission rule, and the border calculation | The `addSpotFactory` registration in `Program.kt` (`stableTypeLimit`, `SPOT_WIDE`) and `fieldIds` and `fieldCapacity` in `NodeConfig`, with the Field creation in `Hosting.kt`; the `spots` declarations in `zlink.structure.json` and the `Structure.kt` check that compares them with the registrations (the starter's check requires no Spots) |
| Entry and first admission | `server/shared/src/main/kotlin/MyGame/admission/Entry.kt` (`Entry`, `EnterWorld`), `Actions.reconnect` in `server/field/src/main/kotlin/MyGame/field/Handlers.kt` | `EnterWorld.join`, the first-entry path, and the identity check and state resend of `Actions.reconnect` | The `addEntrySpot` registration, `addHandlersFromPackageOf<EnterWorld>()`, and the `entry` declaration; the `field` scenario for first entry and the `reconnect` scenario that compares state after reconnection |
| Session binding | `server/session/src/main/kotlin/MyGame/session/Program.kt` (`GameSession`) | The first-packet branch: find or create the Actor, `bindOrGet`, and relay the original typed packet | The same first-packet type as `EnterWorld.join`; transfer it into the starter's `PlayerSession.kt` |
| Host relocation | `FieldTransfer` in `server/field/src/main/kotlin/MyGame/field/Field.kt`, `DungeonTransfer` in `server/instance/src/main/kotlin/MyGame/instance/Dungeon.kt`, `PlayerTransfer` in `server/shared/src/main/kotlin/MyGame/Player.kt`, and `server/shared/src/main/kotlin/MyGame/Transfer.kt` | The snapshots (`EnterField`, `FieldState`, `DungeonState`) with `capture` and `restore`, and `ensureActive` | The `preserveStateWith` registrations; the `update` scenario, which checks that connections and dungeon state survive a server replacement and that the replacement node hosts a new dungeon |
| Dynamic dungeon | `Field.enter` in `server/field/src/main/kotlin/MyGame/field/Field.kt`; `server/instance/src/main/kotlin/MyGame/instance/Program.kt`, `server/instance/src/main/kotlin/MyGame/instance/Dungeon.kt`, `Prepare` in `server/instance/src/main/kotlin/MyGame/instance/Handlers.kt` | The dynamic Dungeon creation with `getOrCreate` and `requestToSpot` preparation, the prepared-member admission, and the stage advance | A new `server/instance/` Gradle module with `instanceA.json` and `instanceB.json`, its start-up, and role; `dungeonCapacity` in `NodeConfig`; the dynamic `dungeon` Spot and the `GetOrCreate` edge in `zlink.structure.json`; the `dungeon` scenario |
| Channel pub/sub and seamless movement | `Field.configure()`, `publishBorder`, and `showBorder` in `server/field/src/main/kotlin/MyGame/field/Field.kt`; `server/field/src/main/kotlin/MyGame/border/BorderRoutes.kt`; `BorderTick` and `Actions.deliver` in `server/field/src/main/kotlin/MyGame/field/Handlers.kt` | The border subscription and publication, and the delivery to the receiving Actor's bound Session | The `border` channel server registration in `Program.kt` and its `channels` declaration in `zlink.structure.json`; the `field` scenario, which checks that a border publication reaches an observer |

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to `Names.SCENARIOS` in `Contracts.kt`, to the `scenario` function in `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the typed classes in `Contracts.kt` first, then move each handler that fails to compile to the new messages. When you change the first-entry packet (`JoinWorldRequest`), update the first-packet branch in `GameSession`, `EnterWorld.join`, and the `reconnect` handlers of `Field` and `Dungeon` to receive the same type.

Keep each piece of state in the object that decides it. Per-player state belongs in `Player`; state shared by several players (visibility, parties, dungeon members and stage) belongs in `Field` or `Dungeon`. When you change state fields, update that object's snapshot class (`EnterField`, `FieldState`, `DungeonState`) and its `capture` and `restore` together. A missing field disappears after relocation, and the `update` scenario detects the loss.

Add a channel, its subscription, and its declaration in the same change: the subscription handlers in `BorderRoutes.kt` (registered from `Field.configure()`), the channel server registration in `Program.kt`, and the `channels` declaration in `zlink.structure.json`. The structure check requires every Field node to declare the `border` channel.

Prepare the Dungeon before any member joins it: `Field.enter` waits for `Prepare.prepare` to answer and only then pushes `JoinDungeon`, because `Dungeon.onActorJoinSuspending` admits only the prepared members. The non-field scenarios share one party fixture, so selecting `reconnect` or `update` alone creates that fixture first.

Register the shared `Entry` and `EnterWorld` handlers on every node that registers the `Player` factory, and declare an Entry in the structure only for the Field and Instance nodes; the Framework's default Entries are not part of the structure. The structure check requires those Entries and every registered User Spot type. Start the Field nodes one at a time and wait for each `READY` after its bootstrap, with `fieldCapacity` equal to the node's fixed Field count (`fieldIds`), so that earlier nodes are full.

To name roles differently from this sample, change the type names, `Names`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use the public packages from Maven Central at the versions pinned in `build.gradle.kts`, and keep project dependencies between this project's own Gradle modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters (the `*Transfer` classes) to application state, keeping management of Framework execution state separate from game state. Framework owns routing, leases, membership, completion, queues, and execution gates; do not duplicate those decisions in application state.

Manage each game fact, such as a player's coordinate, in the object responsible for changing it (`Player`). Store player IDs and observational copies in Spots (`FieldState.visiblePlayers`), without assuming the same Actor object reference remains usable after movement. Replies inside an Actor handler go through that Actor's bound Session; other pushes address the Actor by player ID with the public Actor client, and the receiving Actor handler forwards them through its bound Session. A Spot must not retain Actor references for later delivery because host relocation does not repeat `onJoinedActor` on the target. Session binding owns Actor identity, so game messages do not duplicate it (`SessionInfo` reads the current binding). Sessions forward packets after binding; the Actor or Spot responsible for the state makes gameplay decisions.

### Asynchronous execution and lifecycle

Await public calls with `await()` as the template does, and avoid blocking calls such as `.join()`, `.get()`, `runBlocking`, or `Thread.sleep` in handlers and callbacks. `Field` and `Dungeon` are registered with `SPOT_WIDE`, which gives Spot handlers, member Actor handlers, timers, and lifecycle callbacks of the same Spot a shared execution gate. Do not protect the same state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/kotlin/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. The fixed `Field` Spots are created at node startup and `Dungeon` Spots are created on demand by `Field.enter`, both with `getOrCreate`; [Activation and lifetime](https://docs.zlink.systems/kotlin/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback (`onJoinCompletedSuspending`) for membership changes, and relocation with runtime observations (`drain`, then `SAFE_TO_SHUTDOWN`) for host shutdown, so request acceptance is not mistaken for completion. `Control.kt` derives the safe-shutdown wait (`SAFE_DEADLINE`) from the Message Follow duration (`MESSAGE_FOLLOW`) and the host deadline (`HOST_DEADLINE`); change them there only.

### Reconnection and failure handling

Reconnect with the same player ID and bind the new Session to the same Actor to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/kotlin/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, `.gradle/`, and `.kotlin/` and license files (`zlink.license`) from commits. The project includes no formatter configuration; format Kotlin sources with ktfmt `--kotlinlang-style` from this directory only.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. The runner prints `Logs: <path>` at start. `.logs/run-<id>/` contains the effective settings that `Nodes.kt` writes at node startup (`session.json`, `fieldA.json`, `fieldB.json`, `instanceA.json`, `instanceB.json`) and one directory per node start, `<node>-<uuid>/`, with `<node>-stdout.log` (readiness and control lines) and `<node>-stderr.log` (Framework logs and message flow). A replaced node reuses its settings file and starts a new directory.

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

`ZLINK_JAVA_STREAM_TRACE` is a version-specific diagnostic switch in the public `0.30.1` package, so do not use it as a routing API. Let the runner clean up its own processes and Compose project, keep its Docker socket control path, preserve cleanup failures, and avoid stopping unrelated processes or containers.

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
