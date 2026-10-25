# MyGame development guide

## Project overview

This Kotlin project (Kotlin 2.3.21 on a Java 25 toolchain, built with Gradle) is a MORPG example in which players move across shared fields on several nodes, see each other at field borders, and enter party dungeons. It runs a Session gateway for connections, two Field hosts (`fieldA` and `fieldB`) with four fixed fields, two Instance hosts (`instanceA` and `instanceB`), and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework packages to `0.30.2` (`frameworkVersion`) and the ZLink binding to `1.20.0` (`bindingVersion`).

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Kotlin guide](https://docs.zlink.systems/kotlin/guide/server/), [API reference](https://docs.zlink.systems/kotlin/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the packages pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host JDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario field,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario field,dungeon,reconnect,update
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only together with `--local`/`-Local` and an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README. Run the build and e2e on both Windows and Linux. The runner validates the structure before it starts nodes, and each run uses its own Redis key prefix, Compose project, and OS-assigned ports; keep that isolation when changing the runner.

## Project layout and extension points

### Session gateway

`server/session/src/main/kotlin/MyGame/session/Program.kt` registers the STREAM node and the RouteMesh client, and `GameSession` (in the same file) handles incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession` answers `SessionInfoRequest` from the binding, finds or creates the Player Actor and binds it when it receives `JoinWorldRequest`, and relays every other packet to the bound Actor. Add authentication before binding and retain the typed packet relay so Framework can identify the connection for replies to that player.

### Fields and players

An Actor is a game state object addressed by ID; `Player` in `server/shared/src/main/kotlin/MyGame/Player.kt` retains the player's current `position` and `partyId`. `FieldPositionDomain` in `server/field/src/main/kotlin/MyGame/field/FieldPositionDomain.kt` owns authoritative position, sequence, movement budget, and pending arrivals. The fixed `Field` User Spot owns only a visibility projection in `FieldState`. The dynamic `Party` User Spot owns party membership and run phases; `DungeonRun` owns its Party Spot ID, members, phase, and stage. `Entry` and `EnterWorld` in `server/shared/src/main/kotlin/MyGame/admission/Entry.kt` receive new Actors and send them to the field that covers their coordinate.

`server/field/src/main/kotlin/MyGame/field/Program.kt` registers the handlers, Entry Spot, fixed `Field` factory, dynamic `PartySpot` factory, `Player` factory, and `border` channel server. `Actions` in `server/field/src/main/kotlin/MyGame/field/Handlers.kt` routes player requests to the Field; party commands go to the Party Spot through `requestToSpot`. `BorderTick` publishes border information. `Hosting.kt` creates the fixed Fields listed in `fieldIds` when a node starts. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Border subscriptions

`server/field/src/main/kotlin/MyGame/border/BorderRoutes.kt` registers `BorderHandler` for each adjacent Field topic with the public 0.30.2 `addSubscribe` API. New Field pairs need no additional handler class.

### Dungeons

`server/instance/src/main/kotlin/MyGame/instance/Program.kt` registers the Entry Spot, the `Dungeon` factory, and the `Player` factory. `Dungeon` is a dynamic User Spot whose `DungeonRun` stores its owner Party Spot ID, Run ID, members, phase, and stage. It admits only members with the prepared Run ID, reports arrivals and completion to the owning Party Spot, and pushes `DungeonView`. `Prepare` and `DungeonActions` handle preparation and player requests. Add encounters, rewards, and Dungeon lifecycle behavior in this module.

### Shared contracts and hosting

`server/shared/src/main/kotlin/MyGame/Contracts.kt` defines names, movement and party constants, run phases, rejection reasons, and messages shared by processes. `Hosting.kt` defines `NodeConfig`, Redis stores, hosting and diagnostics configuration, and `runNode`; `Control.kt` defines control and readiness strings. `Rules` owns field, speed, cooldown, and stage limits and the field helpers; `Player.kt` defines `Player`, its factory, and its relocation adapter. Keep hosting and the control protocol here instead of duplicating them per role.

### E2E client

`e2e/src/main/kotlin/MyGame/e2e/Client.kt` connects through the public stream connector, `Scenarios.kt` checks results observable by the client, and `DomainChecks.kt` checks movement, PartyRun, DungeonRun, and JSON boundaries without starting nodes. `Nodes.kt` manages processes and the Compose project, `Structure.kt` validates composition, and `Program.kt` selects checks and scenarios. `DockerRuntime.kt` runs nodes as containers in Docker mode, and `Cleanup.kt` collects cleanup failures. Verify gameplay through received client results.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-world`). Channels distinguish messages by purpose and name, and the `border` channel (`Names.BORDER`) carries typed publications between Fields. [Channel messaging](https://docs.zlink.systems/kotlin/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles Actor entry, `Field` is a fixed User Spot, `Party` is a dynamic User Spot addressed by the Party ID, and `Dungeon` is a dynamic User Spot addressed by the Run ID. The [Spot guide](https://docs.zlink.systems/kotlin/guide/server/21-spot/) describes registration and calls for these objects. `Player` retains its current position and party association when its host Spot changes; the [Actor guide](https://docs.zlink.systems/kotlin/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/kotlin/guide/server/23-stream/), which processes packets on client connections, `GameSession` obtains the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/kotlin/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation (`ZLinkRedisLocationStore` in `Hosting.kt`). Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/kotlin/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Player.move` requests deferred admission to the next Field with `joinSpot(...).defer()`, `Field.onActorJoinSuspending` decides, and `onJoinCompletedSuspending` observes completion. The Field sends an entry command to the Party Spot; that Spot prepares the Dungeon before pushing `JoinDungeon`, and `Dungeon.onActorJoinSuspending` accepts only members with the matching Run ID. [Actor membership](https://docs.zlink.systems/kotlin/guide/server/35-actor-membership/) explains admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `runNode` in `Hosting.kt` requests host relocation when it reads `drain` and reports `SAFE_TO_SHUTDOWN` after observing it. `FieldTransfer` stores visibility and movement snapshots with remaining milliseconds; `PartyTransfer`, `DungeonTransfer`, and `PlayerTransfer` preserve their owned state. The [Relocation guide](https://docs.zlink.systems/kotlin/guide/server/37-relocation/) explains the procedure.

### Border notifications and timers

A timer runs a handler periodically; `BorderTick` uses one to publish information about players near a border. In this pub/sub exchange, `BorderHandler` receives subscribed information and calls `Field.showBorder`; `Actions.deliver` forwards the notification through the receiving Actor's bound Session. [Timers and workers](https://docs.zlink.systems/kotlin/guide/server/36-timer-worker/) explains registration and execution of periodic work.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinWorldRequest`. `GameSession` calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session with `bindOrGet`, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. `EnterWorld.join` checks the identity and calls `Player.move`, which requests `joinSpot(...).defer()` for the Field that covers the coordinate.
3. When the join completes, `Player.onJoinCompletedSuspending` sends `WorldState` through the bound Session. The Field's `Actions` handler sends movement requests to `FieldPositionDomain`, which validates and calculates the authoritative result.
4. When movement reaches another Field, the Field passes the accepted position, sequence, and remaining cooldown to `Player.move`. Field membership changes while Actor and Session identities remain the same.
5. Every `Rules.TICK_MILLIS`, `BorderTick` publishes border presence on the `border` channel. `BorderHandler` calls `Field.showBorder`, which sends `BorderVisible` to nearby observers with `sendToActor`; `Actions.deliver` forwards it through the observer's bound Session.
6. `CreatePartyRequest` creates a server-issued Party ID and dynamic Party Spot. `JoinPartyRequest` and `EnterDungeonRequest` reach that Spot through `requestToSpot`; it owns membership, assigns a Run ID, prepares `dungeon-<runId>`, and pushes `JoinDungeon`. `Dungeon` checks the Run ID, reports arrivals and completion to the owning Party Spot, and handles stage requests. Town-only party requests inside Dungeon return `run-in-progress`.
7. A reconnecting client sends `JoinWorldRequest` on a new Session. The same Actor is bound, and its current Spot resends state. A Field departure removes visibility only; it does not change party membership. A Session disconnect removes visibility while retaining movement and party state.
8. To replace a node, `drain` in `runNode` requests relocation. Framework uses `PlayerTransfer`, `FieldTransfer`, `PartyTransfer`, and `DungeonTransfer` to preserve each owner's state.

## Extending the template domain

Extend this template's domain to build a MORPG server. Run the README build and e2e commands before adding features.

1. Extend movement decisions in `FieldPositionDomain.kt`, party transitions in `PartyRun.kt`, and Dungeon rules in `DungeonRun.kt`. Keep one owner for each state change.
2. Add shared contracts in `server/shared/src/main/kotlin/MyGame/Contracts.kt`. The server validates inputs and calculates results. Rejected inputs must preserve values, sequences, and cooldowns.
3. Add only client packets to the Session allowlist in `server/session/src/main/kotlin/MyGame/session/Program.kt`. Keep internal messages and result packets outside that list.
4. Add boundary checks in `e2e/src/main/kotlin/MyGame/e2e/DomainChecks.kt` and received-result scenarios in `Scenarios.kt`. Update the README scenario table and `run.scenarios` in `zlink.structure.json` together. Wait only for the server's `RetryAfterMs` after a time-limited rejection.
5. Update the owning relocation snapshot and boundary checks when adding state. Store remaining milliseconds rather than process timestamps.

## Development considerations

### State and message contracts

Use the public packages from Maven Central at the versions pinned in `build.gradle.kts`, and keep project dependencies between this project's own Gradle modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters (the `*Transfer` classes) to application state, keeping management of Framework execution state separate from game state. Framework owns routing, leases, membership, completion, queues, and execution gates; do not duplicate those decisions in application state.

Manage each game fact in one owner. `FieldPositionDomain` decides position, sequence, speed budget, and arrivals; `FieldState.visiblePlayers` is an observation copy. `Player` retains current position and Party Spot ID. `PartyRun` owns party membership and phases, while `DungeonRun` owns the prepared run and stage. Store player IDs rather than Actor references in Spots. Replies inside an Actor handler go through its bound Session; other pushes address the Actor by player ID with the public Actor client, and the receiving handler forwards them through its bound Session. Session binding owns Actor identity, so game messages do not duplicate it.

### Asynchronous execution and lifecycle

Await public calls with `await()` as the template does, and avoid blocking calls such as `.join()`, `.get()`, `runBlocking`, or `Thread.sleep` in handlers and callbacks. `Field`, `PartySpot`, and `Dungeon` are registered with `SPOT_WIDE`, which gives each Spot's handlers, member Actor handlers, timers, and lifecycle callbacks a shared execution gate. Do not protect the same state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/kotlin/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Fixed `Field` Spots are created at node startup; `Party` and `Dungeon` Spots are dynamic and use `getOrCreate`. [Activation and lifetime](https://docs.zlink.systems/kotlin/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback (`onJoinCompletedSuspending`) for membership changes, and relocation with runtime observations (`drain`, then `SAFE_TO_SHUTDOWN`) for host shutdown, so request acceptance is not mistaken for completion. `Control.kt` derives the safe-shutdown wait (`SAFE_DEADLINE`) from the Message Follow duration (`MESSAGE_FOLLOW`) and the host deadline (`HOST_DEADLINE`); change them there only.

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
