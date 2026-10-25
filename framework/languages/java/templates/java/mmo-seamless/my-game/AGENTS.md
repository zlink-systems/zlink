# MyGame development guide

## Project overview

This Java 25 project is an MMO server example in which players move across Zone boundaries. It runs a Session gateway for connections, Zone hosts A and B (six Zones, three per host), an Instance host for Dungeons, and Redis for location and relocation state. `build.gradle.kts` pins the ZLink Framework to `0.30.2` and the binding to `1.20.0`.

The project layout below describes where to extend this template's domain. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Java guide](https://docs.zlink.systems/java/guide/server/), [API reference](https://docs.zlink.systems/java/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the version pinned in `build.gradle.kts`.

## Development commands

Default builds and e2e run nodes and the client in Docker, so they need Docker with Compose v2 and no host JDK. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario cross,border,reconnect
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
pwsh -NoProfile -File ./run-e2e.ps1 -CheckDomain
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario cross,border,reconnect
./run-e2e.sh --check-structure
./run-e2e.sh --check-domain
```

For host process debugging, install JDK 25 and add `--local` or `-Local`. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/session/src/main/java/MyGame/session/Program.java` registers STREAM and the mesh listener, and `server/session/src/main/java/MyGame/session/GameSession.java` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `GameSession.onDispatch` requires `JoinWorldRequest` as the first packet, gets or creates the Player Actor by `playerId`, binds it, and relays the original packet. Add authentication before binding and retain the typed relay so Framework can identify the connection for replies to that player.

### Zones and players

An Actor is a game state object addressed by ID. `ZonePositionDomain` in `server/zone/src/main/java/MyGame/zone/ZonePositionDomain.java` owns coordinates, sequence, movement budget, admission and visibility state. `Zone` owns the domain instance and delivers its typed events through local Actor messages and adjacent Zone subscriptions.

`server/zone/src/main/java/MyGame/zone/Program.java` registers the Entry Spot, the Zone and Player factories, and the `border` channel. `server/zone/src/main/java/MyGame/zone/Entry.java` and `server/zone/src/main/java/MyGame/zone/EnterWorld.java` handle a player's first entry into the world. Add handlers that combine player rules with shared Zone rules in `server/zone/src/main/java/MyGame/zone/Actions.java`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/instance/src/main/java/MyGame/instance/Program.java`, `Dungeon.java`, and `DungeonActions.java` in the same directory create Dungeons on demand and return their information. `Dungeon` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract; it is registered with `recreateOnRelocation()`, so its state is not carried across a host relocation. Add party admission rules, encounters, rewards, and Dungeon lifecycle behavior in this project.

### Shared contracts and hosting

`server/shared/src/main/java/MyGame/` holds the messages and game constants shared by processes (`WorldState`, `JoinWorldRequest`, `MoveRequest`, `EnterZone`, `Position`, `ActionRejected`, visibility events, `BorderPresence`, `OpenDungeonRequest`, `DungeonOpened`, `SessionInfoRequest`, `SessionInfo`), `NodeConfig.java` defines process settings, and `Names.java` defines the mesh and type names. `GameSession` in `server/session/src/main/java/MyGame/session/GameSession.java` contains the client packet allow-list. `Rules.java` holds world dimensions and the shared integer and movement limits. Add new typed contracts here, and put position decisions in `ZonePositionDomain` so one domain owns each fact.

### E2E client

`e2e/src/main/java/MyGame/e2e/Client.java` connects through the public connector, `Scenarios.java` checks client replies and push events, and `DomainChecks.java` tests domain boundaries in the same executable before node startup. `Nodes.java` starts the nodes, keeps their logs, and cleans up each run; `Structure.java` validates the actual files before startup. Add scenarios in `Scenarios.java`, and keep `Scenario.java`, `zlink.structure.json`, and the README scenario table aligned.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.MESH` (`my-game-world`). Channels distinguish messages by purpose and name, and the `border` channel carries typed publications between Zones. [Channel messaging](https://docs.zlink.systems/java/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` accepts a player's first entry, `Zone` represents a geographical Zone, and `Dungeon` represents a dynamically created Dungeon. The [Spot guide](https://docs.zlink.systems/java/guide/server/21-spot/) describes registration and calls for these objects. `ZonePositionDomain` retains Zone game state across relocation; the [Actor guide](https://docs.zlink.systems/java/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/java/guide/server/23-stream/), which processes packets on client connections, `GameSession` gets or creates the Player Actor, binds it, and relays the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/java/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation, created in `server/shared/src/main/java/MyGame/Stores.java`, and `docker-compose.yml` defines the Redis service of the e2e runs. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/java/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Player.move` requests deferred admission after `ZonePositionDomain` accepts a move; `Zone.onActorJoin` validates the server-computed arrival and `Player.onJoinCompleted` sends the typed completion reply. [Actor membership](https://docs.zlink.systems/java/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. `ZoneTransfer`, registered with `preserveStateWith(...)`, captures the Zone domain snapshot with remaining movement-budget milliseconds; Framework transfers workload and routes. `Hosting.run` handles the `drain` control by requesting public planned-maintenance relocation and observing safe shutdown; the [Relocation guide](https://docs.zlink.systems/java/guide/server/37-relocation/) explains the procedure.

