# MyGame development guide

## Project overview

This C++20 project is an MORPG example with shared fields on several nodes and party dungeons. It runs a Session gateway for connections, Field nodes A and B that host four Fields with border visibility and dynamic Party Spots, Instance nodes A and B that host party Dungeons, and Redis for location and relocation state. A Party Spot is independent of Fields and its ID is the party ID. `bootstrap.cmake` installs the ZLink Framework prebuilt of the version named by its `ZLINK_FRAMEWORK_CPP_VERSION`, the only place that names the Framework version; no package manager runs.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [C++ guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the Framework version pinned in `bootstrap.cmake`.

## Development commands

Default builds and e2e run the nodes and the runner in Docker without a host toolchain. Run these commands from this directory.

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
./run-e2e.sh --check-domain
```

For host process debugging, install a C++20 compiler (GCC 13 or later, or Visual Studio 2022 17.4 or later in a Visual Studio developer shell on Windows), CMake 3.24 or later, and Ninja, then add `--local` or `-Local`. Local mode runs `cmake -P bootstrap.cmake`, `cmake --preset default` and `cmake --build --preset default`, then starts `build/bin/my-game-e2e`. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README. Domain rule tests (`game-domain`, built from `tests/domain.cpp`) are built with the project; after a local build, run `ctest --test-dir build --output-on-failure`.

## Project layout and extension points

### Session gateway

`server/session/main.cpp` registers the STREAM listener and `Session`, and `server/session/session.hpp` handles connection callbacks and forwards incoming packets in `Session::on_packet`. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player. `Session` answers `SessionInfoRequest` with a `SessionInfo` push built from the bound Actor, because a C++ Session cannot yet send a typed reply that it creates (see Known issues in the README).

### Fields, parties and players

An Actor is a game state object addressed by ID; `Player` in `server/shared/player.hpp` hosts the player's `PlayerState`, which retains accepted coordinates, movement sequence, and Party ID. `FieldPositionDomain` in `server/field/domain.hpp` owns movement decisions, arrival validation, and remaining movement budget. `WorldEntry` in `server/shared/entry.hpp` handles the first packet, and both Field and Instance nodes register it together with the `Player` factory. A Spot is a state object that accepts Actors and executes shared game rules; `Field` in `server/field/field.hpp` manages field visibility and movement observations, while `PartySpot` in `server/field/party.hpp` owns party membership and run progress. Visibility values are copies of Actor movement state used for observation, so apply accepted player state changes to the Actor.

`server/field/main.cpp` registers `WorldEntry`, the `Player` factory, the dynamic `PartySpot` factory, and the `Field` factory with the `FieldCapacity` stable type limit. `NodeControl` creates the fields listed in `FieldIds` at startup. Add handlers in `Field::configure()` with `add_actor_send`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/instance/main.cpp` registers the Player factory, `WorldEntry`, and the `Dungeon` factory with the `DungeonCapacity` stable type limit. `Dungeon` in `server/instance/dungeon.hpp` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract. `DungeonRun` in `server/instance/domain.hpp` owns the roster, run phase, and stage; it retains the owning Party Spot ID so the Dungeon can report member entry, start, and completion. `PartyRun` in `server/field/party-domain.hpp` owns the matching party phase and server-issued Run ID. Add encounters and rewards in this project.

### Shared contracts and hosting

`server/shared/contracts.hpp` defines the `Names`, `Limits`, `Control`, and `Rejection` constants and the messages shared by processes, while `server/shared/config.hpp` defines `NodeConfig`, the process settings loaded from `server/config/`. `server/shared/hosting.hpp` configures hosting and diagnostics in `configure_host`, and `NodeControl` in the same file handles runtime control commands. Add contracts used by several processes to `contracts.hpp`, and keep player state in `PlayerState` so separate processes do not independently modify the same fact.

### Build and e2e runner

`CMakeLists.txt` builds one executable per role (`my-game-session`, `my-game-field`, `my-game-instance`) from `server/<role>/main.cpp` and the `my-game-e2e` runner; add a node role to its `foreach(role ...)` list. `e2e/client.hpp` connects through the public stream connector, and `e2e/scenarios.hpp` checks results observable by the client. `e2e/world.hpp` starts the nodes and infrastructure through `e2e/process.hpp`, `e2e/main.cpp` selects and runs scenarios, and `e2e/structure.hpp` validates `zlink.structure.json` against `server/config/` and the names in `contracts.hpp`. Verify new gameplay through received client results; movement, party, and dungeon boundary cases are covered by `tests/domain.cpp` and can also run through `./run-e2e.sh --check-domain`. The project has no formatter configuration.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names::mesh` (`my-game-world`) through `add_route_mesh`. Channels distinguish messages by purpose and name, and the `border` channel (`Names::border`) carries typed publications between Fields. [Channel messaging](https://docs.zlink.systems/cpp/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`WorldEntry` handles Actor entry, `Field` represents a geographical field, `PartySpot` owns a party independently of Field membership, and `Dungeon` represents that run's dynamically created Dungeon. The [Spot guide](https://docs.zlink.systems/cpp/guide/server/21-spot/) describes registration and calls for these objects. `Player` retains movement and Party ID when its host Spot changes; the [Actor guide](https://docs.zlink.systems/cpp/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/cpp/guide/server/23-stream/), which processes packets on client connections, `Session` obtains the Player Actor, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/cpp/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation, configured in `configure_host`. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/cpp/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Player::move` requests deferred admission to the next Field with `join_spot(...).defer()`, `Field::on_actor_join` decides it, and `Player::on_join_completed` observes the completion result. `Field::travel` does the same for a Dungeon, which admits only members of the prepared party. [Actor membership](https://docs.zlink.systems/cpp/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `NodeControl` requests public host relocation and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/cpp/guide/server/37-relocation/) explains the procedure.

### Border notifications and timers

A timer runs a handler periodically; `BorderTick` is registered in `Field::on_initialize()` with `add_timer` and publishes information about players near a border through the Spot publisher client. In this pub/sub exchange, `Field::border` receives the subscribed information (registered in `Field::configure()`), and `Field::deliver` forwards `BorderVisible` through the receiving Actor's bound Session. [Timers and workers](https://docs.zlink.systems/cpp/guide/server/36-timer-worker/) explains registration and execution of periodic work.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client connects to the Session node over STREAM and sends the first packet, `JoinWorldRequest`. `Session::on_packet` checks the first packet name, calls `get_or_create` for the Player Actor by `playerId`, binds it with `bind_or_get`, and relays the original packet.
2. A new Actor starts in the Entry Spot `WorldEntry`. `WorldEntry::enter` checks the identity and calls `Player::move`, which requests `join_spot(...).defer()` for the Field that covers the coordinate (`Names::field_id`).
3. `Field::on_actor_join` accepts the arrival and `Field::on_actor_joined` applies it to the Actor. When the join completes, `Player::on_join_completed` sends `WorldState` through the bound Session. Later packets from the same client are handled by the handlers of the Field that holds the Actor (such as `Field::move`), which send results through the same Session. When movement reaches another Field, `Field::move` calls `Player::move` again.
4. Every `Names::tick_period`, `BorderTick` publishes the players near a border on the `border` channel. The adjacent Field's `Field::border` sends `BorderVisible` to the nearby players through the Actor client, and `Field::deliver` forwards it to the bound Session.
5. `CreatePartyRequest` and `JoinPartyRequest` are handled by `Field::create_party` and `Field::join_party`. Field issues a Party ID, creates or finds that Party Spot, and forwards `PartyCommand` with `request_to_spot`. `PartySpot` rejects invalid requests and broadcasts the current `PartyView` on success. Leaving a Field removes only its visibility and movement projection.
6. `EnterDungeonRequest` is handled by `Field::enter_dungeon`, which forwards the request to the player's Party Spot. The Party Spot issues a Run ID, creates the Dungeon, and sends `PrepareDungeon` with an idempotent `request_to_spot`; it then sends `JoinDungeon` to members who have not entered. Each member's `Field::travel` requests `join_spot(...).defer()` for the Dungeon. `Dungeon::on_actor_join` checks the run ID and roster, and `on_actor_joined` reports member entry and run start to the Party Spot. `Dungeon::advance` reports completion and sends the current view to every member.
7. To replace a node, `drain` in `NodeControl` requests relocation. Framework takes the game state from `capture` in `PlayerRelocation`, `FieldRelocation`, `PartyRelocation`, and `DungeonRelocation` and calls `restore` on another node.

## Extending the template domain

Extend this template's domain to build a MORPG server. Run the README build and e2e commands before adding features.

1. Extend authoritative movement decisions in `server/field/domain.hpp`, Party Spot transitions in `server/field/party-domain.hpp`, and dungeon rules in `server/instance/domain.hpp`. Keep one owner for each state change.
2. Add shared contracts in `server/shared/contracts.hpp`. The server validates inputs and calculates results. Rejected inputs must preserve values, sequences, and cooldowns.
3. Add only client packets to the Session allowlist in `server/session/session.hpp`. Keep internal messages and result packets outside that list.
4. Add received-result scenarios in `e2e/scenarios.hpp` and boundary cases in `tests/domain_checks.hpp`. Update the README scenario table and `run.scenarios` in `zlink.structure.json` together. For time-limited rejection, send the accepted request and the request expected to be rejected before waiting for either reply, then wait only for the server's `RetryAfterMs`.
5. Update the owning relocation snapshot and boundary checks when adding state. Store remaining milliseconds rather than process timestamps.

## Development considerations

### State and message contracts

Use the public Framework prebuilt that `bootstrap.cmake` installs at the pinned version, and add no repository sources or local feeds to the project. Process typed messages with the default Framework JSON serializer so processes use the same contract: declare each message in `server/shared/contracts.hpp` with a `packet_name` and JSON conversion, and add no per-message codecs, manual encoding, or raw frame parsing. Limit direct serialization in relocation adapters to application state (`zlink::message_t::from_json` in `capture`, `parse_json` in `restore`), keeping management of Framework execution state separate from game state.

Manage each game fact in its owner. Store player IDs and observational copies in Spots, and never retain Actor references: a Spot handler receives the Actor as an argument and reaches other players through `actor_client_t` by player ID. Sessions forward packets after authentication and binding. `FieldPositionDomain` decides movement, `PlayerState` retains accepted values and Party ID, `PartySpot` decides party membership and run phase, and `DungeonRun` decides the dungeon roster, phase, and stage. Field departure removes visibility only and leaves Party Spot state unchanged. Keep these rules in `server/field/domain.hpp`, `server/field/party-domain.hpp`, `server/instance/domain.hpp`, and `server/shared/player-state.hpp`, and keep Framework callbacks in the Spot, Actor, and Session files.

### Asynchronous execution and lifecycle

Write handlers as `task_t` coroutines and `co_await` public asynchronous calls as the template does; the [Host execution model](https://docs.zlink.systems/cpp/guide/server/43-execution-model/) reserves blocking waits such as `.result()` for tests and clients. Do not block a handler with waits or synchronous I/O. `NodeControl` waits with `await_host` only on its own stdin thread. Since the SpotWide execution mode limits execution over the same state to one gate, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/cpp/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases: factories and Entry Spots are registered in each node's `main.cpp`, handlers in `configure()`, timers in `on_initialize()`, and admission and membership decisions in `on_actor_join`, `on_actor_joined`, `on_leave_actor` and `on_disconnect_actor`. Apply the User Spot contract to `Dungeon` even though its process is named Instance. Use `join_spot(...).defer()` and the public completion callback `on_join_completed` for membership changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. [Activation and lifetime](https://docs.zlink.systems/cpp/guide/server/34-activation-lifetime/) explains creation and initialization.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state: `Session::on_packet` binds with `bind_or_get`, and `Field::reconnect` and `Dungeon::reconnect` re-send the current state for the replayed first packet. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/cpp/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, and `.zlink/` from commits. Run formatters and installation commands only from this project directory, with explicit file paths that exclude those directories.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains the effective settings that `e2e/world.hpp` writes at node startup and the logs for each node. Each selected scenario starts its own nodes and writes to its own `.logs/run-<id>/` directory.

Settings files are `fieldA.json`, `fieldB.json`, `instanceA.json`, `instanceB.json`, and `session.json`; a replaced Instance node uses its file name with the suffix `-replacement`. `e2e/process.hpp` records diagnostic stderr in `<node>.err.log` and readiness/control stdout in `<node>.out.log`, where `<node>` is the settings file name without `.json`. Helper commands (Compose and Docker CLI calls) write `command-<n>.out.log` and `command-<n>.err.log`. Local build output is in `.logs/build.log`. Docker build and outer cleanup logs are in `.logs/docker-<project>/`: `build.log`, `redis-start.log`, `redis.log`, `cleanup.log`, and `containers-stop.log` from the PowerShell runner.

### Diagnostic levels

`configure_host` in `server/shared/hosting.hpp` enables message flow, which records the stages from message receipt to completion, with this setting.

```cpp
options.configure_dispatch ().message_flow (fx::message_flow_log_mode_t::normal);
```

`normal` records receive, dispatch, and completion stages and errors. Use `detailed` for timing detail, and chain `.include_message_sizes (true)` on the same dispatch options for size recording. Detailed recording adds cost, so use it for the required investigation scope.

`NodeConfig` in `server/shared/config.hpp` defines no diagnostics JSON key, so change the `configure_host` call, rebuild, and restart to apply startup settings. The host `app_t` exposes `set_message_flow_mode_async` for live changes, but this project's stdin controls do not expose it. The synchronous `set_message_flow_mode` blocks, so use the coroutine form inside Framework handlers and callbacks. [Monitoring](https://docs.zlink.systems/cpp/guide/server/26-monitoring/) and [Observability diagnostics](https://docs.zlink.systems/cpp/reference/08-observability-diagnostics/) describe diagnostic records and settings.

### Message flow investigation

`configure_host` also installs a logging sink that writes each Framework log record as one JSON object per line to stderr, so inspect message flow in the node `.err.log` files; the template creates no separate flow file. Message flow records have `"event_id":"zlink.message_flow"` with `phase`, `outcome` and `packet`. The `flow` value identifies a message across nodes and `corr` relates request and reply records. Dispatch failures have `"event_id":"zlink.dispatch_error"`, `"outcome":"failed"`, `reason`, `error_type` and `error_message`.

Read the failure fields and preceding stage together without filtering categories out. Flow IDs differ between runs, so compare passing and failing runs by logical operation, Actor ID, and stage order to find the first missing or failed stage. If startup fails before message flow recording, preserve the startup exception text that the node writes to stderr.

### Debugger and cleanup

In local mode the runner starts `build/bin/my-game-<role>` processes with the settings files above, so attach a debugger to the spawned node process. Docker nodes do not inherit host environment variables. Let the runner clean up its own processes and Compose project, preserve cleanup failures, and avoid stopping unrelated processes or containers. A cleanup failure makes the run fail with exit code 1.

## Reporting ZLink defects

### Identifying a defect

A ZLink defect is a case where a public API called as documented produces a result that differs from the documentation. First compare your API usage with the public guides and samples linked above to rule out an application usage error. If the usage is correct, report an issue instead of adding a workaround, and keep the scenario assertions unchanged. A workaround hides the defect and stays in the code after you upgrade to a fixed package, where it changes behavior.

### Checking existing issues

Before filing, search both open and closed [ZLink issues](https://github.com/zlink-systems/zlink/issues?q=is%3Aissue) by error message, exception type, and API name. If the same defect is already reported, add your environment and logs to that issue instead of opening a new one. If it matches a closed issue, find the package version that contains the fix in that issue, raise `ZLINK_FRAMEWORK_CPP_VERSION` in `bootstrap.cmake`, and run again.

### Issue body

The body follows the issue template of the ZLink repository. Copy the following form and fill it in.

```markdown
# Issue description
<scenario and the public operation that fails>

# Environment
* zlink version: <ZLINK_FRAMEWORK_CPP_VERSION from bootstrap.cmake>
* OS: <OS, compiler and CMake versions>

# Minimal test code / Steps to reproduce the issue
1. <commands or minimal code using public APIs only>

# What's the actual result? (include assertion message & call stack if applicable)
<first failing node-log excerpt, error text, flow/corr records>

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

Put the language and the failing behavior in the title (for example, `framework(cpp): Spot startup fails with empty SpotId`). Record the filed issue number in the README Known issues so the project keeps the place to re-verify when a fixed package is released.
