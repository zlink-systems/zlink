# MyGame development guide

## Project overview

This Node.js 22 (TypeScript, NestJS) project is a board-table game server example in which players take seats, play hands, and reconnect to the same seat. It runs a Session gateway for connections, Table nodes A and B that host Table Spots, and Redis for location and relocation state. `package.json` pins the ZLink Framework and connector packages to `0.30.2`.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime. [한국어](AGENTS.ko.md) provides the same guide in Korean.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [Node.js guide](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the package pinned in `package.json`.

## Development commands

Default builds and e2e run nodes and the runner in Docker without a host Node.js installation. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario table,reconnect
pwsh -NoProfile -File ./run-e2e.ps1 -CheckStructure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario table,reconnect
./run-e2e.sh --check-structure
```

For host process debugging, install Node.js 22 or later and npm, then add `--local` or `-Local`; the nodes run as local processes with Redis in Docker. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README.

## Project layout and extension points

### Session gateway

`server/session/main.ts` registers STREAM and the mesh listener, and `server/session/session.ts` handles connection callbacks and forwards incoming packets. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. `Session` accepts `JoinTableRequest` as the first packet. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player.

### Tables and players

A Spot is a state object that accepts Actors and executes shared game rules; `Table` in `server/table/infrastructure/table.ts` admits players, builds the table view, and publishes it to every seat. The game rules and the application snapshot live in `TableState` in `server/table/domain/table-state.ts`: seats, chips, deck, hands, turn, hand number, pot, and settlement. Change game rules there, and keep Framework callbacks and I/O in `server/table/infrastructure/`. An Actor is a game state object addressed by ID; `Player` in `server/table/infrastructure/player.ts` holds no application state and reports a rejected join to the client.

`server/table/main.ts` registers the Entry Spot, factories, and handlers, and every new handler is added to its `providers` array. Add handlers that combine player rules with table rules in `server/table/infrastructure/handlers.ts`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition. The optional `tableCapacity` setting in a node's configuration sets the public stable-type limit for Tables on that node; omitted or zero leaves capacity unlimited, and the e2e runner sets it so that a replacement node must host the new table.

### Shared contracts and hosting

`server/shared/messages.ts` defines messages shared by processes, `server/shared/constants.ts` defines names, scenario IDs, limits, and control strings, and `server/shared/config.ts` defines process settings. `server/shared/hosting.ts` configures hosting and diagnostics and handles runtime control commands, while `server/shared/diagnostics.ts` writes diagnostic records to stderr. Define messages in `server/shared/messages.ts`: the build generates packet schemas from that directory. Table state stays in `TableState`, so separate processes do not independently modify the same fact.

### E2E client

`e2e/client.mts` connects through the public connector and runs the scenarios, which check results observable by the client. `e2e/domain.test.ts` unit-tests `TableState` and runs after the build with `node --test dist/e2e/domain.test.js`; `npm run typecheck`, `npm run lint`, and `npm run format:check` check types, lint rules, and formatting. `e2e/runner.ts` starts nodes, captures logs, and cleans up what each run created, while `e2e/structure.ts` validates composition. Verify new gameplay through received client results.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names.mesh` (`my-game-tables`). [Channel messaging](https://docs.zlink.systems/node/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles the first admission of an Actor, and `Table` represents one table. `Table` is a User Spot created on demand by its table ID. `Player` carries no game state of its own, so a table keeps its seats while Actors move or reconnect. The [Spot guide](https://docs.zlink.systems/node/guide/server/21-spot/) describes registration and calls for these objects, and the [Actor guide](https://docs.zlink.systems/node/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/node/guide/server/23-stream/), which processes packets on client connections, `Session` obtains the Player Actor from `JoinTableRequest`, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/node/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/node/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `EntryJoin` requests deferred admission to the table with `joinSpot(...).defer()`, `Table.onActorJoin` accepts or rejects the seat, and `Player.onJoinCompleted` observes the completion result. [Actor membership](https://docs.zlink.systems/node/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state (`TableState.snapshot`, restored by `TableRelocation`), while Framework transfers workload and routes. `runHost` in `server/shared/hosting.ts` requests public host relocation on the `drain` command and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/node/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinTableRequest`. `Session` checks the first packet name, calls `getOrCreate` for the Player Actor by `playerId`, binds it to the Session, and relays the original packet.
2. A new Actor starts in the Entry Spot `Entry`. The `EntryJoin` handler checks the identity, calls `getOrCreate` for the Table by `tableId`, and requests `joinSpot(...).defer()`.
3. `Table.onActorJoin` seats the player through `TableState.admit`. When the table is full, `Player.onJoinCompleted` sends `ActionRejected` through the bound Session. After admission, `Table.onJoinedActor` publishes `TableView` to every seat by player ID, and the `Deliver` handler forwards it through each bound Session.
4. `Start` and `Bet` call `TableState.start` and `TableState.bet`. A valid action publishes `TableView`, `HandDealt`, or `HandResult` to the seats; an invalid action sends `ActionRejected` to the requester through its bound Session.
5. After a reconnect, the new Session sends `JoinTableRequest` again. The `Rejoin` handler sends the table view and the player's cards directly through the bound Session.
6. To replace a node, the `drain` command in `runHost` requests relocation. Framework takes the game state from `capture` in `TableRelocation` and calls `restore` on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the Node.js starter: choose Node.js in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/node/templates/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `board-tables`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature                   | File in this sample                                                                                                                                                                                                                   | What to transfer                                                                                                                                                                                                                                 | What to grow in the starter alongside                                                                                                                                                                                                      |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Typed messages            | `server/shared/messages.ts`                                                                                                                                                                                                           | The classes of the features you adopt: `JoinTableRequest`, `TableViewRequest`, `StartHandRequest`, `BetRequest`, `TableView`, `HandDealt`, `HandResult`, `ActionRejected`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The packet names in `PacketNames`, the registration names in `Names`, and the rejection reasons in `Reasons` in `server/shared/constants.ts`                                                                                               |
| Actor                     | `server/table/infrastructure/player.ts`                                                                                                                                                                                               | `Player` with its join-completion handling, `PlayerFactory`, and `PlayerRelocation`, which uses an empty payload because `Player` has no application state                                                                                       | A new `server/table/` project with `main.ts`, its `server/config/` files, and the Actor factory registration; a `table` entry in `NodeEntrypoints` in `server/shared/constants.ts`; the structure check                                    |
| Spot                      | `server/table/infrastructure/table.ts`, `server/table/domain/table-state.ts`                                                                                                                                                          | `Table` with admission, view, and publish, and `TableState` with seats, chips, deck, turn, and settlement                                                                                                                                        | The Spot factory registration in `server/table/main.ts`; the `spots` and `nodes` declarations in `zlink.structure.json`, and the structure check, which asserts zero Spots in the starter, extended to compare them with the registrations |
| Entry and first admission | `Entry` in `server/table/infrastructure/table.ts`; `EntryJoin` and `Rejoin` in `server/table/infrastructure/handlers.ts`                                                                                                              | `EntryJoin`: the identity check, `getOrCreate` of the dynamic Table, and the deferred join; `Rejoin`: the identity check and the state resend after reconnection                                                                                 | The Entry Spot registration and its `entry` declaration, and the dynamic `table` Spot declaration; a `table` scenario and a `reconnect` scenario that compares state after reconnection                                                    |
| Session binding           | `server/session/session.ts`                                                                                                                                                                                                           | The first-packet check, then find or create the Actor, bind, and relay the original typed packet, and the `SessionInfo` reply                                                                                                                    | The same first-packet name as `EntryJoin`; transfer it into the starter's `session.ts`                                                                                                                                                     |
| Host relocation           | `TableRelocation` in `server/table/infrastructure/table.ts`, `PlayerRelocation` in `server/table/infrastructure/player.ts`, `Control` in `server/shared/constants.ts` and the control loop in `runHost` of `server/shared/hosting.ts` | The snapshot adapters with `capture` and `restore`; the starter already has `drain` and `shutdown`                                                                                                                                               | The `preserveStateWith` registrations; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node hosts a new table                                                         |

The starter's `Dynamic dungeon` feature is the creation of a User Spot on demand. In this sample the same pattern creates the `Table` inside `EntryJoin`, so it is part of the `Entry and first admission` row.

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the `Scenarios` list in `server/shared/constants.ts`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the classes in `messages.ts` first, then move each handler that fails to compile to the new messages. The build generates packet schemas from `server/shared`, so messages defined elsewhere are not registered. When you change the first-entry packet, update the first-packet check in `Session`, `EntryJoin`, and `Rejoin` to receive the same packet.

Keep each piece of state in the object that decides it. Table state belongs in `TableState`, and `Player` keeps none. When you change state fields, update `TableSnapshot` and the `TableRelocation` adapter together. A missing field disappears after relocation, and the `update` scenario detects the loss.

Add a Spot type, its registration, and its declaration in the same change: the factory registration in `server/table/main.ts` and the `spots` declaration in `zlink.structure.json`. If they disagree, the structure check fails.

To name roles such as Table differently from this sample, change the class names, `Names`, `server/config/`, and `zlink.structure.json` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use public npm packages at the versions pinned in `package.json`, and keep imports between this project's own modules relative. Process typed messages with the default Framework JSON serializer so processes use the same contract; the build generates the packet schemas and `server/shared/hosting.ts` loads them. Limit direct serialization in relocation adapters to application snapshots, keeping management of Framework execution state separate from game state.

Manage each game fact, such as a seat's chips, in the object responsible for changing it; here that is the Table. Store player IDs in the Table rather than Actor references, because host relocation does not repeat `onJoinedActor` on the target. Reply to the requesting Actor through the handler's bound Session, and push to the other seats by addressing the Actor by player ID. `SessionInfo` is the source of Actor identity. The playerId in `JoinTableRequest` is an e2e input; validate the identity in `Session` when you add authentication. Node.js typed handlers currently receive the fields without the declared class (#1731), so `Deliver` restores the class from a packet-name table before forwarding; keep that table in step with the pushes you add. Keep the Message Follow duration and the derived `Limits.safeMs` observation deadline in `server/shared/constants.ts`.

### Asynchronous execution and lifecycle

Await public asynchronous calls as the template does, and do not block handler execution with synchronous I/O or busy loops. Since `SpotWide` limits execution over the same state, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/node/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases. Apply the User Spot contract to `Table`, which is created on demand by its ID; [Activation and lifetime](https://docs.zlink.systems/node/guide/server/34-activation-lifetime/) explains creation and initialization. Use `joinSpot(...).defer()` and the public completion callback to take a seat, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using the existing seat. Framework manages current placement, so do not add an application route cache or reset the table during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/node/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `dist/`, and `node_modules/` from commits.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains effective settings written by `e2e/runner.ts` at node startup and logs for each node.

Settings files are `session.json`, `tableA.json`, and `tableB.json`. `NodeProcess` in `e2e/runner.ts` records diagnostic stderr in `<node>-<n>.err.log` and readiness/control stdout in `<node>-<n>.out.log`, where `<n>` counts the processes started in the run; a replaced node has a second pair with a later `<n>`. The client packets each scenario received are in `<scenario>-<suffix>.jsonl`. Docker command logs of the run are `docker-<label>.log`, and local build output is in `.logs/build.log`. The shell runner writes Docker build, Redis, and outer cleanup logs to `.logs/docker-<project>/`.

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