### Border notifications

Zone state publishes appearance, movement and departure when each fact changes. `BorderRoutes` delivers adjacent Zone events through the same Actor delivery handlers used for local events; the same fact is sent through only one route to each observer.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinWorldRequest`. `GameSession` enforces the client packet allow-list, gets or creates the Player Actor, binds it and relays the original typed packet.
2. A new Actor starts in the Entry Spot. `EnterWorld.join` checks identity and asks `ZonePositionDomain` to resolve the initial location before requesting deferred Zone admission.
3. `Zone.onActorJoin` validates the admission and returns a typed `WorldState` completion reply. Once joined, the Zone domain owns position, sequence, movement budget and visibility.
4. `Actions.move` submits a delta and sequence to the domain. The server checks and calculates the destination; a cross-Zone move carries the remaining budget as milliseconds into the next Zone admission.
5. To replace a node, the `drain` control in `Hosting.run` requests relocation. `ZoneTransfer` captures the domain snapshot with remaining budget milliseconds; Framework moves workload and routes.

## Extend this template's domain

Extend this template's domain to build a seamless MMO server. Add rules to the object that owns their state and decision.

- Domain state and decisions: `server/zone/src/main/java/MyGame/zone/ZonePositionDomain.java`.
- Typed contracts: `server/shared/src/main/java/MyGame/`.
- Session client packet allow-list: `server/session/src/main/java/MyGame/session/GameSession.java`.
- Domain boundary checks: `e2e/src/main/java/MyGame/e2e/DomainChecks.java`.
- Scenario behavior and push observations: `e2e/src/main/java/MyGame/e2e/Scenarios.java` and `Client.java`.

Add the typed contract first, implement its state transition in `ZonePositionDomain`, permit only client-originated packets in the Session allow-list, add a domain boundary check, and observe acceptance, rejection and unchanged state in a scenario. Add the scenario to `Scenario.java`, `zlink.structure.json` (`run.scenarios` and `run.commands`) and the README table. Run `./run-e2e.sh --check-domain`, `./run-e2e.sh --build-only`, and `./run-e2e.sh`.

## Development considerations

### State and message contracts

Use public Maven Central packages at the versions pinned centrally in `build.gradle.kts`, and keep project dependencies between this project's own modules; do not commit local package feeds or dependencies on repository sources. Process typed messages with the default Framework JSON serializer so processes use the same contract. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

Manage each game fact, such as a player's coordinate, in the object responsible for changing it. Store player IDs and observational copies in Spots, never Actor references, because host relocation does not call `onJoinedActor` again on the target. Sessions forward packets after authentication and binding; the Actor or Spot responsible for the state makes movement and gameplay decisions. Reply to the requester through the handler Actor's bound Session (`actor.context().boundSession().send(...)`). Send other pushes to the Actor by player ID with `ZLinkActorClient.sendToActor`, and let a Spot Actor handler such as `Actions.deliver` forward the message to the bound Session. `SessionInfo` reads identity from the Session binding, and game-state messages contain no binding identity.

### Asynchronous execution and lifecycle

Compose public asynchronous calls as `CompletionStage` chains as the template does, and avoid `.join()`, `.get()`, `Thread.sleep`, or blocking I/O in handlers and callbacks; the `.join()` calls in `Hosting.java` run on the startup and control thread, outside any handler. Framework's execution gate limits concurrent turns over the same state, so do not protect that state again with another lock or queue. `Program.java` sets `SPOT_WIDE` execution mode on `Zone`, so handlers, timers, and callbacks of that Spot and its member Actors share one gate. Awaiting can retain the turn, so do not wait for work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/java/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Apply the User Spot contract to `Dungeon` even though its process is named Instance; [Activation and lifetime](https://docs.zlink.systems/java/guide/server/34-activation-lifetime/) explains creation and initialization. `Hosting.run` creates the fixed Zones of a host at startup with `getOrCreate` in the declared order, so start Zone A before Zone B as the README Deployment sequence describes. Use `joinSpot(...).defer()` and the public completion callback `onJoinCompleted` for Spot changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. Handlers are registered by `options.addHandlersFromPackageOf(...)` in each `Program.java`; a new handler class outside the packages those calls cover is not registered unless you register it explicitly.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/java/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, `.gradle/`, and `zlink.license` files from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains the effective settings that `Nodes.java` writes at node startup and the logs of each node.

Settings files are `instance.json`, `session.json`, `zoneA.json`, `zoneB.json`. Each node start has its own directory `<node>-<uuid>/` with `<node>-stderr.log` for diagnostic stderr and `<node>-stdout.log` for readiness and control stdout; a replaced node (the `update` scenario) logs to a new directory. In local mode `compose.log` records the Redis Compose output of the run and `.logs/build.log` records the local build. In Docker mode `.logs/docker-<project>/` holds `build.log`, `redis-start.log`, `redis.log`, and `cleanup.log`.

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
