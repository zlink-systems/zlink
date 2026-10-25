# MyGame development guide

## Project overview

This Node.js 22 (TypeScript, NestJS) project is an MMO server example with isolated worlds, each with its own Session endpoint, mesh, Redis database, and Player Actors. Players earn persistent gold and switch between the fixed channel Zones of a world without replacing their connected Session. It runs an Accounts node that publishes the world list over HTTP, Session nodes A and B, Channel nodes A1, A2, and B, and a separate Redis per world. `package.json` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime. [한국어](AGENTS.ko.md) provides the same guide in Korean.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Node.js guide](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in `package.json`.

## Development commands

Default builds and e2e run nodes and the runner in Docker without a host Node.js installation. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario worlds,channel,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario worlds,channel,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install Node.js 22 or later and npm, then add `--local` or `-Local`; the nodes run as local processes with Redis in Docker. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Accounts and Sessions

`server/session/main.ts` registers STREAM and the mesh listener of one world, and `server/session/session.ts` handles connection callbacks and forwards only packets in `allowedClientPackets`. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `Session` accepts `EnterWorldRequest` as the first packet and rejects internal packets with `ActionRejected(packet-not-allowed, 0)`. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Channels and players

An Actor is a game state object addressed by ID; `Player` in `server/channel/player.ts` owns the player's `GoldBalance` domain object, which decides gold and earn timing. `PlayerData` loads gold when the Actor is created and saves accepted changes before the domain state changes. A Spot is a state object that accepts Actors and executes shared game rules; `Zone` in `server/channel/spots.ts` represents one channel, admits the player, and owns connected player IDs.

`server/channel/main.ts` registers the Entry Spot, factories, and handlers, and creates the Zones listed in `zoneIds` of the node configuration. Add handlers that combine player rules with Zone rules in `server/channel/handlers.ts`, and list every new handler in the `providers` array of `server/channel/main.ts`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Shared contracts and hosting

`server/shared/messages.ts` defines typed messages and the `GameRules` and `Rejection` contract constants. `server/shared/constants.ts` defines names, scenario IDs, and control strings, and `server/shared/config.ts` defines process settings. `server/shared/hosting.ts` configures hosting and diagnostics and handles runtime control commands, while `server/shared/diagnostics.ts` writes diagnostic records to stderr. Define messages in `server/shared/messages.ts`: the build generates packet schemas from that directory. `meshName` in `server/shared/constants.ts` derives each world's mesh name, and `worldId` in the node configuration selects the world.

### E2E client

`e2e/client.mts` connects through the public connector and runs scenarios that check results observable by the client. `e2e/runner.ts` starts nodes, runs domain boundary checks, captures logs, and cleans up each run; `e2e/structure.ts` validates composition. Verify gameplay through received client results.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses one mesh per world, named by `meshName(worldId)`. Worlds are isolated by mesh and by Redis endpoint, so an Actor in one world is never addressed from another. [Channel messaging](https://docs.zlink.systems/node/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles the first admission of an Actor. `Zone` represents one channel and owns connected player IDs. A channel here is a game space selected inside a world, distinct from a messaging channel; this project registers no pub/sub subscriptions. `Player` retains gold when its host Spot changes. The [Spot guide](https://docs.zlink.systems/node/guide/server/21-spot/) and [Actor guide](https://docs.zlink.systems/node/guide/server/22-actor/) describe registration, creation, and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/node/guide/server/23-stream/), `Session` obtains the Player Actor from `EnterWorldRequest`, binds it, and relays only allowlisted client packets; internal packets receive `ActionRejected(packet-not-allowed, 0)`. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/node/guide/server/24-actor-session/) describes this relationship.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/node/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `EnterPlayer` and `ChangeChannel` request deferred admission to a Zone with `joinSpot(...).defer()`, `Zone.onActorJoin` accepts it, and `Player.onJoinCompleted` observes the completion result. [Actor membership](https://docs.zlink.systems/node/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload so the host can shut down. Application adapters capture and restore game state: `PlayerRelocation` stores gold and remaining cooldown milliseconds, and `ZoneRelocation` stores connected player IDs. Framework transfers workload and routes. `runHost` in `server/shared/hosting.ts` requests public host relocation on `drain` and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/node/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client reads the world list from Accounts with `GET /worlds`, connects to the Session endpoint of the chosen world over STREAM, and sends the first packet, `EnterWorldRequest`. `Session` checks the first packet name, calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. `PlayerFactory` loads the player's gold from the world's Redis. The `EnterPlayer` handler checks the identity and requests `joinSpot(zoneId(Game.firstChannel), ...).defer()`. `Zone.onJoinedActor` pushes `PlayerView` with the world, channel, node, and gold.
3. `EarnGold` asks `GoldBalance` to decide the fixed reward and cooldown. It rejects early and over-limit requests without changing state, and saves accepted gold before updating the Actor. `ViewPlayer` resends the view through the bound Session.
4. `ChangeChannel` requests admission to another Zone with `joinSpot(zoneId(channel), ...).defer()`. The client connection and the bound Session do not change, even when that Zone is on another node. After a reconnect, `EnterZone` checks the identity and resends the view.
5. To replace a node, the `drain` command in `runHost` requests relocation. Framework transfers the gold and remaining cooldown milliseconds from `PlayerRelocation` and connected player IDs from `ZoneRelocation` to another node.

## Extend this template's domain

Extend this template's domain to build an MMO world-and-channel server. Keep one owner for each game decision.

1. Put player gold and earn timing in `GoldBalance` in `server/channel/domain.ts`. Put connected player IDs and channel membership views in `Zone` in `server/channel/spots.ts`.
2. Add typed client and server messages in `server/shared/messages.ts`. Add every client-sendable packet name to `allowedClientPackets` in `server/session/session.ts`.
3. Add observable assertions to `e2e/client.mts`. Add each new scenario ID and its execution order to `server/shared/constants.ts` and `zlink.structure.json` (`run.scenarios` and `run.commands`). Preserve existing scenario coverage.

## Development considerations

### State and message contracts

Use public npm packages at the versions pinned in `package.json`, and keep imports between this project's own modules relative. Process typed messages with the default Framework JSON serializer so processes use the same contract; the build generates the packet schemas and `server/shared/hosting.ts` loads them. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

`GoldBalance` owns gold and earn timing; `Zone` owns connected player IDs. `PlayerRelocation` captures gold and cooldown remaining milliseconds, and `ZoneRelocation` captures the player ID list. Store player IDs in Spots, without assuming an Actor object reference remains usable after movement; host relocation does not repeat `onJoinedActor` on the target, so a Spot cannot rely on a stored reference for later delivery. Reply to the requesting Actor through the handler's bound Session, and push to other players by addressing the Actor by player ID. `SessionInfo` is the source of Actor identity. Node.js typed handlers currently receive the fields without the declared class (#1731), so `DeliverView` restores the class from a packet-name table before forwarding; keep that table in step with the pushes you add. Keep the Message Follow duration and the derived `Limits.safeMs` observation deadline in `server/shared/constants.ts`.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and do not block handler execution with synchronous I/O or busy loops. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/node/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. [Activation and lifetime](https://docs.zlink.systems/node/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback for channel changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using the saved channel and gold. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/node/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `dist/`, and `node_modules/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `e2e/runner.ts` at node startup and logs for each node.

Settings files are `accounts.json`, `channelA1.json`, `channelA2.json`, `channelB.json`, `sessionA.json`, and `sessionB.json`. `NodeProcess` in `e2e/runner.ts` records diagnostic stderr in `<node>-<n>.err.log` and readiness/control stdout in `<node>-<n>.out.log`, where `<n>` counts the processes started in the run; a replaced node has a second pair with a later `<n>`. The client packets each scenario received are in `<scenario>-<suffix>.jsonl`. Docker command logs of the run are `docker-<label>.log`, and local build output is in `.logs/build.log`. The shell runner writes Docker build, Redis, and outer cleanup logs to `.logs/docker-<project>/`.

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
