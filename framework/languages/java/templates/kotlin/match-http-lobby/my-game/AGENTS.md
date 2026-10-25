# MyGame development guide

## Project overview

This Kotlin project (Kotlin 2.3.21 on a Java 25 toolchain, built with Gradle) is a duel server that pairs two players through HTTP and runs the duel over ZLink STREAM. It runs an HTTP lobby, a Session gateway for connections, two Match hosts (`matchA` and `matchB`), and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework packages to `0.30.2` (`frameworkVersion`) and the ZLink binding to `1.20.0` (`bindingVersion`).

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Kotlin guide](https://docs.zlink.systems/kotlin/guide/server/), [API reference](https://docs.zlink.systems/kotlin/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the packages pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker without a host JDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,wall,input,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,wall,input,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only together with `--local`/`-Local` and an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README. Run the build and e2e on both Windows and Linux. The runner validates the structure before it starts nodes, and each run uses its own Redis key prefix, Compose project, and OS-assigned ports; keep that isolation when changing the runner.

## Project layout and extension points

### HTTP lobby

`server/lobby/src/main/kotlin/MyGame/lobby/Program.kt` starts the lobby as a RouteMesh client and creates the `HttpLobby` bean. `server/lobby/src/main/kotlin/MyGame/lobby/infrastructure/HttpLobby.kt` implements the HTTP endpoint (`POST` on `Names.MATCH_PATH`) with `RequestHandler`; `ZLinkRooms` creates the Room through the public Spot manager, sends the matched member list in `PrepareRoom`, and waits for the matching `RoomPrepared` reply before returning `MatchResponse`. `server/lobby/src/main/kotlin/MyGame/lobby/application/Matchmaker.kt` owns mode pairing, default mode `duel`, and a Room ID formed from `Names.ROOM_PREFIX` plus a hyphen-free UUID. The lobby defers the response to the first request of a mode until another player asks for the same mode, and a client that disconnects while waiting is removed from the queue. Set the `sessionEndpoint` setting to the Session node's `READY` endpoint before starting the lobby; `httpEndpoint` is the lobby's own listener.

### Session gateway

`server/session/src/main/kotlin/MyGame/session/Program.kt` registers the STREAM node and the RouteMesh client, and `GameSession` (in the same file) handles incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession` answers `SessionInfoRequest` from the binding, finds or creates the Player Actor and binds it when it receives `JoinRoomRequest`, and relays only the client packet allowlist (`JoinRoomRequest`, `RoomViewRequest`, `MoveRequest`, `ShootRequest`, and `SessionInfoRequest`). It rejects internal packets with `ActionRejected("packet-not-allowed", 0)`. Add authentication before binding and retain typed packet relay so Framework can identify the connection for replies to that player.

### Match nodes

An Actor is a game state object addressed by ID; `Player` in `server/match/src/main/kotlin/MyGame/match/infrastructure/zlink/Player.kt` keeps no application state. A Spot is a state object that accepts Actors and executes shared game rules; `Room` in `server/match/src/main/kotlin/MyGame/match/infrastructure/zlink/Room.kt` is a dynamic User Spot. `RoomState` owns positions, HP, accepted action sequences, movement budget, fire cooldown, phase, winner, and action decisions. `RoomRoster` owns the matched member list and Room admission.

`server/match/src/main/kotlin/MyGame/match/Program.kt` registers the handlers, the Entry Spot, the `Room` factory (`stableTypeLimit(config.roomCapacity)`, default `SPOT_WIDE`), and the `Player` factory. `Entry` and `Join` in `Player.kt` handle player entry, and the `Actions` class in `server/match/src/main/kotlin/MyGame/match/infrastructure/zlink/Actions.kt` connects player requests and pushes to the Room. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Shared contracts and hosting

`server/shared/src/main/kotlin/MyGame/Contracts.kt` defines names, the `2^53−1` integer limit, action rules, and typed messages shared by processes, including `PrepareRoom`, `RoomPrepared`, and `ActionRejected(Reason, RetryAfterMs)`. `Hosting.kt` defines `NodeConfig`, Redis stores, `configure` for hosting and diagnostics, and `runNode`; `Control.kt` defines control and readiness strings. `Transfer.kt` defines `ensureActive` for relocation adapters. Keep game state in `RoomState`, roster membership in `RoomRoster`, and the control protocol in their existing owners.

### E2E client

`e2e/src/main/kotlin/MyGame/e2e/Client.kt` connects through the public stream connector, and `Scenarios.kt` checks results observable by the client. `RoomStateTest.kt` owns game boundary checks and `ContractSerializationTest.kt` verifies the eight wire samples during the build. `LobbyDomainChecks.kt` runs the 19 HTTP checks before scenarios. `Nodes.kt` manages processes and the Compose project for each run, `Structure.kt` validates the composition, and `Program.kt` selects scenarios and reports counts. The six scenarios are `lobby`, `hit`, `wall`, `input`, `reconnect`, and `update`. The existing JUnit tests are in `server/lobby/src/test/kotlin/MyGame/lobby/application/MatchmakerTest.kt` and `server/match/src/test/kotlin/MyGame/match/domain/RoomStateTest.kt`.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-arena`). The Match nodes serve objects in the mesh, while the lobby and Session nodes join it as clients. Channels distinguish messages by purpose and name; this project declares no application channels (`channels` is empty in `zlink.structure.json`). [Channel messaging](https://docs.zlink.systems/kotlin/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles Actor entry and `Room` is a User Spot that the lobby creates on demand for each pair of players. The [Spot guide](https://docs.zlink.systems/kotlin/guide/server/21-spot/) describes registration and calls for these objects. `Player` is addressed by player ID and keeps no application state; the [Actor guide](https://docs.zlink.systems/kotlin/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/kotlin/guide/server/23-stream/), which processes packets on client connections, `GameSession` obtains the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/kotlin/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation (`ZLinkRedisLocationStore` in `Hosting.kt`). Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/kotlin/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Join.join` requests deferred admission to the room named in the first packet with `joinSpot(...).defer()`. `Room.onActorJoinSuspending` checks the identity, asks `RoomRoster.canAdmit`, and then calls `RoomState.admit`; `Player.onJoinCompletedSuspending` reports a rejection to the client. [Actor membership](https://docs.zlink.systems/kotlin/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `runNode` in `Hosting.kt` requests host relocation when it reads `drain` and reports `SAFE_TO_SHUTDOWN` after observing it; `RoomTransfer` stores `{game, roster}` and timer durations as remaining milliseconds. It does not store process clock readings. `PlayerTransfer` stores no application state, and the lobby stays outside host relocation. The [Relocation guide](https://docs.zlink.systems/kotlin/guide/server/37-relocation/) explains the procedure.

### Creating Spots from the lobby

The lobby is a RouteMesh client that creates the Room through the public Spot manager and sends `PrepareRoom(matchId, members)` through the public route client. It returns the HTTP assignment only after the Room replies with `RoomPrepared` for the same match ID. The [Spot guide](https://docs.zlink.systems/kotlin/guide/server/21-spot/) describes Spot creation.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client sends `POST` `Names.MATCH_PATH` with a `MatchRequest` to the lobby. Omitted `mode` selects `duel`; `Matchmaker.match` defers the response until another player requests the same mode.
2. When two players are paired, `Matchmaker` creates a UUID Room ID and passes both member IDs to `ZLinkRooms.prepare`. `ZLinkRooms` creates the Room, sends `PrepareRoom`, and waits for `RoomPrepared`; only then do both HTTP requests complete with the same `MatchResponse`.
3. Each client connects to the Session node over STREAM and sends the first packet, `JoinRoomRequest`. `GameSession` calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session with `bindOrGet`, and relays the original packet.
4. A new Actor starts in the Entry Spot `Entry`. `Join.join` checks the identity and requests `joinSpot(request.roomId, request).defer()`. `Room.onActorJoinSuspending` asks `RoomRoster.canAdmit` before `RoomState.admit`; the server selects the spawn. `Room.onJoinedActorSuspending` broadcasts `RoomView`.
5. `move` and `shoot` requests reach `Room`, which applies `RoomState` rules and computes outcomes on the server. Rejections use `ActionRejected(reason, retryAfterMs)` and leave game values, sequence, and timers unchanged. Accepted actions update the sequence and broadcast `RoomView`; the winner is announced with `MatchEnded`.
6. A reconnecting client sends `JoinRoomRequest` on a new Session. The same Actor is bound, and `Actions.join` resends the room view after checking the identity.
7. To replace a match node, `drain` in `runNode` requests relocation. Framework transfers the application snapshot from `RoomTransfer`, which contains game state, roster, and remaining timer milliseconds.

## Extend this template's domain

Extend this template's domain to build a two-dimensional action server. Keep each decision in the object that owns the state it changes.

- Add game state and its decision rules in `server/match/src/main/kotlin/MyGame/match/domain/RoomState.kt`. Add matched membership and admission rules in `RoomRoster.kt` in the same domain package.
- Add typed messages shared by processes in `server/shared/src/main/kotlin/MyGame/Contracts.kt`.
- Update the client packet allowlist in `server/session/src/main/kotlin/MyGame/session/Program.kt` when clients need to send a new packet. Keep internal messages such as `PrepareRoom` and `RoomPrepared` outside the allowlist.
- Add domain boundary tests to the existing `RoomStateTest.kt` or `MatchmakerTest.kt` location. Add client-observable behavior to `Scenario` in `Contracts.kt` and `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt`.
- Update README scenario observations and `zlink.structure.json` `run.scenarios` and selection commands with each new scenario. When state changes, update `RoomTransferSnapshot` and test that relocation preserves remaining timer milliseconds.

## Development considerations

### State and message contracts

Use the public packages from Maven Central at the versions pinned in `build.gradle.kts`, and keep project dependencies between this project's own Gradle modules. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters (the `*Transfer` classes) to application state, keeping management of Framework execution state separate from game state. Framework owns routing, leases, membership, completion, queues, and execution gates; do not duplicate those decisions in application state.

Manage each game fact, such as a player's HP, in the object responsible for changing it (`Room`). The lobby keeps only the waiting requests, which are not game state. Replies inside an Actor handler go through that Actor's bound Session; other pushes address the Actor by player ID with the public Actor client, and the receiving Actor handler forwards them through its bound Session. A Spot must not retain Actor references for later delivery because host relocation does not repeat `onJoinedActor` on the target. Session binding owns Actor identity, so game messages do not duplicate it (`SessionInfo` reads the current binding). Sessions forward packets after binding; the Actor or Spot responsible for the state makes gameplay decisions.

### Asynchronous execution and lifecycle

Await public calls with `await()` as the template does, and avoid blocking calls such as `.join()`, `.get()`, `runBlocking`, or `Thread.sleep` in handlers and callbacks. `Room` uses the default `SPOT_WIDE` execution mode, which gives Spot handlers, member Actor handlers, and lifecycle callbacks of the same Room a shared execution gate. Do not protect the same state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/kotlin/guide/server/32-execution-model/) explains these execution boundaries. `Matchmaker` guards its waiting maps with its own `synchronized` blocks because they are HTTP-layer state outside any Spot and execution gate; do not copy that pattern for Spot state.

Keep type registration and per-instance callbacks in their defined lifecycle phases. `Room` is a User Spot that the lobby creates on demand with `getOrCreate`; [Activation and lifetime](https://docs.zlink.systems/kotlin/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback (`onJoinCompletedSuspending`) for membership changes, and relocation with runtime observations (`drain`, then `SAFE_TO_SHUTDOWN`) for host shutdown, so request acceptance is not mistaken for completion. `Control.kt` derives the safe-shutdown wait (`SAFE_DEADLINE`) from the Message Follow duration (`MESSAGE_FOLLOW`) and the host deadline (`HOST_DEADLINE`); change them there only.

### Reconnection and failure handling

Reconnect with the same player ID and bind the new Session to the same Actor to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/kotlin/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, `.gradle/`, and `.kotlin/` and license files (`zlink.license`) from commits. The project includes no formatter configuration; format Kotlin sources with ktfmt `--kotlinlang-style` from this directory only.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. The runner prints `Logs: <path>` at start. `.logs/run-<id>/` contains the effective settings that `Nodes.kt` writes at node startup (`lobby.json`, `session.json`, `matchA.json`, `matchB.json`) and one directory per node start, `<node>-<uuid>/`, with `<node>-stdout.log` (readiness and control lines) and `<node>-stderr.log` (Framework logs and message flow). A replaced node reuses its settings file and starts a new directory.

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
