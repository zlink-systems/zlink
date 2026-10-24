# MyGame development guide

## Project overview

This Node.js 22 (TypeScript, NestJS) project is an action/FPS server example with a matchmaking lobby and server-authoritative duel rooms. Players enter the lobby over STREAM, are paired by mode, and move and shoot in a dynamically created Room. It runs a Session gateway for connections, Match nodes A and B that host the Matchmaking Spot and the Rooms, and Redis for location and relocation state. `package.json` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime. [한국어](AGENTS.ko.md) provides the same guide in Korean.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Node.js guide](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in `package.json`.

## Development commands

Default builds and e2e run nodes and the runner in Docker without a host Node.js installation. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario hit,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario hit,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install Node.js 22 or later and npm, then add `--local` or `-Local`; the nodes run as local processes with Redis in Docker. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/session/main.ts` registers STREAM and the mesh listener, and `server/session/session.ts` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `Session` accepts `QueueRequest` as the first packet. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Matchmaking and rooms

A Spot is a state object that accepts Actors and executes shared game rules. `Matchmaking` in `server/match/spots.ts` is the fixed lobby Spot that queues players, and `MatchmakingState` in `server/match/domain.ts` holds the queue and the room sequence. `Room` in the same `spots.ts` is created per match, and `RoomState` in `domain.ts` holds positions, HP, action sequences, wall intersection, and the winner. Change game rules in `domain.ts`, and keep Framework callbacks and I/O in `spots.ts` and `handlers.ts`. An Actor is a game state object addressed by ID; `Player` in `server/match/player.ts` holds no application state and reports a rejected admission.

`server/match/main.ts` registers the Entry Spot, factories, and handlers, and creates the fixed `matchmaking` Spot on the node whose `bootstrapMatchmaking` setting is true. Add handlers that combine player rules with Room rules in `server/match/handlers.ts`, and list every new handler in the `providers` array of `server/match/main.ts`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition. The `roomCapacity` setting limits the Room Spots per node.

### Shared contracts and hosting

`server/shared/messages.ts` defines messages shared by processes, `server/shared/constants.ts` defines names, scenario IDs, limits, and control strings, and `server/shared/config.ts` defines process settings. `server/shared/hosting.ts` configures hosting and diagnostics and handles runtime control commands, while `server/shared/diagnostics.ts` writes diagnostic records to stderr. Define messages in `server/shared/messages.ts`: the build generates packet schemas from that directory. Match state stays in `Matchmaking` and `Room`, so separate processes do not independently modify the same fact.

### E2E client

`e2e/client.mts` connects through the public connector and runs the scenarios, which check results observable by the client. `e2e/runner.ts` starts nodes, captures logs, and cleans up what each run created, while `e2e/structure.ts` validates composition. Verify new gameplay through received client results; the project includes no separate unit-test project.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.mesh` (`my-game-arena`). [Channel messaging](https://docs.zlink.systems/node/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles the first admission of an Actor, `Matchmaking` is the fixed lobby Spot, and `Room` represents one dynamically created match. `Player` carries no game state of its own, so the Spots keep the facts while Actors move. The [Spot guide](https://docs.zlink.systems/node/guide/server/21-spot/) describes registration and calls for these objects, and the [Actor guide](https://docs.zlink.systems/node/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/node/guide/server/23-stream/), which processes packets on client connections, `Session` obtains the Player Actor from `QueueRequest`, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/node/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/node/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `EnterQueue` requests deferred admission to `Matchmaking` with `joinSpot(...).defer()`, and `Matchmaking` creates a Room for each pair and sends `RoomAssignment` to the Actors. The `Assignment` handler then requests admission to the Room, `Room.onActorJoin` accepts the player through `RoomState.admit`, and `Player.onJoinCompleted` observes a rejection. [Actor membership](https://docs.zlink.systems/node/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state (`MatchmakingState` and `RoomState`, captured by `MatchmakingRelocation` and `RoomRelocation`), while Framework transfers workload and routes. `runHost` in `server/shared/hosting.ts` requests public host relocation on the `drain` command and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/node/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `QueueRequest`. `Session` checks the first packet name, calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. The `EnterQueue` handler checks the identity and requests `joinSpot(Names.matchmakingId, ...).defer()`. `Matchmaking.onActorJoin` enqueues the player, and `Matchmaking.onJoinedActor` tries to pair players of the same mode.
3. For a pair, `Matchmaking` allocates a room ID, calls `getOrCreate` for the `Room`, and sends `RoomAssignment` to both Actors by player ID. The `Assignment` handler requests `joinSpot(roomId, ...).defer()`. `Room.onJoinedActor` sends `MatchFound` and broadcasts `RoomView`; `Delivery` forwards these pushes through each bound Session. A player that is already in `Matchmaking` and sends `QueueRequest` again is queued by `QueueAgain`.
4. `Move` and `Shoot` call `Room.move` and `Room.shoot`. A valid action broadcasts `RoomView`, and a winning shot also broadcasts `MatchEnded`; an invalid action sends `ActionRejected` or `ShotRejected` to the requester through its bound Session.
5. After a reconnect, the new Session sends `QueueRequest` again. The `Reconnect` handler of the Room that holds the Actor checks the identity and resends the view through the bound Session.
6. To replace a node, the `drain` command in `runHost` requests relocation. Framework takes the game state from `capture` in `MatchmakingRelocation` and `RoomRelocation` and calls `restore` on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the Node.js starter: choose Node.js in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/node/templates/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `match-entry-lobby`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature                   | File in this sample                                                                                                                                                                                                                | What to transfer                                                                                                                                                                                                                                                                                          | What to grow in the starter alongside                                                                                                                                                                                                                                               |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Typed messages            | `server/shared/messages.ts`                                                                                                                                                                                                        | The classes of the features you adopt: `QueueRequest`, `RoomAdmission`, `RoomAssignment`, `MatchFound`, `MoveRequest`, `ShootRequest`, `RoomViewRequest`, `PlayerView`, `RoomView`, `ShotRejected`, `ActionRejected`, `MatchEnded`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The packet names in `PacketNames`, the registration names in `Names`, and `Rejection` in `server/shared/constants.ts`                                                                                                                                                               |
| Actor                     | `server/match/player.ts`                                                                                                                                                                                                           | `Player` with its admission-rejection handling, `PlayerFactory`, and `PlayerRelocation`, which uses an empty payload                                                                                                                                                                                      | A new `server/match/` project with `main.ts`, its `server/config/` files, and the Actor factory registration; a `match` entry in `NodeEntrypoints` in `server/shared/constants.ts`; the Actor declaration and the structure check                                                   |
| Spot                      | `server/match/spots.ts`, `server/match/domain.ts`                                                                                                                                                                                  | `Matchmaking` and `Room` with their admission and broadcast, and `MatchmakingState` and `RoomState` with the queue and combat rules                                                                                                                                                                       | The Spot factory registrations and the `bootstrapMatchmaking` creation in `server/match/main.ts`; the `spots` and `nodes` declarations in `zlink.structure.json`, and the structure check, which asserts zero Spots in the starter, extended to compare them with the registrations |
| Entry and first admission | `Entry` in `server/match/spots.ts`; `EnterQueue`, `QueueAgain`, `Assignment`, and `Reconnect` in `server/match/handlers.ts`                                                                                                        | `EnterQueue`, the identity check, the deferred join to `Matchmaking`, the hand-over to the Room in `Assignment`, and the state resend of `Reconnect`                                                                                                                                                      | The Entry Spot registration and its `entry` declaration; a first-entry scenario and a `reconnect` scenario that compares state after reconnection                                                                                                                                   |
| Session binding           | `server/session/session.ts`                                                                                                                                                                                                        | The first-packet check, then find or create the Actor, bind, and relay the original typed packet, and the `SessionInfo` reply                                                                                                                                                                             | The same first-packet name as `EnterQueue`; transfer it into the starter's `session.ts`                                                                                                                                                                                             |
| Host relocation           | `PlayerRelocation` in `server/match/player.ts`, `MatchmakingRelocation` and `RoomRelocation` in `server/match/spots.ts`, `Control` in `server/shared/constants.ts` and the control loop in `runHost` of `server/shared/hosting.ts` | The snapshot adapters with `capture` and `restore`; the starter already has `drain` and `shutdown`                                                                                                                                                                                                        | The `preserveStateWith` registrations; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node accepts a new Room                                                                                                 |
| Dynamic dungeon           | `Matchmaking.match` in `server/match/spots.ts`; `Assignment` in `server/match/handlers.ts`                                                                                                                                         | The dynamic User Spot creation pattern; in this sample it creates a `Room` for each pair                                                                                                                                                                                                                  | The dynamic `room` Spot declaration in `zlink.structure.json`; a `hit` scenario that plays a match in the new Room                                                                                                                                                                  |

The starter's `Dynamic dungeon` feature is the creation of a User Spot on demand. This sample uses the same pattern to create `Room`, so the row above describes Rooms.

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the `Scenarios` list in `server/shared/constants.ts`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the classes in `messages.ts` first, then move each handler that fails to compile to the new messages. The build generates packet schemas from `server/shared`, so messages defined elsewhere are not registered. When you change the first-entry packet, update the first-packet check in `Session`, `EnterQueue`, and `Reconnect` to receive the same packet.

Keep each piece of state in the object that decides it. The queue and room sequence belong in `Matchmaking`; positions, HP, action sequences, and the winner belong in `Room`. `Entry` and `Player` keep none. When you change state fields, update that object's snapshot and its `capture` and `restore` together. A missing field disappears after relocation, and the `update` scenario detects the loss.

The fixed `matchmaking` Spot is created on the node whose `bootstrapMatchmaking` is true. Wait for that node to report `READY` before starting another Match node, so the Spot starts on its declared owner while that node is the only placement candidate. A replacement for that node starts with `bootstrapMatchmaking` set to `false`, because the relocated Matchmaking Spot already exists.

To name roles such as Match or Room differently from this sample, change the class names, `Names`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use public npm packages at the versions pinned in `package.json`, and keep imports between this project's own modules relative. Process typed messages with the default Framework JSON serializer so processes use the same contract; the build generates the packet schemas and `server/shared/hosting.ts` loads them. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

Manage each game fact, such as a player's HP, in the object responsible for changing it. Store player IDs in Spots, without assuming an Actor object reference remains usable after movement; host relocation does not repeat `onJoinedActor` on the target, so a Spot cannot rely on a stored reference for later delivery. Reply to the requesting Actor through the handler's bound Session, and push to other players by addressing the Actor by player ID. Session binding owns Actor identity. Node.js typed handlers currently receive the fields without the declared class (#1731), so `Delivery` restores the class from a packet-name table before forwarding; keep that table in step with the pushes you add. Keep the Message Follow duration and the derived `Limits.safeMs` observation deadline in `server/shared/constants.ts`.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and do not block handler execution with synchronous I/O or busy loops. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/node/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Apply the User Spot contract to `Room`, which is created on demand by its ID; [Activation and lifetime](https://docs.zlink.systems/node/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback for queue and Room admission, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using the existing Room state. Framework manages current placement, so do not add an application route cache or reset state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/node/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `dist/`, and `node_modules/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `e2e/runner.ts` at node startup and logs for each node.

Settings files are `matchA.json`, `matchB.json`, and `session.json`. `NodeProcess` in `e2e/runner.ts` records diagnostic stderr in `<node>-<n>.err.log` and readiness/control stdout in `<node>-<n>.out.log`, where `<n>` counts the processes started in the run; a replaced node has a second pair with a later `<n>`. The client packets each scenario received are in `<scenario>-<suffix>.jsonl`. Docker command logs of the run are `docker-<label>.log`, and local build output is in `.logs/build.log`. The shell runner writes Docker build, Redis, and outer cleanup logs to `.logs/docker-<project>/`.

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
