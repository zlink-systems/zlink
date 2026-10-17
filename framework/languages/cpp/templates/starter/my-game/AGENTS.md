# MyGame development guide

This C++20 starter provides one Session node, Redis hosting, diagnostics, stdin control, and a connection e2e runner. It has no game state or game packets. [README](README.md) owns build, run, and deployment instructions; [한국어](AGENTS.ko.md) provides the same development guide in Korean.

## Node responsibilities

| Module | Responsibility and extension point |
| --- | --- |
| `server/session/main.cpp` | Register STREAM and the mesh listener; add registrations when introducing application objects |
| `server/session/session.hpp` | Connection callbacks; add authentication and application packet handling |
| `server/shared/` | Settings (`config.hpp`), hosting and stdin control (`hosting.hpp`), names, limits, and the single `Control` marker definition (`contracts.hpp`) |
| `e2e/scenarios.hpp` | Observable scenarios; add new scenario IDs to `scenario_order` and the descriptor together |
| `e2e/client.hpp` | Public STREAM connector wrapper; add typed listeners and sends for new packets |
| `e2e/world.hpp` | Start nodes, capture logs, and clean up owned resources; `close()` collects cleanup failures |
| `e2e/main.cpp` | Select scenarios and report the scenario failure first, then cleanup failures |
| `e2e/structure.hpp` | Check actual configuration and registration declarations before startup |

`zlink.structure.json` describes initial composition, not runtime placement. This starter declares no application channels or Spots. When adding them, extend the checker to compare their names and types with registration constants.

## Development from samples

Download and run a structure from [ZLink Start](https://zlink.systems/start) before introducing its features here. The sample collection contains nine structures in five server languages. Keep the sample separate and transfer only the registrations, application state, and tests required by your feature.

Choose the sample according to the composition your game needs.

| Structure | Selection criterion |
| --- | --- |
| `board-tables` | Table-based board games with player Actors and a shared table Spot |
| `match-entry-lobby` | STREAM entry, matchmaking, and dynamically allocated match rooms |
| `match-http-lobby` | HTTP matchmaking followed by STREAM room entry |
| `morpg-lobby-instances` | Party formation in a lobby followed by a private dungeon |
| `morpg-channel-town` | Town channels, parties, and private dungeon instances |
| `morpg-fields-instances` | Shared fields across nodes, border visibility, and party dungeons |
| `mmo-world-channels` | Separate worlds with their own storage and switchable field channels |
| `mmo-megaserver` | Additional region instances when one region reaches capacity |
| `mmo-seamless` | Continuous movement across zone boundaries and border subscriptions |

The feature paths below are relative to the ZLink repository's `framework/languages/cpp/templates/`. In a downloaded sample, use the portion after `<structure>/my-game/`. Short companion paths refer to the main file's directory or the explicitly named project root.

| Feature | Sample file and work to transfer |
| --- | --- |
| Spot | `board-tables/my-game/server/table/table.hpp`: table-owned state and admission; register it in `server/table/main.cpp` |
| Actor | `board-tables/my-game/server/table/table.hpp`: minimal player Actor (`Player`, `PlayerFactory`) and join completion |
| Entry and first admission | `morpg-channel-town/my-game/server/shared/player.hpp`: `TownEntry` first-packet handler and deferred Town join |
| Session binding | `board-tables/my-game/server/session/session.hpp`: get-or-create, bind, and relay the original typed packet |
| Host relocation | `board-tables/my-game/server/table/table.hpp`: `TableRelocation` capture/restore; pair with `e2e/scenarios.hpp` update checks |
| Dynamic dungeon | `morpg-lobby-instances/my-game/server/lobby/party.hpp`: get-or-create and preparation; pair with `server/instance/dungeon.hpp` and `server/instance/main.cpp` |
| World/channel selection | `mmo-world-channels/my-game/server/channel/zone.hpp`: `change` uses deferred membership changes; inspect `server/accounts/main.cpp` for world listing |
| Channel pub/sub and seamless movement | `mmo-seamless/my-game/server/zone/zone.hpp`: border subscription/publication; pair with `server/zone/player.hpp` for movement |
| HTTP lobby | `match-http-lobby/my-game/server/lobby/http.hpp`: HTTP endpoint and matchmaking; pair with `server/lobby/main.cpp` for hosting |
| Typed messages | `board-tables/my-game/server/shared/contracts.hpp`: shared packet types |

The channels in `mmo-world-channels` are player-selected game spaces within a world. They differ from messaging subscription channels, and that sample registers no pub/sub subscriptions. Use the actual registrations and handlers in `mmo-seamless` for border messaging subscriptions.

After each addition, extend settings and registrations, `zlink.structure.json`, its checker, and e2e together. Keep game rules in plain domain headers when they appear (see `server/table/domain.hpp` in `board-tables`); keep Framework callbacks and transport adapters in separate headers. Do not create empty layers in this starter.
## Public API boundaries

Use the [documentation](https://docs.zlink.systems/), [C++ server guide](https://docs.zlink.systems/cpp/guide/server/), [API reference](https://docs.zlink.systems/cpp/reference/), and [agent index](https://docs.zlink.systems/llms.txt). Check API availability against the version in `bootstrap.cmake` because online guides may describe a newer release.

Use public APIs and the Framework default typed JSON serializer. Do not introduce reflection, internal APIs, raw frames, manual codecs, sleeps, retries, extended deadlines, or weakened assertions to compensate for a Framework failure.

Each fact has one state owner. When gameplay is added, replies inside an Actor handler go through that Actor's bound Session. Other pushes address the Actor by player ID; the receiving Actor handler forwards them through its bound Session. A Spot must not retain Actor references for later delivery because host relocation does not repeat `OnJoinedActor` on the target. Framework owns routing, leases, execution gates, and membership; relocation adapters capture application state only.

## Changes and verification

Run the README commands from this directory. For debugger attachment, use `--local` or `-Local` and attach the debugger to the `my-game-session` process that the runner starts from `build/bin`. The `connect` scenario owns the current smoke check; add gameplay scenarios without replacing it. Preserve received-result assertions and check failure exit codes as well as success.

Use OS-assigned ports, unique Redis key prefixes, and unique Compose projects. Clean up only resources created by the run. A cleanup error or an intermittent failure is a failure; preserve the first logs before another run.

Each run owns one `World`. `World::close()` stops the nodes and removes the run's containers and Compose project, collecting every failure; `main` reports the scenario exception first and cleanup errors afterward, and returns exit 1 for either kind of failure.

## Debugging settings

`configure_host` in `server/shared/hosting.hpp` enables dispatch diagnostics at `Normal` and writes Framework message flow to node stderr. Node stdout and stderr are stored separately as `session.out.log` and `session.err.log` under `.logs/run-<id>/`. Do not add a second flow listener.

Read the first exception and the preceding flow/correlation records. A connection-only run may contain no application message-flow events because it dispatches no game packets. Startup failures before dispatch must retain their exception and stack.

## Defect reporting

Compare public API usage with the guide and a working sample first. Preserve the pinned versions, OS, reproduction command, first node failure, and flow/correlation records. Check existing [ZLink issues](https://github.com/zlink-systems/zlink/issues) and record the matching issue number in README Known issues. If a coding agent prepares a new public issue, show its body to the developer before publication. Remove credentials and personal information from attached logs.
