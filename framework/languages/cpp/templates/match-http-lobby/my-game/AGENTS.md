# MyGame development guide

## Project overview

This C++20 project is an arena example in which an HTTP matchmaking lobby pairs two players and ZLink STREAM carries the gameplay. It runs an HTTP Lobby node, a Session gateway for connections, Match nodes A and B that host Room Spots and Player Actors, and Redis for location and relocation state. `bootstrap.cmake` installs the ZLink Framework prebuilt of the version named by its `ZLINK_FRAMEWORK_CPP_VERSION`, the only place that names the Framework version; no package manager runs.

The project layout and feature procedure below describe where to add gameplay. The [README](README.md) covers prerequisites, deployment, and known issues in the public packages. [zlink.structure.json](zlink.structure.json) describes the initial node composition and scenarios; Framework and Redis manage object placement at runtime.

> **Documentation and package versions.** The [ZLink documentation](https://docs.zlink.systems/), [C++ guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/), and [agent index](https://docs.zlink.systems/llms.txt) are the public documentation entry points. When a current guide describes a newer release, check its API against the Framework version pinned in `bootstrap.cmake`.

## Development commands

Default builds and e2e run the nodes and the runner in Docker without a host toolchain. Run these commands from this directory.

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

For host process debugging, install a C++20 compiler (GCC 13 or later, or Visual Studio 2022 17.4 or later in a Visual Studio developer shell on Windows), CMake 3.24 or later, and Ninja, then add `--local` or `-Local`. Local mode runs `cmake -P bootstrap.cmake`, `cmake --preset default` and `cmake --build --preset default`, then starts `build/bin/my-game-e2e`. Add `--no-build` or `-NoBuild` only with an existing local build. Keep canonical scenario order and assertions; compare first failures with Known issues in the README. Room rule tests and the HTTP disconnect/rejoin regression (`room-rules`, built from `e2e/domain_tests.cpp`) are built with the project; after a local build, run `ctest --test-dir build --output-on-failure`.

## Project layout and extension points

### HTTP lobby

`server/lobby/main.cpp` registers the Lobby node as a RouteMesh client and adds `HttpLobby` from `server/lobby/http.hpp` as a hosted service. `HttpLobby` accepts `POST /match` with `MatchRequest` (`playerId` and `mode`), holds the first response until another player requests the same mode, then calls `get_or_create` for a new Room and answers both players with `MatchResponse` (`roomId` and `sessionEndpoint`). The HTTP listener is written with Boost.Beast in `server/lobby/http.hpp` and `server/shared/http.hpp`; the [HTTP hosting guide](https://docs.zlink.systems/cpp/guide/server/42-http-hosting/) describes the Framework's own HTTP surface. The lobby keeps its queue in process memory and is not a relocation participant. Production authentication and durable matchmaking are extension points of this node.

### Session gateway

`server/session/main.cpp` registers the STREAM listener and `Session`, and `server/session/session.hpp` handles connection callbacks and forwards incoming packets in `Session::on_packet`. A Session is a communication object for a connection; Framework provides binding to associate it with a player state object. Add authentication before binding and retain the existing typed packet relay so Framework can identify the connection for replies to that player. `Session` answers `SessionInfoRequest` with a `SessionInfo` push built from the bound Actor, because a C++ Session cannot yet send a typed reply that it creates (see Known issues in the README).

### Rooms and players

An Actor is a game state object addressed by ID; `Player` in `server/match/player.hpp` is a stateless Actor that reports a rejected join to its Session. A Spot is a state object that accepts Actors and executes shared game rules; `Room` in `server/match/room.hpp` owns the combat state in `RoomState` from `server/match/domain.hpp`, which holds the admission rule, closed-wall and range checks, sequence checks, and the winner. Per-player HP and position live in the Room, so apply player state changes to `RoomState`.

`server/match/main.cpp` registers `Entry` (defined in `server/match/player.hpp`), the `Player` factory, and the `Room` factory with the `RoomCapacity` stable type limit. Add handlers that apply combat rules in `Room::configure()` with `add_actor_send`. When changing node composition, update registrations, `server/config/`, and the descriptor together so the structure check validates the same composition.

### Shared contracts and hosting

`server/shared/contracts.hpp` defines the `Names`, `Game`, `Rejection`, `Limits`, and `Control` constants and the messages shared by processes, while `server/shared/config.hpp` defines `NodeConfig`, the process settings loaded from `server/config/`. `server/shared/hosting.hpp` configures hosting and diagnostics in `configure_host`, and `NodeControl` in the same file handles runtime control commands. Add contracts used by several processes to `contracts.hpp`, and keep combat state in `RoomState` so separate processes do not independently modify the same fact.

### Build and e2e runner

`CMakeLists.txt` builds one executable per role (`my-game-session`, `my-game-match`, `my-game-lobby`) from `server/<role>/main.cpp` and the `my-game-e2e` runner; add a node role to its `foreach(role ...)` list. `e2e/client.hpp` connects through the public stream connector, and `e2e/scenarios.hpp` checks results observable by the client. `e2e/world.hpp` starts the nodes and infrastructure through `e2e/process.hpp`, `e2e/main.cpp` selects and runs scenarios, and `e2e/structure.hpp` validates `zlink.structure.json` against `server/config/` and the names in `contracts.hpp`. Verify new gameplay through received client results; the Room rules and the lobby's HTTP handling are covered by the separate unit test target `my-game-rules`. The project has no formatter configuration.

## ZLink concepts in this project

### RouteMesh

A RouteMesh connects nodes that exchange messages between processes; this project uses `Names::mesh` (`my-game-arena`) through `add_route_mesh`. [Channel messaging](https://docs.zlink.systems/cpp/guide/server/20-channel-messaging/) describes registration and calls.

### Spots and Actors

`Entry` handles Actor entry and `Room` represents one dynamically created match room. The [Spot guide](https://docs.zlink.systems/cpp/guide/server/21-spot/) describes registration and calls for these objects. `Player` has no application state of its own, so a player's HP and position stay in the Room when the Actor moves; the [Actor guide](https://docs.zlink.systems/cpp/guide/server/22-actor/) describes Actor creation and calls.

### STREAM and Sessions

With [STREAM](https://docs.zlink.systems/cpp/guide/server/23-stream/), which processes packets on client connections, `Session` obtains the Player Actor, binds it, and forwards the original typed packet. Actor handlers send messages to the client through the bound Session. [Session and Actor](https://docs.zlink.systems/cpp/guide/server/24-actor-session/) describes the relationship between connections and Actors.

### Location Store

The Location Store records an object's current runtime location by ID; this project uses its Redis implementation, configured in `configure_host`. Application code addresses Actors and Spots by ID rather than node address, so calls can use the same ID after placement changes. The [Location guide](https://docs.zlink.systems/cpp/guide/server/25-location/) explains location resolution.

### Actor membership

A membership change moves an Actor from one Spot to another. Admission is the target Spot's decision to accept or reject the membership request. `Entry::join` requests deferred admission to the Room named by `roomId` with `join_spot(...).defer()`, `Room::on_actor_join` decides it through `RoomState::admit`, and `Player::on_join_completed` observes a rejected result. [Actor membership](https://docs.zlink.systems/cpp/guide/server/35-actor-membership/) explains the order of admission and completion.

### Host relocation and drain

Relocation changes an object's execution host; drain moves a host's workload elsewhere so the host can shut down. Application adapters capture and restore game state, while Framework transfers workload and routes. `NodeControl` requests public host relocation and observes safe shutdown; the [Relocation guide](https://docs.zlink.systems/cpp/guide/server/37-relocation/) explains the procedure.

## Request flow

Requests in the example game follow this path. Reading the code along this path before adding or transferring a feature locates the places to change.

1. The client posts `{"playerId":"p1","mode":"duel"}` to `POST /match` on the Lobby node (`HttpLobby::match`). The first response is held until another player requests the same mode.
2. When two players of one mode are waiting, `HttpLobby` generates a UUID Room ID and calls `get_or_create` for it through the Spot manager, then answers both requests with `MatchResponse`, which carries the `roomId` and the `SessionEndpoint` configured in `lobby.json`.
3. Each client connects to that Session endpoint over STREAM and sends the first packet, `JoinRoomRequest`. `Session::on_packet` checks the first packet name, calls `get_or_create` for the Player Actor by `playerId`, binds it with `bind_or_get`, and relays the original packet.
4. A new Actor starts in the Entry Spot `Entry`. `Entry::join` checks the identity and requests `join_spot(...).defer()` for the Room. `Room::on_actor_join` admits the player, and `Room::on_actor_joined` broadcasts `RoomView` to the players through the Actor client; `Room::deliver_view` forwards it to the bound Session.
5. `MoveRequest` and `ShootRequest` are handled by `Room::move` and `Room::shoot`, which apply the `RoomState` rules, answer a rejection with `ActionRejected`, and broadcast the new `RoomView` and, when a player wins, `MatchEnded` (forwarded by `Room::deliver_end`).
6. To replace a node, `drain` in `NodeControl` requests relocation. Framework takes the game state from `capture` in `RoomRelocation` and calls `restore` on another node. `PlayerRelocation` carries no state.

## Extending the game domain

Extend this template's domain to build a two-dimensional action server. Run the [build and e2e commands](#development-commands) before changing it; if the baseline fails, compare the first failure with Known issues in the [README](README.md).

| Extension point | Decision owner | How to extend it |
| --- | --- | --- |
| Game state and action rules | `server/match/domain.hpp`: `RoomState` | Add state and movement or combat decisions here. Rejected actions return `ActionRejected` and leave position, HP, sequence, and cooldown unchanged. |
| Room membership and admission | `server/match/domain.hpp`: `RoomRoster`; `server/match/room.hpp`: `Room::prepare` and `on_actor_join` | Add the approved member list through the internal preparation request. `RoomRoster` decides admission. |
| Typed contracts | `server/shared/contracts.hpp` | Add shared packet structs, `packet_name`, JSON fields, and their constants in this file. Keep numeric limits in the same contract owner. |
| Client packet allowlist | `server/session/session.hpp`: `Session::client_packet_allowed` | Add only packets the client is allowed to send. Internal messages such as `PrepareRoom` stay outside this list. |
| Domain boundary tests | `e2e/domain_tests.cpp` | Add one check for each new rule and its rejected-state invariants. |
| Client scenarios | `e2e/scenarios.hpp`, `e2e/structure.hpp`, `e2e/main.cpp` | Add the observable scenario and register it in the enum/order, runner switch, `run.scenarios`, and selection examples in `run.commands`. The lobby scenario rejects an outsider Join against the empty prepared Room before either matched player enters, then checks member admission and client `PrepareRoom` rejection. Keep the README scenario table in sync. |

Keep each fact in its owner. `RoomState` owns positions, HP, accepted action sequence, phase, winner, and monotonic cooldown state. `RoomRoster` owns the prepared member list and admission decision. `HttpLobby` owns the temporary match queue and returns an assignment only after the Room confirms preparation. A relocation snapshot contains `Game` and `Roster`, with cooldowns represented as remaining milliseconds.

When a contract or scenario changes, update all of its C++ declarations and observations together. Judge results by replies and events actually received; use only the server-provided `RetryAfterMs` when a retry is needed. Do not add fixed waits or extend deadlines. If `update` loses state, inspect the Room snapshot and then use the message flow in [Debugging](#debugging) to find the first missing transition.

Configure `HttpEndpoint` with port 0 and read the bound port from the lobby's `READY` line. Set `SessionEndpoint` to the endpoint published by the Session node; `e2e/world.hpp` does both for each run. If node roles or composition change, update the type registrations, `server/config/`, `zlink.structure.json`, `CMakeLists.txt`, and the structure check together.

## Development considerations

### State and message contracts

Use the public Framework prebuilt that `bootstrap.cmake` installs at the pinned version, and add no repository sources or local feeds to the project. Process typed messages with the default Framework JSON serializer so processes use the same contract: declare each message in `server/shared/contracts.hpp` with a `packet_name` and JSON conversion, and add no per-message codecs, manual encoding, or raw frame parsing. Limit direct serialization in relocation adapters to application state (`zlink::message_t::from_json` in `capture`, `parse_json` in `restore`), keeping management of Framework execution state separate from game state.

Manage each game fact, such as a player's HP and position, in the object responsible for changing it. Store player IDs and observational copies in Spots, and never retain Actor references: a Spot handler receives the Actor as an argument and reaches other players through `actor_client_t` by player ID. Sessions forward packets after authentication and binding; movement and combat are decided by the `Room` that owns the state, and `HttpLobby` only pairs HTTP requests and prepares the Room. Keep game rules in `server/match/domain.hpp`, and keep Framework callbacks in the Spot, Actor, and Session files.

### Asynchronous execution and lifecycle

Write handlers as `task_t` coroutines and `co_await` public asynchronous calls as the template does; the [Host execution model](https://docs.zlink.systems/cpp/guide/server/43-execution-model/) reserves blocking waits such as `.result()` for tests and clients. Do not block a handler with waits or synchronous I/O. `NodeControl` waits with `await_host` on its own stdin thread, and `HttpLobby` waits with it on its own IO thread when it prepares a Room; neither runs inside a Spot or Actor handler. Since the SpotWide execution mode limits execution over the same state to one gate, do not protect that state again with another lock or queue. Awaiting can retain the turn, so do not await work that requires that turn to finish, such as a request back to the same Spot; perform long-running work outside handlers. The [Execution model](https://docs.zlink.systems/cpp/guide/server/32-execution-model/) explains these execution boundaries.

Keep type registration and per-instance callbacks in their defined lifecycle phases: factories and Entry Spots are registered in each node's `main.cpp`, handlers in `configure()`, and admission and membership decisions in `on_actor_join`, `on_actor_joined`, `on_leave_actor` and `on_disconnect_actor`. Use `join_spot(...).defer()` and the public completion callback `on_join_completed` for membership changes, and relocation with runtime observations for host shutdown, so request acceptance is not mistaken for completion. [Activation and lifetime](https://docs.zlink.systems/cpp/guide/server/34-activation-lifetime/) explains creation and initialization.

### Reconnection and failure handling

Reconnect with the same Actor ID and bind the new Session to continue using existing player state: `Session::on_packet` binds with `bind_or_get`, and `Room::reconnect` re-sends the current `RoomView` to the new Session. Framework manages current placement, so do not add an application route cache or reset player state during reconnect. Backpressure is the behavior that delays or rejects calls when processing capacity is unavailable; observe the public results described in the [Backpressure guide](https://docs.zlink.systems/cpp/guide/server/33-backpressure/).

Investigate Framework failures using actual replies, events, and lifecycle results. Internal APIs, reflection, raw frames, private codecs, extra pollers, fixed sleeps, transport retries, increased deadlines, and weakened assertions do not verify the failing public operation and must not be added as fixes. Write code comments in English and exclude execution and build artifacts in `.logs/`, `build/`, and `.zlink/` from commits. Run formatters and installation commands only from this project directory, with explicit file paths that exclude those directories.

## Debugging

### Logs for each run

Preserve the first failing run and read its logs before deciding whether to run it again. An intermittent failure is an observed defect; a subsequent pass does not invalidate it. `.logs/run-<id>/` contains the effective settings that `e2e/world.hpp` writes at node startup and the logs for each node. Each selected scenario starts its own nodes and writes to its own `.logs/run-<id>/` directory.

Settings files are `lobby.json`, `match-a.json`, `match-b.json`, and `session.json`; a replaced Match node uses its file name with the suffix `-replacement`. `e2e/process.hpp` records diagnostic stderr in `<node>.err.log` and readiness/control stdout in `<node>.out.log`, where `<node>` is the settings file name without `.json`. Helper commands (Compose and Docker CLI calls) write `command-<n>.out.log` and `command-<n>.err.log`. Local build output is in `.logs/build.log`. Docker build and outer cleanup logs are in `.logs/docker-<project>/`: `build.log`, `redis-start.log`, `redis.log`, `cleanup.log`, and `containers-stop.log` from the PowerShell runner.

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
