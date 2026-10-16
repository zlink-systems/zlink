# MyGame development guide

## Project overview

This Java 25 project is a match server example in which an HTTP lobby pairs two players and their duel runs over ZLink STREAM. It runs an HTTP lobby, a Session gateway for connections, Match hosts A and B with dynamic Room Spots, and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework to `0.30.1` and the binding to `1.19.0`.

The project layout below describes where to add gameplay, and "Developing from this sample" describes how to carry its features into your own project. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Java guide](https://docs.zlink.systems/java/guide/server/), [API reference](https://docs.zlink.systems/java/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the version pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker, so they need Docker with Compose v2 and no host JDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario lobby,hit,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario lobby,hit,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/session/src/main/java/MyGame/session/Program.java` registers STREAM and the mesh listener, and `server/session/src/main/java/MyGame/session/GameSession.java` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession.onDispatch` requires `JoinRoomRequest` as the first packet, gets or creates the Player Actor by `playerId`, binds it, and relays the original packet. Add authentication before binding and retain the typed relay so Framework can identify the connection for replies to that player.

### HTTP lobby

`server/lobby/src/main/java/MyGame/lobby/Program.java` joins the mesh as an object client only and creates the `HttpLobby` bean. `server/lobby/src/main/java/MyGame/lobby/infrastructure/HttpLobby.java` is the Netty HTTP endpoint for `POST /match` (`Names.MATCH_PATH`); it decodes `MatchRequest` and holds the response until a partner arrives. `server/lobby/src/main/java/MyGame/lobby/application/Matchmaker.java` pairs requests of the same mode, assigns `room-<sequence>`, and completes both responses with `MatchResponse` after `RoomProvisioner.prepare` returns. `server/lobby/src/main/java/MyGame/lobby/infrastructure/ZLinkRooms.java` implements that port by creating the Room with `getOrCreate` and returns the Session endpoint from `server/config/lobby.json`. Keep matching decisions in `Matchmaker` and ZLink calls in `ZLinkRooms`.

### Match rooms

`server/match/src/main/java/MyGame/match/domain/RoomState.java` (`RoomState`) keeps positions, health, shot range, wall intersection, and the winner. `server/match/src/main/java/MyGame/match/infrastructure/zlink/Room.java` is the Room User Spot that applies it, `server/match/src/main/java/MyGame/match/infrastructure/zlink/Entry.java` and `server/match/src/main/java/MyGame/match/infrastructure/zlink/Join.java` accept a player's first entry and defer the join to the prepared Room, and `server/match/src/main/java/MyGame/match/infrastructure/zlink/Player.java` has no application state. `server/match/src/main/java/MyGame/match/infrastructure/zlink/Actions.java` holds the handlers that connect requests to `Room`. `server/match/src/main/java/MyGame/match/Program.java` registers the Entry Spot, the Room factory (`roomCapacity` from `server/config/match-a.json`), and the Player factory. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Shared contracts and hosting

`server/shared/src/main/java/MyGame/` holds the messages and game constants shared by processes (`MatchRequest`, `MatchResponse`, `JoinRoomRequest`, `RoomViewRequest`, `RoomView`, `PlayerView`, `MoveRequest`, `ShootRequest`, `ShotRejected`, `MatchEnded`, `ActionRejected`, `SessionInfoRequest`, `SessionInfo`), `NodeConfig.java` defines process settings, and `Names.java` defines the mesh and type names. `Hosting.java` configures hosting and diagnostics and runs the stdin control loop (`drain`, `shutdown`), `Control.java` defines the control markers and deadlines, and `Stores.java` creates the Redis Location and Relocation stores. `HttpProtocol.java` holds the HTTP method, content type, and status constants, `Rules.java` the shot range, walls, and rejection reasons, and `Roles.java` the node roles. Add contracts used by several processes here, and keep combat facts in `RoomState` so separate processes do not independently modify the same fact.

### E2E client

`e2e/src/main/java/MyGame/e2e/Client.java` connects through the public connector, and `e2e/src/main/java/MyGame/e2e/Scenarios.java` checks results observable by the client. `e2e/src/main/java/MyGame/e2e/Nodes.java` starts the nodes, keeps their logs, and cleans up the resources of each run; `e2e/src/main/java/MyGame/e2e/Program.java` reports cleanup failures after scenario failures, and either one makes the exit code 1. `e2e/src/main/java/MyGame/e2e/Structure.java` validates the actual files before startup. Keep the scenario order `lobby,hit,reconnect,update`. Verify new gameplay through received client results; the unit tests `server/lobby/src/test/java/MyGame/lobby/application/MatchmakerTest.java` and `server/match/src/test/java/MyGame/match/domain/RoomStateTest.java` cover the matching and combat rules without Framework. The project includes no formatter configuration.

## ZLink concepts in this project

### RouteMesh

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-arena`). [Channel messaging](https://docs.zlink.systems/java/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` accepts a player's first entry and `Room` is a User Spot that the lobby creates on demand before it answers the HTTP request. The [Spot guide](https://docs.zlink.systems/java/guide/server/21-spot/) describes registration and calls for these objects. `Player` is addressed by player ID; the [Actor guide](https://docs.zlink.systems/java/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/java/guide/server/23-stream/), which processes packets on client connections, `GameSession` gets or creates the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/java/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation, created in `server/shared/src/main/java/MyGame/Stores.java`, and `docker-compose.yml` defines the Redis service of the e2e runs. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/java/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Join.join` requests deferred admission to the prepared `Room`, `Room.onActorJoin` accepts or rejects it, and `Player.onJoinCompleted` observes the completion result. [Actor membership](https://docs.zlink.systems/java/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters (`RoomTransfer`, `PlayerTransfer`), registered with `preserveStateWith(...)`, capture and restore game state, while Framework transfers workload and routes. The lobby keeps its queue in process memory and is not a relocation target. `Hosting.run` handles the `drain` control by requesting a public planned-maintenance relocation and observing safe shutdown; the [Relocation guide](https://docs.zlink.systems/java/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client sends `POST /match` with `MatchRequest` (`playerId`, `mode`) to the lobby. `HttpLobby` passes it to `Matchmaker.match`, and `Matchmaker.match` holds the response until another player of the same mode arrives.
2. When the second request arrives, `Matchmaker` assigns `room-<sequence>` and calls `ZLinkRooms.prepare`, and `ZLinkRooms.prepare` creates the Room with `getOrCreate(roomId, Names.ROOM)`. After the Room is prepared, both HTTP responses complete with `MatchResponse` (`roomId`, `sessionEndpoint`).
3. Each client connects to the returned Session endpoint over STREAM and sends the first packet, `JoinRoomRequest`. `GameSession` checks the first packet type, calls `getOrCreate` for the Player Actor by `playerId`, binds it with `bindOrGet`, and relays the original packet.
4. A new Actor starts in the Entry Spot. `Join.join` checks the identity and requests `joinSpot(request.roomId(), request).defer()`. `Room.onActorJoin` admits the player, and the Room sends the current `RoomView` to the players.
5. `MoveRequest` and `ShootRequest` are delivered to `Room` through `Actions`. `RoomState` decides range, wall, and damage; `Actions.forward` delivers the results through the receiving Actor's bound Session.
6. To replace a Match node, the `drain` control in `Hosting.run` requests relocation. Framework takes the game state from `capture` in `RoomTransfer` and `PlayerTransfer` and calls `restore` on another node. The lobby is outside host relocation.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the Java starter: choose Java in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/java/templates/java/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `match-http-lobby`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature | File in this sample | What to transfer | What to grow in the starter alongside |
| --- | --- | --- | --- |
| Typed messages | `server/shared/src/main/java/MyGame/JoinRoomRequest.java` | The records of the features you adopt: `JoinRoomRequest`, `RoomViewRequest`, `RoomView`, `PlayerView`, `MoveRequest`, `ShootRequest`, `ShotRejected`, `MatchEnded`, `ActionRejected`; `MatchRequest` and `MatchResponse` for the HTTP lobby; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The registration names in `Names.java`; the starter already has `Control.java` and `Json.java` |
| Actor | `server/match/src/main/java/MyGame/match/infrastructure/zlink/Player.java`, `server/match/src/main/java/MyGame/match/infrastructure/zlink/PlayerFactory.java` | `Player` with its join-completion handling, and its factory | A new `server/match/` module (`build.gradle.kts`, `Program.java`) with `include("server:match")` in `settings.gradle.kts`, its `server/config/` files, and the Actor factory registration in `Program.java`; the node declaration in `zlink.structure.json` and the check in `Structure.java`. The starter's `Nodes.java` starts only the Session role, so extend `Nodes.java` and `Roles.java` for the new role |
| Spot | `server/match/src/main/java/MyGame/match/infrastructure/zlink/Room.java`, `server/match/src/main/java/MyGame/match/domain/RoomState.java` | `Room` and its `RoomState` combat rules; keep the rules in the domain class | The Spot factory registration in `Program.java`; the `spots` and `nodes` declarations in `zlink.structure.json`, and a `Structure.java` check that compares them with the registrations (the starter declares no Spots) |
| Entry and first admission | `server/match/src/main/java/MyGame/match/infrastructure/zlink/Entry.java`, `server/match/src/main/java/MyGame/match/infrastructure/zlink/Join.java` | `Join.join`, which defers the admission to the prepared Room | The Entry Spot registration and its `entry` declaration; a first-entry scenario and a `reconnect` scenario that compares state after reconnection |
| Session binding | `server/session/src/main/java/MyGame/session/GameSession.java` | The first-packet check, then find or create the Actor, bind, and relay the original typed packet | The same first-packet type as `Join.join`; transfer it into the starter's `PlayerSession.java` |
| Host relocation | `server/match/src/main/java/MyGame/match/infrastructure/zlink/RoomTransfer.java`, `server/match/src/main/java/MyGame/match/infrastructure/zlink/PlayerTransfer.java`, `server/match/src/main/java/MyGame/match/infrastructure/zlink/Transfer.java`, `server/shared/src/main/java/MyGame/Hosting.java` | The snapshot adapters with `capture` and `restore` for the Room state and the Player | The `preserveStateWith(...)` registrations; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node accepts new work |
| Dynamic dungeon | `server/lobby/src/main/java/MyGame/lobby/infrastructure/ZLinkRooms.java`, `server/lobby/src/main/java/MyGame/lobby/application/RoomProvisioner.java` | The dynamic User Spot creation pattern, here for Rooms: `getOrCreate` through the public Spot manager before the client is told where to connect | The dynamic `room` Spot in `zlink.structure.json` and the `lobby` to `location` "Find room" edge |
| HTTP lobby | `server/lobby/src/main/java/MyGame/lobby/infrastructure/HttpLobby.java`, `server/lobby/src/main/java/MyGame/lobby/application/Matchmaker.java` | The HTTP endpoint and the matching port; keep matching decisions in `Matchmaker` | A new `server/lobby/` module, its `lobby.json` (`httpEndpoint`, `sessionEndpoint`), and the `lobby` node and `HTTP` edge in `zlink.structure.json`; a `lobby` scenario that checks both players receive the same room |

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the `Scenario` enum in `server/shared/src/main/java/MyGame/Scenario.java`, to the dispatch in `e2e/src/main/java/MyGame/e2e/Scenarios.java`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the typed records in `server/shared/src/main/java/MyGame/` first, then move each handler that fails to compile to the new messages. When you change the first-entry packet, update the first-packet check in `GameSession` and `Join.join` to receive the same type.

Keep each piece of state in the object that decides it. Combat facts belong in `RoomState`, the waiting queue in `Matchmaker`, and `Player` keeps no application state. When you change `RoomState` fields, update `RoomTransfer`'s `capture` and `restore` together; the lobby queue is not relocated. A missing field disappears after relocation, and the `update` scenario detects the loss.

Run the HTTP request path with the `lobby` scenario. The lobby answers only after the Room is prepared, so the client never connects to a Room that does not exist; keep that order when you change `Matchmaker` or `ZLinkRooms`.

To name roles differently from this sample, change the type names, `Names.java`, `server/config/`, the module and package names with `settings.gradle.kts`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use public Maven Central packages at the versions pinned centrally in `build.gradle.kts`, and keep project dependencies between this project's own modules; do not commit local package feeds or dependencies on repository sources. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

`RoomState` owns combat facts, `Matchmaker` owns the waiting queue, and Session binding owns identity; `RoomView` contains game state only. Store player IDs in `Room` state, never Actor references, because host relocation does not call `onJoinedActor` again on the target. Framework owns routing, membership, execution serialization, and relocation. Reply to the requester through the handler Actor's bound Session (`actor.context().boundSession().send(...)`). Send other pushes to the Actor by player ID with `ZLinkActorClient.sendToActor`, and let a Spot Actor handler such as `Actions.forward` forward the message to the bound Session. `SessionInfo` reads identity from the Session binding, and game-state messages contain no binding identity.

### Asynchronous execution and lifecycle

Compose public asynchronous calls as `CompletionStage` chains as the template does, and avoid `.join()`, `.get()`, `Thread.sleep`, or blocking I/O in handlers and callbacks; the `.join()` calls in `Hosting.java` run on the startup and control thread, outside any handler. Framework's execution gate limits concurrent turns over the same state, so do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not wait for work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/java/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. `Room` is a User Spot created on demand with `getOrCreate`; [Activation and lifetime](https://docs.zlink.systems/java/guide/server/34-activation-lifetime/) explains creation and initialization. The HTTP adapter removes an unpaired request when its client disconnects, and shutdown fails pending requests (README Deployment sequence). Use `joinSpot(...).defer()` and the public completion callback `onJoinCompleted` for Spot changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. Handlers are registered by `options.addHandlersFromPackageOf(...)` in each `Program.java`; a new handler class outside the packages those calls cover is not registered unless you register it explicitly (in this sample, `match` registers the handlers under `server/match/src/main/java/MyGame/match/infrastructure/zlink/`, and the lobby registers none).

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/java/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, `.gradle/`, and `zlink.license` files from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains the effective settings that `Nodes.java` writes at node startup and the logs of each node.

Settings files are `lobby.json`, `matchA.json`, `matchB.json`, `session.json`. Each node start has its own directory `<node>-<uuid>/` with `<node>-stderr.log` for diagnostic stderr and `<node>-stdout.log` for readiness and control stdout; a replaced node (the `update` scenario) logs to a new directory. In local mode `compose.log` records the Redis Compose output of the run and `.logs/build.log` records the local build. In Docker mode `.logs/docker-<project>/` holds `build.log`, `redis-start.log`, `redis.log`, and `cleanup.log`.

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
