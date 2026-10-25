# MyGame development guide

## Project overview

This Kotlin project (Kotlin 2.3.21 on a Java 25 toolchain, built with Gradle) is an MMO server example in which players move across Zone boundaries. It runs a Session gateway for connections, Zone hosts A and B, an Instance host for Dungeons, and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework packages to `0.30.2` (`frameworkVersion`) and the ZLink binding to `1.20.0` (`bindingVersion`).

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and the scenario assertions. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Kotlin guide](https://docs.zlink.systems/kotlin/guide/server/), [API reference](https://docs.zlink.systems/kotlin/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the packages pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host JDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario cross,border,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario cross,border,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only together with `--local`/`-Local` and an existing local build. Keep canonical scenario order and assertions; compare first failures with the README. Run the build and e2e on both Windows and Linux. The runner validates the structure before it starts nodes, and each run uses its own Redis key prefix, Compose project, and OS-assigned ports; keep that isolation when changing the runner.

## Project layout and extension points

### Session gateway

`server/session/src/main/kotlin/MyGame/session/Program.kt` registers the STREAM node and the RouteMesh client, and `GameSession` (in the same file) handles incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession` answers `SessionInfoRequest` from the binding, finds or creates the Player Actor and binds it when it receives `JoinWorldRequest`, and relays every other packet to the bound Actor. Add authentication before binding and retain the typed packet relay so Framework can identify the connection for replies to that player.

### Zones and players

An Actor is a game state object addressed by ID; `Player` in `server/zone/src/main/kotlin/MyGame/zone/Player.kt` owns no duplicate position state. A Spot accepts Actors and executes shared game rules; `ZonePositionDomain` in `server/zone/src/main/kotlin/MyGame/zone/ZonePositionDomain.kt` owns position, sequence, movement budget, arrivals, and visibility state for its Zone.

`server/zone/src/main/kotlin/MyGame/zone/Program.kt` registers the handlers, the Entry Spot, the `Zone` factory (`stableTypeLimit(config.zoneCapacity)`, `SPOT_WIDE`), the `Player` factory, and the `border` channel server. `Entry` and `EnterWorld` in `Player.kt` handle player entry into the world. Add handlers that combine player rules with shared Zone rules to the `Actions` class in `Handlers.kt`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Border subscriptions

`server/zone/src/main/kotlin/MyGame/border/BorderRoutes.kt` declares one subscription handler class per adjacent Zone pair, because annotation topics are compile-time constants (`BorderTopics`). `Zone.configure()` calls `BorderRoutes.register` for each neighbor. A new adjacent pair needs a new handler class and a `when` branch there.

### Dungeons

`server/instance/src/main/kotlin/MyGame/instance/Program.kt` registers the `Dungeon` factory (`recreateOnRelocation()`) and defines `Dungeon` and the `DungeonActions.open` request handler, so Dungeons are created on demand and return their information. `Dungeon` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract. Add party admission rules, encounters, rewards, and Dungeon lifecycle behavior in this module.

### Shared contracts and hosting

`server/shared/src/main/kotlin/MyGame/Contracts.kt` defines names, game constants, and the messages shared by processes. `Hosting.kt` defines `NodeConfig` (process settings), the Redis stores, `configure` for hosting and diagnostics, and `runNode`, which starts a node and handles the stdin control commands; `Control.kt` defines the control and readiness strings. `Transfer.kt` defines `ensureActive` for relocation adapters. Keep hosting and the control protocol here instead of duplicating them per role. Add shared contracts in `Contracts.kt`; keep position and movement decisions in `ZonePositionDomain` so separate processes do not independently modify the same fact.

### E2E client

`e2e/src/main/kotlin/MyGame/e2e/Client.kt` connects through the public stream connector, and `Scenarios.kt` checks results observable by the client. `Nodes.kt` manages the processes and the Compose project for each run, `Structure.kt` validates the composition, and `Program.kt` selects scenarios and sets the exit code (a failed scenario or a failed cleanup exits with 1). `DockerRuntime.kt` runs nodes as containers in Docker mode, and `Cleanup.kt` collects cleanup failures. Verify new gameplay through received client results. The project includes no separate unit-test module.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-world`). Channels distinguish messages by purpose and name, and the `border` channel carries typed publications between Zones. [Channel messaging](https://docs.zlink.systems/kotlin/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles Actor entry, `Zone` represents a geographical Zone, and `Dungeon` represents a dynamically created Dungeon. The [Spot guide](https://docs.zlink.systems/kotlin/guide/server/21-spot/) describes registration and calls for these objects. `Player` retains movement state when its host Spot changes; the [Actor guide](https://docs.zlink.systems/kotlin/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/kotlin/guide/server/23-stream/), which processes packets on client connections, `GameSession` obtains the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/kotlin/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation (`ZLinkRedisLocationStore` in `Hosting.kt`). Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/kotlin/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. The Zone handler requests deferred admission after `ZonePositionDomain` accepts movement; `onJoinCompletedSuspending` observes completion. [Actor membership](https://docs.zlink.systems/kotlin/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `runNode` in `Hosting.kt` requests host relocation when it reads `drain` and reports `SAFE_TO_SHUTDOWN` after observing it; `PlayerTransfer` and `ZoneTransfer` hold the adapters. The [Relocation guide](https://docs.zlink.systems/kotlin/guide/server/37-relocation/) explains the procedure.

### Border notifications and timers

Zone publishes appearance, movement, and departure when each fact changes. In this pub/sub exchange, the `Route*` subscription handlers in `BorderRoutes.kt` receive nearby events and `Actions` delivers each typed event through the receiving Actor's bound Session without duplicating a local fact.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinWorldRequest`. `GameSession` calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session with `bindOrGet`, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. `EnterWorld.join` checks identity and asks `ZonePositionDomain` to resolve the requested initial position before requesting `joinSpot(...).defer()`.
3. When the join completes, `Player.onJoinCompletedSuspending` sends `WorldState` through the bound Session. Later packets from the same client are handled by the `Actions` handlers of the Zone that holds the Actor (such as `move`), which send results through the same Session.
4. `Actions.move` asks `ZonePositionDomain` to validate a delta and sequence. When the computed position belongs to another Zone, the handler requests the target admission. The client connection does not change when that Zone is on another node.
5. To replace a node, `drain` in `runNode` requests relocation. The Zone domain snapshot stores remaining budget milliseconds; the stateless Player Actor snapshot is empty.

## Extend this template's domain

Extend this template's domain to build a seamless MMO server.

- Domain state and decisions: `server/zone/src/main/kotlin/MyGame/zone/ZonePositionDomain.kt`.
- Shared typed contracts and limits: `server/shared/src/main/kotlin/MyGame/Contracts.kt`.
- Client packet allow-list: `GameSession` in `server/session/src/main/kotlin/MyGame/session/Program.kt`.
- Domain boundary checks and scenarios: `e2e/src/main/kotlin/MyGame/e2e/DomainChecks.kt`, `Scenarios.kt`, and `Client.kt`; keep `run.scenarios` and `run.commands` in `zlink.structure.json` aligned.

Add a typed contract, put its decision in the domain owner, permit only client packets in Session, add a domain boundary check, and observe acceptance, rejection, and unchanged state through the public client. Relocation snapshots store remaining budget milliseconds. Run `./run-e2e.sh --check-domain`, `./run-e2e.sh --build-only`, and `./run-e2e.sh`.

### Validate the current domain

Before changing a rule, run the domain checks and related e2e scenario with the [Development commands](#development-commands). Compare failures with the first failure in the logs (see [Debugging](#debugging)) and the [README](README.md).

### Existing extension points

Paths are relative to this directory. Each row identifies the owner and the related registration or observable scenario.

| Area | Owner | Rule or code to extend | Related registration or scenario |
| --- | --- | --- | --- |
| Typed messages | `server/shared/src/main/kotlin/MyGame/Contracts.kt` | Shared request, response, event, and rejection contracts; `SessionInfo` carries binding identity separately from `WorldState` | Keep JSON property names and event string values aligned across languages |
| Movement and visibility domain | `server/zone/src/main/kotlin/MyGame/zone/ZonePositionDomain.kt` | `ZonePositionDomain` owns position, sequence, budget, arrivals, connected players, and boundary decisions | Add the domain check and observe results in `cross` and `border` |
| Actor and Zone lifecycle | `server/zone/src/main/kotlin/MyGame/zone/Player.kt`; `Zone.kt` | Player carries no duplicate position state; Zone admission and leave callbacks update the domain and publish typed events | Preserve `addActorFactory` and `addSpotFactory` registrations in `Program.kt` |
| Entry and first admission | `server/zone/src/main/kotlin/MyGame/zone/Player.kt` (`Entry`, `EnterWorld`), `Actions.reconnect` in `server/zone/src/main/kotlin/MyGame/zone/Handlers.kt` | `EnterWorld.join`, the first-entry path, and the identity check and state resend of `Actions.reconnect` | The `addEntrySpot` registration and its `entry` declaration; the `cross` scenario for first entry and the `reconnect` scenario that compares state after reconnection |
| Session binding | `server/session/src/main/kotlin/MyGame/session/Program.kt` (`GameSession`) | The first-packet branch: find or create the Actor, `bindOrGet`, and relay the original typed packet | The same first-packet type as `EnterWorld.join`; transfer it into the starter's `PlayerSession.kt` |
| Host relocation | `ZoneTransfer` in `server/zone/src/main/kotlin/MyGame/zone/Zone.kt`, `Transfer.kt`, and host controls | Zone snapshots store remaining budget milliseconds; Player transfer state is empty | Keep `preserveStateWith` registrations and the `update` scenario aligned |
| Dynamic dungeon | `server/instance/src/main/kotlin/MyGame/instance/Program.kt`, `Actions.dungeon` in `server/zone/src/main/kotlin/MyGame/zone/Handlers.kt` | The dynamic User Spot creation pattern (`recreateOnRelocation()`), `DungeonActions.open`, and the Dungeon information | A new `server/instance/` Gradle module with `instance.json`, its start-up, and role; the dynamic `dungeon` Spot and the `GetOrCreate` edge in `zlink.structure.json` |
| Channel pub/sub and seamless movement | `Zone` visibility publication, `BorderRoutes.kt`, and typed delivery handlers in `Handlers.kt` | Publish appearance, movement, and departure on state changes | Keep the `border` channel registration and the `border` scenario aligned |

`vacate-zone` is the control that e2e uses to verify replacement placement (`Control.VACATE_ZONE`, handled in `runNode`).

### Transfer order and rules

Extend one rule at a time and run its domain check, build, structure check, and related e2e scenario. Add scenario IDs to `Names.SCENARIOS`, `Scenarios.kt`, `run.scenarios`, and the command examples in `zlink.structure.json` together. Judge results by replies and push events actually received; do not use fixed waits or longer deadlines.

Start with the message contracts. Define the typed classes in `Contracts.kt` first, then move each handler that fails to compile to the new messages. When you change the first-entry packet, update the first-packet branch in `GameSession`, `EnterWorld.join`, and `Actions.reconnect` to receive the same type.

Keep position, sequence, movement budget, and visibility in `ZonePositionDomain`. When those fields change, update its snapshot and `capture` and `restore` together. A missing field disappears after relocation, and the `update` scenario detects the loss.

Add a channel, its subscription, and its declaration in the same change: the subscription handlers in `BorderRoutes.kt` (registered from `Zone.configure()`), the channel server registration in `Program.kt`, and the `channels` declaration in `zlink.structure.json`. The structure check requires every Zone node to declare the `border` channel.

To name roles differently from this sample, change the type names, `Names`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use the public packages from Maven Central at the versions pinned in `build.gradle.kts`, and keep project dependencies between this project's own Gradle modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters (the `*Transfer` classes) to application state, keeping management of Framework execution state separate from game state. Framework owns routing, leases, membership, completion, queues, and execution gates; do not duplicate those decisions in application state.

Keep position, sequence, budget, and visibility in `ZonePositionDomain`; Spots retain player IDs and forward typed observations without storing Actor references. Replies inside an Actor handler go through that Actor's bound Session; pushes address the Actor by player ID with the public Actor client, and the receiving Actor handler forwards them through its bound Session. Host relocation does not repeat `onJoinedActor` on the target. Session binding owns Actor identity, so game messages do not duplicate it (`SessionInfo` reads the current binding). Session forwards only packets in its client allow-list; the domain owner makes movement decisions.

### Asynchronous execution and lifecycle

Await public calls with `await()` as the template does, and avoid blocking calls such as `.join()`, `.get()`, `runBlocking`, or `Thread.sleep` in handlers and callbacks. `Zone` is registered with `SPOT_WIDE`, which gives Spot handlers, member Actor handlers, timers, and lifecycle callbacks of the same Zone a shared execution gate. Do not protect the same state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/kotlin/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Apply the User Spot contract to `Dungeon` even though its process is named Instance; [Activation and lifetime](https://docs.zlink.systems/kotlin/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback (`onJoinCompletedSuspending`) for membership changes, and relocation with runtime observations (`drain`, then `SAFE_TO_SHUTDOWN`) for host shutdown, so request acceptance is not mistaken for completion. `Control.kt` derives the safe-shutdown wait (`SAFE_DEADLINE`) from the Message Follow duration (`MESSAGE_FOLLOW`) and the host deadline (`HOST_DEADLINE`); change them there only.

### Reconnection and failure handling

Reconnect with the same player ID and bind the new Session to the same Actor to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/kotlin/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, `.gradle/`, and `.kotlin/` and license files (`zlink.license`) from commits. The project includes no formatter configuration; format Kotlin sources with ktfmt `--kotlinlang-style` from this directory only.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. The runner prints `Logs: <path>` at start. `.logs/run-<id>/` contains the effective settings that `Nodes.kt` writes at node startup (`session.json`, `zoneA.json`, `zoneB.json`, `instance.json`) and one directory per node start, `<node>-<uuid>/`, with `<node>-stdout.log` (readiness and control lines) and `<node>-stderr.log` (Framework logs and message flow). A replaced Zone A starts from the same `zoneA.json` in a new `zoneA-<uuid>/` directory.

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

Put the language and the failing behavior in the title (for example, `framework(kotlin): Spot startup fails with empty SpotId`). Record the filed issue number in the README, in a Known issues section that you add if it has none, so the project keeps the place to re-verify when a fixed package is released.
