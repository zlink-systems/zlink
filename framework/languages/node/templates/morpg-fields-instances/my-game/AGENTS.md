# MyGame development guide

## Project overview

This Node.js 22 (TypeScript, NestJS) project is a MORPG server example in which players move between fixed public fields, form parties in a field, and enter party dungeons. It runs a Session gateway for connections, Field nodes A and B that host the fixed Field Spots, Instance nodes A and B that host Dungeons, and Redis for location and relocation state. `package.json` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime. [한국어](AGENTS.ko.md) provides the same guide in Korean.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Node.js guide](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in `package.json`.

## Development commands

Default builds and e2e run nodes and the runner in Docker without a host Node.js installation. Run these commands from this directory.

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

For host process debugging, install Node.js 22 or later and npm, then add `--local` or `-Local`; the nodes run as local processes with Redis in Docker. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/session/main.ts` registers STREAM and the mesh listener, and `server/session/session.ts` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `Session` accepts `JoinWorldRequest` as the first packet. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Fields and players

An Actor is a game state object addressed by ID; `Player` in `server/shared/player.ts` manages the player's coordinate and sequence, and every node that hosts players registers it. A Spot is a state object that accepts Actors and executes shared game rules; `Field` in `server/field/spots.ts` owns movement, pending arrivals, and visibility through `FieldState` in `server/field/domain.ts`. Independent `Party` in `server/field/party.ts` owns party records through `PartyRun` in `server/field/party-domain.ts`. Visibility values are copies of Actor movement state used for observation, so apply player state changes to the Actor.

`server/field/main.ts` registers the Entry Spot, factories, handlers, and the `border` channel, and creates the fixed Fields listed in `fieldIds` of the node configuration. Add handlers that combine player rules with shared field rules in `server/field/handlers.ts`, and list every new handler in the `providers` array of `server/field/main.ts`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/instance/main.ts` registers the Instance node, `Dungeon` in `server/instance/spots.ts` holds the admitted party and stage through `DungeonState` in `server/instance/domain.ts`, and `server/instance/handlers.ts` holds the dungeon handlers. `Dungeon` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract. `Field.enter` creates it on demand and prepares it for a party. Add encounters, rewards, and Dungeon lifecycle behavior in this project.

### Entry and shared Player

`Entry` and the `EnterWorld` handler in `server/shared/entry.ts` receive the first admission of a player Actor on both Field and Instance nodes, because any node that registers the Player factory can receive a new Actor. `EnterWorld` checks the identity and requests the Field that covers the starting coordinate.

### Shared contracts and hosting

`server/shared/messages.ts` defines messages shared by processes, `server/shared/constants.ts` defines names, scenario IDs, limits, and control strings, and `server/shared/config.ts` defines process settings. `server/shared/hosting.ts` configures hosting and diagnostics and handles runtime control commands, while `server/shared/diagnostics.ts` writes diagnostic records to stderr. Define messages in `server/shared/messages.ts`: the build generates packet schemas from that directory. Keep player state in `Player` so separate processes do not independently modify the same fact.

### E2E client

`e2e/client.mts` connects through the public connector and runs the scenarios, which check results observable by the client. `e2e/runner.ts` starts nodes, captures logs, and cleans up what each run created, while `e2e/structure.ts` validates composition. Verify new gameplay through received client results; the project includes no separate unit-test project.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.mesh` (`my-game-world`). Channels distinguish messages by purpose and name, and the `border` channel carries typed publications between Fields. [Channel messaging](https://docs.zlink.systems/node/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles Actor entry, `Field` represents a fixed public field, and `Dungeon` represents a dynamically created party dungeon. `Player` retains movement state when its host Spot changes. The [Spot guide](https://docs.zlink.systems/node/guide/server/21-spot/) describes registration and calls for these objects, and the [Actor guide](https://docs.zlink.systems/node/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/node/guide/server/23-stream/), which processes packets on client connections, `Session` obtains the Player Actor from `JoinWorldRequest`, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/node/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/node/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Player.move` requests deferred admission to the next Field, and `onJoinCompleted` observes the completion result. A party enters a dungeon through the same mechanism: each member's Actor receives `JoinDungeon` and requests admission to the Dungeon. [Actor membership](https://docs.zlink.systems/node/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state (`FieldState`, `DungeonState`, and `Player` coordinates, captured by `FieldRelocation`, `DungeonRelocation`, and `PlayerRelocation`), while Framework transfers workload and routes. `runHost` in `server/shared/hosting.ts` requests public host relocation on the `drain` command and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/node/guide/server/37-relocation/) explains the procedure.

### Border notifications and timers

A timer runs a handler periodically; `BorderTick` uses one to publish information about players near a field border. In this pub/sub exchange, `BorderHandler` receives subscribed information, and `Delivery` forwards notifications through the receiving Actor's bound Session. [Timers and workers](https://docs.zlink.systems/node/guide/server/36-timer-worker/) explains registration and execution of periodic work.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinWorldRequest`. `Session` checks the first packet name, calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. The `EnterWorld` handler checks the identity and calls `Player.move`, which requests `joinSpot(...).defer()` for the Field that covers the coordinate. When the join completes, `Player.onJoinCompleted` sends `WorldState` through the bound Session.
3. Later packets from the same client are handled by the handlers of the Field that holds the Actor (such as `Move`). When movement reaches a coordinate in another Field, `Move` calls `Player.move` again; the client connection does not change, even when that Field is on another node. `Field.publishBorder` publishes nearby positions, and `BorderHandler` and `Delivery` push the result to observers.
4. `CreateParty` and `JoinParty` forward typed requests to an independent Party User Spot. It owns membership and leadership and sends `PartyView` to member Actors, including those in other Fields.
5. When the leader sends `EnterDungeon`, `Field.enter` calls `getOrCreate` for the `Dungeon`, sends `PrepareDungeon` to it as a request, and sends `JoinDungeon` to each member's Actor. `DungeonTravel` requests the join, `Dungeon.onJoinedActor` pushes `DungeonView`, and `Advance` moves the stage.
6. To replace a node, the `drain` command in `runHost` requests relocation. Framework takes the game state from `capture` in `FieldRelocation`, `DungeonRelocation`, and `PlayerRelocation` and calls `restore` on another node.

## Extending the template domain

Extend this template's domain to build a MORPG server. Run the README build and e2e commands before adding features.

1. Extend movement decisions in `server/field/domain.ts` and party run decisions in `server/field/party-domain.ts`. Extend dungeon rules in the Dungeon domain described under project layout, and keep one owner for each state change.
2. Add shared contracts in `server/shared/messages.ts`. The server validates inputs and calculates results. Rejected inputs must preserve values, sequences, and cooldowns.
3. Add only client packets to the Session allowlist in `server/session/session.ts`. Keep internal messages and result packets outside that list.
4. Add received-result scenarios in `e2e/client.mts`. Update the README scenario table and `run.scenarios` in `zlink.structure.json` together. Wait only for the server's `RetryAfterMs` after a time-limited rejection.
5. Update the owning relocation snapshot and boundary checks when adding state. Store remaining milliseconds rather than process timestamps.

## Development considerations

### State and message contracts

Use public npm packages at the versions pinned in `package.json`, and keep imports between this project's own modules relative. Process typed messages with the default Framework JSON serializer so processes use the same contract; the build generates the packet schemas and `server/shared/hosting.ts` loads them. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

Manage each game fact, such as a player's coordinate, in the object responsible for changing it. Store player IDs and observational copies in Spots, without assuming the same Actor object reference remains usable after movement; host relocation does not repeat `onJoinedActor` on the target, so a Spot cannot rely on a stored reference for later delivery. Reply to the requesting Actor through the handler's bound Session, and push to other players by addressing the Actor by player ID. `SessionInfo` is the source of Actor identity. Node.js typed handlers currently receive the fields without the declared class (#1731), so `Delivery` restores the class from a packet-name table before forwarding; keep that table in step with the pushes you add. Keep the Message Follow duration and the derived `Limits.safeShutdownMs` observation deadline in `server/shared/constants.ts`.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and do not block handler execution with synchronous I/O or busy loops. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/node/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Apply the User Spot contract to `Dungeon` even though its process is named Instance; [Activation and lifetime](https://docs.zlink.systems/node/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback for Field and Dungeon changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/node/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `dist/`, and `node_modules/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `e2e/runner.ts` at node startup and logs for each node.

Settings files are `fieldA.json`, `fieldB.json`, `instanceA.json`, `instanceB.json`, and `session.json`. `NodeProcess` in `e2e/runner.ts` records diagnostic stderr in `<node>-<n>.err.log` and readiness/control stdout in `<node>-<n>.out.log`, where `<n>` counts the processes started in the run; a replaced node has a second pair with a later `<n>`. The client packets each scenario received are in `<scenario>-<suffix>.jsonl`. Docker command logs of the run are `docker-<label>.log`, and local build output is in `.logs/build.log`. The shell runner writes Docker build, Redis, and outer cleanup logs to `.logs/docker-<project>/`.

### Diagnostic levels

`runHost` in `server/shared/hosting.ts` enables message flow, which records the stages from message receipt to completion, with this setting.

```typescript
builder.configureDispatch().messageFlow('normal');
```

`normal` records receive, dispatch, and completion stages and errors. The accepted modes are `off`, `errors`, `normal`, and `detailed`; use `detailed` only for the investigation scope that needs it, because it records more.

`server/shared/config.ts` defines no diagnostics key, so change the hosting call, rebuild, and restart to apply startup settings. This project's stdin controls do not change the mode at runtime. [Monitoring](https://docs.zlink.systems/node/guide/server/26-monitoring/) and [Observability diagnostics](https://docs.zlink.systems/node/reference/08-observability-diagnostics/) describe diagnostic records and settings.

### Message flow investigation

`server/shared/diagnostics.ts` writes each diagnostic record to stderr as one JSON line, so inspect message flow in node `.err.log` files; the template creates no separate flow file. A record's `flow` value identifies a message across nodes, and `corr` relates request and reply records. The `body` field starts with `zlink flow:`; failures have `outcome=failed`, `error_type`, and `error_message`.

Read the failure fields and preceding stage together without filtering categories out. Flow IDs differ between runs, so compare passing and failing runs by logical operation, Actor ID, and stage order to find the first missing or failed stage. If startup fails before message flow recording, preserve the startup exception and stack.

### Supplementary tracing and cleanup

In local process mode, set supplementary relocation tracing in the parent environment before starting nodes. Docker nodes do not inherit host environment variables. In PowerShell, use this environment variable.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_RELOCATION = "1"
```

The value applies to child processes started from that environment; preserve stderr alongside message flow. In Bash, set the same variable as follows.

```bash
export ZLINK_DEBUG_FRAMEWORK_RELOCATION=1
```

`ZLINK_DEBUG_FRAMEWORK_RELOCATION` is a version-specific diagnostic switch in the public `0.30.2` package, so do not use it as a routing API. Let the runner clean up its own processes and Compose project, preserve cleanup failures, and avoid stopping unrelated processes or containers.

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

- zlink version: <package versions from package.json>
- OS: <OS, Node.js version>

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

Put the language and the failing behavior in the title (for example, `framework(node): Spot startup fails with empty SpotId`). Record the filed issue number in the README Known issues so the project keeps the place to re-verify when a fixed package is released.
