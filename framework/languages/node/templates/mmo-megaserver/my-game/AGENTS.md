# MyGame development guide

## Project overview

This Node.js 22 (TypeScript, NestJS) project is an MMO server example with one shared world in which a Region fills to its capacity and the next Region instance takes the following players. It runs a Session gateway for connections, Region nodes A and B that host capacity-limited Region Spots, an Instance node that hosts Dungeons, and Redis for location and relocation state. `package.json` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime. [한국어](AGENTS.ko.md) provides the same guide in Korean.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Node.js guide](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in `package.json`.

## Development commands

Default builds and e2e run nodes and the runner in Docker without a host Node.js installation. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario region,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario region,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install Node.js 22 or later and npm, then add `--local` or `-Local`; the nodes run as local processes with Redis in Docker. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/session/main.ts` registers STREAM and the mesh listener, and `server/session/session.ts` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `Session` accepts `EnterRegionRequest` as the first packet. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Regions and players

A Spot is a state object that accepts Actors and executes shared game rules; `Region` in `server/region/infrastructure/zlink/spots.ts` admits players up to the node's `capacity`, records positions through `Positions` in `server/region/domain/positions.ts`, and broadcasts the region view. Change game rules in the domain class, and keep Framework callbacks and I/O in `server/region/infrastructure/zlink/`. An Actor is a game state object addressed by ID; `Player` in `server/region/infrastructure/zlink/player.ts` has no application state and forwards a capacity rejection to the Entry.

`Entry` in the same `spots.ts` selects the Region instance. `server/region/main.ts` registers the Entry Spot, factories, and handlers, and lists every handler in its `providers` array; add handlers in `server/region/infrastructure/zlink/handlers.ts`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition. The node settings `capacity` (players per Region) and `regionLimit` (Region Spots per node) set the limits.

### Dungeons

`server/instance/main.ts` registers the Instance node, and `Dungeon` in `server/instance/infrastructure/zlink/spots.ts` is created on demand and answers an information request through `DungeonInfo`. `Dungeon` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract. It keeps no state, so its factory uses `recreateOnRelocation` instead of a snapshot adapter. Add party admission rules, encounters, rewards, and Dungeon lifecycle behavior in this project.

### Shared contracts and hosting

`server/shared/messages.ts` defines messages shared by processes, `server/shared/constants.ts` defines names, scenario IDs, limits, and control strings, and `server/shared/config.ts` defines process settings. `server/shared/hosting.ts` configures hosting and diagnostics and handles runtime control commands, while `server/shared/diagnostics.ts` writes diagnostic records to stderr. Define messages in `server/shared/messages.ts`: the build generates packet schemas from that directory. Region state stays in `Region`, so separate processes do not independently modify the same fact.

### E2E client

`e2e/client.mts` connects through the public connector and runs the scenarios, which check results observable by the client. `e2e/runner.ts` starts nodes, captures logs, and cleans up what each run created, while `e2e/structure.ts` validates composition. Verify new gameplay through received client results; the project includes no separate unit-test project.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.mesh` (`my-game-world`). [Channel messaging](https://docs.zlink.systems/node/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` selects the first Region instance with room, `Region` represents a capacity-limited region instance named by `regionId(region, index)`, and `Dungeon` represents a dynamically created Dungeon. `Player` carries no game state of its own, so the Region keeps positions while Actors move or reconnect. The [Spot guide](https://docs.zlink.systems/node/guide/server/21-spot/) describes registration and calls for these objects, and the [Actor guide](https://docs.zlink.systems/node/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/node/guide/server/23-stream/), which processes packets on client connections, `Session` obtains the Player Actor from `EnterRegionRequest`, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/node/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/node/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Entry.select` requests deferred admission to a Region with `joinSpot(...).defer()`. `Region.onActorJoin` rejects the request with a `RegionFull` reply when the Region is at capacity, `Player.onJoinCompleted` forwards that reply to the Actor, and the `NextRegion` handler selects the next index. [Actor membership](https://docs.zlink.systems/node/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state (`Positions`, captured by `RegionRelocation`), while Framework transfers workload and routes. `runHost` in `server/shared/hosting.ts` requests public host relocation on the `drain` command and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/node/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `EnterRegionRequest`. `Session` checks the first packet name, calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. The `EnterRegion` handler checks the identity and calls `Entry.select` for the first index, which calls `getOrCreate` for the `Region` named by `regionId(region, index)` and requests `joinSpot(...).defer()`.
3. `Region.onActorJoin` admits the player through `Positions.admit`. At capacity it replies `RegionFull`; `Player.onJoinCompleted` sends that reply to the Actor, and `NextRegion` calls `Entry.select` for the next index. After admission, `Region.onJoinedActor` broadcasts `RegionView` to every player in the Region by player ID, and `Delivery` forwards it through the bound Session.
4. `Move` checks the sequence in `Positions.move` and broadcasts the view. `OpenDungeon` calls `getOrCreate` for the `Dungeon`, sends `DungeonInfoRequest` to it as a request, and sends `DungeonOpened` through the bound Session.
5. After a reconnect, the new Session sends `EnterRegionRequest` again. `ReconnectRegion` checks the identity and resends the region view through the bound Session.
6. To replace a node, the `drain` command in `runHost` requests relocation. Framework takes the game state from `capture` in `RegionRelocation` and calls `restore` on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the Node.js starter: choose Node.js in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/node/templates/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `mmo-megaserver`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature                   | File in this sample                                                                                                                                                                                                                                  | What to transfer                                                                                                                                                                                                                                                                          | What to grow in the starter alongside                                                                                                                                                                                                                                            |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Typed messages            | `server/shared/messages.ts`                                                                                                                                                                                                                          | The classes of the features you adopt: `EnterRegionRequest`, `JoinRegion`, `RegionFull`, `MoveRequest`, `PlayerPosition`, `RegionView`, `ViewRequest`, `OpenDungeonRequest`, `DungeonInfoRequest`, `DungeonOpened`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The packet names in `PacketNames`, the registration names in `Names`, and `Rejection` in `server/shared/constants.ts`                                                                                                                                                            |
| Actor                     | `server/region/infrastructure/zlink/player.ts`                                                                                                                                                                                                       | `Player` with its rejection forwarding, `PlayerFactory`, and `PlayerRelocation`, which uses an empty payload                                                                                                                                                                              | A new `server/region/` project with `main.ts`, its `server/config/` files, and the Actor factory registration; a `region` entry in `NodeEntrypoints` in `server/shared/constants.ts`; the Actor declaration and the structure check                                              |
| Spot                      | `server/region/infrastructure/zlink/spots.ts`, `server/region/domain/positions.ts`                                                                                                                                                                   | `Region` with admission, positions, and broadcast, and `Positions` with the capacity and sequence rules                                                                                                                                                                                   | The Spot factory registration with `stableTypeLimit` in `server/region/main.ts`; the dynamic `region` Spot and `nodes` declarations in `zlink.structure.json`, and the structure check, which asserts zero Spots in the starter, extended to compare them with the registrations |
| Entry and first admission | `Entry` in `server/region/infrastructure/zlink/spots.ts`; `EnterRegion`, `NextRegion`, and `ReconnectRegion` in `server/region/infrastructure/zlink/handlers.ts`                                                                                     | `Entry.select` with its on-demand Region creation and the next-index retry after `RegionFull`, and the identity check and state resend of `ReconnectRegion`                                                                                                                               | The Entry Spot registration and its `entry` declaration; a `region` scenario that fills a Region, and a `reconnect` scenario that compares state after reconnection                                                                                                              |
| Session binding           | `server/session/session.ts`                                                                                                                                                                                                                          | The first-packet check, then find or create the Actor, bind, and relay the original typed packet, and the `SessionInfo` reply                                                                                                                                                             | The same first-packet name as `EnterRegion`; transfer it into the starter's `session.ts`                                                                                                                                                                                         |
| Host relocation           | `PlayerRelocation` in `server/region/infrastructure/zlink/player.ts`, `RegionRelocation` in `server/region/infrastructure/zlink/spots.ts`, `Control` in `server/shared/constants.ts` and the control loop in `runHost` of `server/shared/hosting.ts` | The snapshot adapters with `capture` and `restore`; the starter already has `drain` and `shutdown`                                                                                                                                                                                        | The `preserveStateWith` registrations; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node serves a new Region                                                                                             |
| Dynamic dungeon           | `server/instance/infrastructure/zlink/spots.ts`, `server/instance/main.ts`; `OpenDungeon` in `server/region/infrastructure/zlink/handlers.ts`                                                                                                        | The dynamic User Spot creation pattern and the Dungeon information request                                                                                                                                                                                                                | A new `server/instance/` project with `main.ts` and its config; an `instance` entry in `NodeEntrypoints`; the dynamic `dungeon` Spot and the `GetOrCreate` edge in `zlink.structure.json`; a `dungeon` scenario                                                                  |

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the `Scenarios` list in `server/shared/constants.ts`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the classes in `messages.ts` first, then move each handler that fails to compile to the new messages. The build generates packet schemas from `server/shared`, so messages defined elsewhere are not registered. When you change the first-entry packet, update the first-packet check in `Session`, `EnterRegion`, and `ReconnectRegion` to receive the same packet. Only Region nodes register the application Entry and its first-admission handler, so only those nodes declare an Entry in `zlink.structure.json`.

Keep each piece of state in the object that decides it. Membership, positions, and sequence validation belong in `Region`; `Entry` owns only the index selection after an explicit `region-full` rejection. When you change state fields, update `Positions` and the `RegionRelocation` snapshot together. A missing field disappears after relocation, and the `update` scenario detects the loss.

Region capacity defaults to two players per Region and the Region stable-type limit to two Region Spots per node, as in the shipped configuration. The e2e fixture keeps the Regions of both nodes occupied before drain, so after relocation the survivor is full and only the replacement can serve a new Region. Keep enough capacity on the surviving node when you change these limits.

To name roles such as Region or Dungeon differently from this sample, change the class names, `Names`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use public npm packages at the versions pinned in `package.json`, and keep imports between this project's own modules relative. Process typed messages with the default Framework JSON serializer so processes use the same contract; the build generates the packet schemas and `server/shared/hosting.ts` loads them. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

Manage each game fact, such as a player's position, in the object responsible for changing it. Store player IDs in the Region, without assuming an Actor object reference remains usable after movement; host relocation does not repeat `onJoinedActor` on the target, so a Spot cannot rely on a stored reference for later delivery. Reply to the requesting Actor through the handler's bound Session, and push to other players by addressing the Actor by player ID. `SessionInfo` is the source of Actor identity. Node.js typed handlers currently receive the fields without the declared class (#1731), so `Delivery` restores the class from a packet-name table before forwarding; keep that table in step with the pushes you add. Keep the Message Follow duration and the derived `Limits.safeObservationMs` observation deadline in `server/shared/constants.ts`.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and do not block handler execution with synchronous I/O or busy loops. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/node/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Apply the User Spot contract to `Dungeon` even though its process is named Instance; [Activation and lifetime](https://docs.zlink.systems/node/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback for Region admission, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state. Membership and position survive a disconnected Session until an explicit leave. Framework manages current placement, so do not add an application route cache or reset state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/node/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `dist/`, and `node_modules/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `e2e/runner.ts` at node startup and logs for each node.

Settings files are `instance.json`, `regionA.json`, `regionB.json`, and `session.json`. `NodeProcess` in `e2e/runner.ts` records diagnostic stderr in `<node>-<n>.err.log` and readiness/control stdout in `<node>-<n>.out.log`, where `<n>` counts the processes started in the run; a replaced node has a second pair with a later `<n>`. The client packets each scenario received are in `<scenario>-<suffix>.jsonl`. Docker command logs of the run are `docker-<label>.log`, and local build output is in `.logs/build.log`. The shell runner writes Docker build, Redis, and outer cleanup logs to `.logs/docker-<project>/`.

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
