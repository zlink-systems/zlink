# MyGame development guide

## Project overview

This C++20 project is an MORPG example with shared fields on several nodes and party dungeons. It runs a Session gateway for connections, Field nodes A and B that host four Fields with border visibility, Instance nodes A and B that host party Dungeons, and Redis for location and relocation state. `bootstrap.cmake` installs the ZLink Framework prebuilt of the version named by its `ZLINK_FRAMEWORK_CPP_VERSION`, the only place that names the Framework version; no package manager runs.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [C++ guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the Framework version pinned in `bootstrap.cmake`.

## Development commands

Default builds and e2e run the nodes and the runner in Docker without a host toolchain. Run these commands from this directory.

```powershell
pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly
pwsh -NoProfile -File ./run-e2e.ps1
pwsh -NoProfile -File ./run-e2e.ps1 -Scenario field,dungeon,reconnect,update
pwsh -NoProfile -File ./run-e2e.ps1 --check-structure
```

Bash provides the same workflow:

```bash
./run-e2e.sh --build-only
./run-e2e.sh
./run-e2e.sh --scenario field,dungeon,reconnect,update
./run-e2e.sh --check-structure
```

For host process debugging, install a C++20 compiler (GCC 13 or later, or Visual Studio 2022 17.4 or later in a Visual Studio developer shell on Windows), CMake 3.24 or later, and Ninja, then add `--local` or `-Local`. Local mode runs `cmake -P bootstrap.cmake`, `cmake --preset default` and `cmake --build --preset default`, then starts `build/bin/my-game-e2e`. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README. Domain rule tests (`game-domain`, built from `tests/domain.cpp`) are built with the project; after a local build, run `ctest --test-dir build --output-on-failure`.

## Project layout and extension points

### Session gateway

`server/session/main.cpp` registers the STREAM listener and `Session`, and `server/session/session.hpp` handles connection callbacks and forwards incoming packets in `Session::on_packet`. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player. `Session` answers `SessionInfoRequest` with a `SessionInfo` push built from the bound Actor, because a C++ Session cannot yet send a typed reply that it creates (see Known issues in the README).

### Fields, parties and players

An Actor is a game state object addressed by ID; `Player` in `server/shared/player.hpp` hosts the player's `PlayerState`, whose coordinate and movement rules are in `server/shared/player-state.hpp`. `WorldEntry` in `server/shared/entry.hpp` handles the first packet, and both Field and Instance nodes register it together with the `Player` factory. A Spot is a state object that accepts Actors and executes shared game rules; `Field` in `server/field/field.hpp` manages field visibility, pending arrivals, and parties in `FieldState` from `server/field/domain.hpp`. Visibility values are copies of Actor movement state used for observation, so apply player state changes to the Actor.

`server/field/main.cpp` registers `WorldEntry`, the `Player` factory, and the `Field` factory with the `FieldCapacity` stable type limit, and `NodeControl` creates the fields listed in `FieldIds` at startup. Add handlers in `Field::configure()` with `add_actor_send`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Dungeons

`server/instance/main.cpp` registers the Player factory, `WorldEntry`, and the `Dungeon` factory with the `DungeonCapacity` stable type limit. `Dungeon` in `server/instance/dungeon.hpp` is an application-created User Spot, distinct from the Framework Instance Spot with its separate creation and initialization contract. It owns the party roster and stage in `DungeonState` from `server/instance/domain.hpp`. Add encounters, rewards, and Dungeon lifecycle behavior in this project.

### Shared contracts and hosting

`server/shared/contracts.hpp` defines the `Names`, `Limits`, `Control`, and `Rejection` constants and the messages shared by processes, while `server/shared/config.hpp` defines `NodeConfig`, the process settings loaded from `server/config/`. `server/shared/hosting.hpp` configures hosting and diagnostics in `configure_host`, and `NodeControl` in the same file handles runtime control commands. Add contracts used by several processes to `contracts.hpp`, and keep player state in `PlayerState` so separate processes do not independently modify the same fact.

### Build and e2e runner

`CMakeLists.txt` builds one executable per role (`my-game-session`, `my-game-field`, `my-game-instance`) from `server/<role>/main.cpp` and the `my-game-e2e` runner; add a node role to its `foreach(role ...)` list. `e2e/client.hpp` connects through the public stream connector, and `e2e/scenarios.hpp` checks results observable by the client. `e2e/world.hpp` starts the nodes and infrastructure through `e2e/process.hpp`, `e2e/main.cpp` selects and runs scenarios, and `e2e/structure.hpp` validates `zlink.structure.json` against `server/config/` and the names in `contracts.hpp`. Verify new gameplay through received client results; the movement, party, and dungeon rules are covered by the separate unit test target `my-game-domain-tests` from `tests/domain.cpp`. The project has no formatter configuration.

## ZLink concepts in this project

### RouteMesh and channels

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names::mesh` (`my-game-world`) through `add_route_mesh`. Channels distinguish messages by purpose and name, and the `border` channel (`Names::border`) carries typed publications between Fields. [Channel messaging](https://docs.zlink.systems/cpp/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`WorldEntry` handles Actor entry, `Field` represents a geographical field with its parties, and `Dungeon` represents a dynamically created party Dungeon. The [Spot guide](https://docs.zlink.systems/cpp/guide/server/21-spot/) describes registration and calls for these objects. `Player` retains movement state when its host Spot changes; the [Actor guide](https://docs.zlink.systems/cpp/guide/server/22-actor/) describes Actor creation and calls.

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
5. `CreatePartyRequest` and `JoinPartyRequest` are handled by `Field::create_party` and `Field::join_party`, which apply `FieldState::party` and send the updated `FieldView`, or answer `ActionRejected`.
6. `EnterDungeonRequest` is handled by `Field::enter_dungeon`. For the party led by the requester, it calls `get_or_create` for the Dungeon, asks it with `request_to_spot` and `PrepareDungeon`, and sends `JoinDungeon` to every member through the Actor client. Each member's `Field::travel` requests `join_spot(...).defer()` for the Dungeon; `Dungeon::on_actor_join` admits the members, and `Dungeon::advance` raises the stage and sends the new view to every member.
7. To replace a node, `drain` in `NodeControl` requests relocation. Framework takes the game state from `capture` in `PlayerRelocation`, `FieldRelocation`, and `DungeonRelocation` and calls `restore` on another node.

## Developing from this sample

This sample shows how the features work together, so read and run it rather than turning it into your game. Start your own project from the C++ starter: choose C++ in the Initializer at [ZLink Start](https://zlink.systems/start), or use `framework/languages/cpp/templates/starter/my-game` in the ZLink repository. Then transfer the features you need from this sample one at a time. The starter's `AGENTS.md` owns the choice between structures; this section covers only `morpg-fields-instances`. Keep this sample unchanged next to your project as a reference.

### Run the sample first

Before transferring anything, run the build and e2e once with the [Development commands](#development-commands) to confirm the downloaded sample works and to see its behavior. This result is the baseline for comparison. If it fails, compare the failure with Known issues in the [README](README.md) first.

### Features to transfer

Feature names match the feature table in the starter's `AGENTS.md`, except Party, which only this structure has. Paths are relative to this directory. The starter has one Session node and the `connect` scenario only, so the last column lists what grows in the starter with each feature.

| Feature | File in this sample | What to transfer | What to grow in the starter alongside |
| --- | --- | --- | --- |
| Typed messages | `server/shared/contracts.hpp` | The structs of the features you adopt: `JoinWorldRequest`, `MoveRequest`, `EnterField`, `WorldState`, `BorderPresence`, `BorderVisible`, `CreatePartyRequest`, `JoinPartyRequest`, `EnterDungeonRequest`, `PrepareDungeon`, `DungeonPrepared`, `JoinDungeon`, `AdvanceRequest`, `ViewRequest`, `FieldView`, `DungeonView`, `ActionRejected`; `SessionInfoRequest` and `SessionInfo` only if your client needs them | The `Names` and `Rejection` constants that the messages refer to |
| Actor | `Player`, `PlayerFactory` in `server/shared/player.hpp`, `PlayerState` in `server/shared/player-state.hpp` | `Player` with `PlayerState` (coordinate, `sequence`, `validate`, `apply`), `move`, and `send_state` | A new `server/field/` role with its `main.cpp`, `server/config/` files, and an entry in the `CMakeLists.txt` role list; the Actor factory registration in `main.cpp`; the `nodes` declaration in `zlink.structure.json` and the structure check in `e2e/structure.hpp` |
| Spot | `Field` in `server/field/field.hpp`, `FieldState` in `server/field/domain.hpp` | `Field` with `FieldState` (visibility, pending arrivals, parties) and the `deliver` forwarders | The Spot factory registration in `main.cpp` with `FieldCapacity`; the `FieldIds` setting and the `spots` declarations in `zlink.structure.json` that the structure check compares |
| Party | `Field::create_party`, `Field::join_party` in `server/field/field.hpp`, `FieldState::party` in `server/field/domain.hpp` | The party rules (one party per player, a size limit, leader entry) and the handlers that apply them and answer `ActionRejected` | The `Names::maximum_party_members` constant and the `Rejection` strings; the `dungeon` scenario, which creates and joins a party |
| Entry and first admission | `WorldEntry` in `server/shared/entry.hpp`, `Field::reconnect` in `server/field/field.hpp` | `WorldEntry::enter` (identity check, first move to the Field of the coordinate) and the identity check and state resend of `Field::reconnect` | The `add_entry_spot` registration and the `entry` declarations; a first-entry scenario (`field`) and a `reconnect` scenario that compares state after reconnection |
| Session binding | `server/session/session.hpp` | The first-packet check, then find or create the Actor, `bind_or_get`, and relay the original typed packet | The same first-packet type as `WorldEntry::enter`; transfer it into the starter's `server/session/session.hpp` |
| Host relocation | `PlayerRelocation` in `server/shared/player.hpp`, `FieldRelocation` in `server/field/field.hpp`, `DungeonRelocation` in `server/instance/dungeon.hpp`, `NodeControl` in `server/shared/hosting.hpp` | The adapters with `capture` and `restore` of `PlayerState`, `FieldState`, and `DungeonState`, and the `drain` and `shutdown` controls | The `preserve_state_with` registrations in `main.cpp`; the `update` scenario, which checks that connections and state survive a server replacement and that the replacement node accepts new work |
| Dynamic dungeon | `Field::enter_dungeon`, `Field::travel` in `server/field/field.hpp`, `server/instance/dungeon.hpp`, `server/instance/main.cpp` | The dynamic User Spot creation pattern (`get_or_create`, then `request_to_spot` with `PrepareDungeon`), the member-only admission of `Dungeon`, and `DungeonState` | A new `server/instance/` role with its config (`DungeonCapacity`) and `CMakeLists.txt` entry; the dynamic `dungeon` Spot and the `GetOrCreate` edge in `zlink.structure.json` |
| Channel pub/sub and seamless movement | `Field::configure()`, `Field::publish_border`, `Field::border`, `Field::deliver`, and `BorderTick` in `server/field/field.hpp` | The border subscription and publication, and the delivery to the receiving Actor's bound Session | The `border` channel registration in the node `main.cpp` files and its `channels` declaration in `zlink.structure.json`; a scenario that checks a border publication (`field`) |

`drain` and `shutdown` are transferred together with `NodeControl`.

### Transfer order and rules

Transfer one row at a time and run the build, the structure check, and the related e2e scenario after each. Add a scenario ID to the `Scenario` enum and `scenario_order` in `e2e/structure.hpp`, to the `switch` and the usage message in `e2e/main.cpp`, to `run.scenarios`, and to the selection examples in `run.commands` of `zlink.structure.json` together. Judge results by the replies and events actually received; do not make tests pass with fixed sleeps or longer deadlines.

Start with the message contracts. Define the packet structs in `server/shared/contracts.hpp` first, each with a `packet_name` and its JSON conversion, then move each handler that fails to compile to the new messages. When you change the first-entry packet, update the first-packet check in `Session::on_packet` in `server/session/session.hpp` and `WorldEntry::enter`, `Field::reconnect`, and `Dungeon::reconnect` to receive the same type.

Keep each piece of state in the object that decides it. Per-player state belongs in `Player`; state shared by several players belongs in a Spot. When you change state fields, update that object's state struct with its JSON conversion and the `capture` and `restore` of its relocation adapter together. A missing field disappears after relocation, and the `update` scenario detects the loss.

Add a channel, its subscription, and its declaration in the same change: the subscription in `Field::configure()`, the channel registration in the Field `main.cpp`, and the `channels` declaration in `zlink.structure.json`. If they disagree, the structure check fails before startup. The subscription calls `add_subscribe(channel, topic)` through `subscribe_border`; when the installed Framework lacks that call, Field startup fails with an explicit message instead of switching the subscription to `Names::mesh` (see Known issues in the README).

Declare one Entry in `zlink.structure.json` for each node that registers `WorldEntry` (the Field and Instance nodes), and keep the dynamic Dungeon declarations complete for every Instance. Both Instance nodes set `DungeonCapacity` to 1 in the e2e configuration, so a replacement must accept a fresh Dungeon while the surviving node holds the relocated one; placement itself remains a Framework decision.

To name roles such as Field or Dungeon differently from this sample, change the type names, `Names`, `server/config/`, `zlink.structure.json`, and the role list in `CMakeLists.txt` at once. Do the renaming in its own change, separate from transferring a feature, so failures are easier to trace.

If `update` fails after a transfer, first check whether the snapshot is missing any state, then use the message flow in [Debugging](#debugging) to find the first failing step.

## Development considerations

### State and message contracts

Use the public Framework prebuilt that `bootstrap.cmake` installs at the pinned version, and add no repository sources or local feeds to the project. Process typed messages with the default Framework JSON serializer so processes use the same contract: declare each message in `server/shared/contracts.hpp` with a `packet_name` and JSON conversion, and add no per-message codecs, manual encoding, or raw frame parsing. Limit direct serialization in relocation adapters to application state (`zlink::message_t::from_json` in `capture`, `parse_json` in `restore`), keeping management of Framework execution state separate from game state.

Manage each game fact, such as a player's coordinate, in the object responsible for changing it. Store player IDs and observational copies in Spots, and never retain Actor references: a Spot handler receives the Actor as an argument and reaches other players through `actor_client_t` by player ID. Sessions forward packets after authentication and binding; movement is decided by the Actor's `PlayerState`, parties and visibility by the `Field`, and dungeon membership and stage by the `Dungeon`, each of which owns that state. Keep game rules in `server/field/domain.hpp`, `server/instance/domain.hpp`, and `server/shared/player-state.hpp`, and keep Framework callbacks in the Spot, Actor, and Session files.

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
