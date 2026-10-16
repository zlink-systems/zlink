# MyGame development guide

This Kotlin starter (Kotlin 2.3.10 on a Java 25 toolchain) provides one Session node, Redis hosting, diagnostics, stdin control, and a connection e2e runner. It has no game state or game packets. [README](README.md) owns build, run, and deployment instructions; [한국어](AGENTS.ko.md) provides the same development guide in Korean.

## Node responsibilities

| Module | Responsibility and extension point |
| --- | --- |
| `server/session/src/main/kotlin/MyGame/session/Program.kt` | Register STREAM and the mesh listener; add registrations when introducing application objects |
| `server/session/src/main/kotlin/MyGame/session/PlayerSession.kt` | Connection callbacks; add authentication and application packet handling |
| `server/shared/` | Settings and hosting (`Hosting.kt`), the single `Control` marker definition, and the name, role, and scenario constants (`Contracts.kt`) |
| `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt` | Observable scenarios; add new scenario IDs to `Scenario` and the descriptor together |
| `e2e/src/main/kotlin/MyGame/e2e/Nodes.kt` | Start nodes, capture logs, and clean up owned resources; Program reports cleanup failures after scenario failures |
| `e2e/src/main/kotlin/MyGame/e2e/Structure.kt` | Check actual configuration and registration declarations before startup |

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

The feature paths below are relative to the ZLink repository's `framework/languages/java/templates/kotlin/`. In a downloaded sample, use the portion after `<structure>/my-game/`. Short companion paths refer to the main file's directory or the explicitly named module root.

| Feature | Sample file and work to transfer |
| --- | --- |
| Spot | `board-tables/my-game/server/table/src/main/kotlin/MyGame/table/Table.kt`: table-owned state and admission; register it in `Program.kt` |
| Actor | `board-tables/my-game/server/table/src/main/kotlin/MyGame/table/Player.kt`: minimal player Actor and join completion |
| Entry and first admission | `morpg-channel-town/my-game/server/shared/src/main/kotlin/MyGame/admission/Entry.kt`: `Entry` and the first-packet handler with the deferred Town join |
| Session binding | `board-tables/my-game/server/session/src/main/kotlin/MyGame/session/Program.kt`: `GameSession` finds or creates the Actor, binds it, and relays the original typed packet |
| Host relocation | `board-tables/my-game/server/table/src/main/kotlin/MyGame/table/Transfer.kt`: capture/restore of application state, and the update scenario in `e2e/src/main/kotlin/MyGame/e2e/Scenarios.kt` |
| Dynamic dungeon | `morpg-lobby-instances/my-game/server/lobby/src/main/kotlin/MyGame/lobby/infrastructure/zlink/Spots.kt`: getOrCreate and preparation; pair with `server/instance/src/main/kotlin/MyGame/instance/infrastructure/zlink/Dungeon.kt` and `MyGame/instance/Program.kt` |
| World/channel selection | `mmo-world-channels/my-game/server/channel/src/main/kotlin/MyGame/channel/infrastructure/zlink/Handlers.kt`: the channel change uses a deferred Spot join; inspect `server/accounts/src/main/kotlin/MyGame/accounts/Program.kt` for world listing |
| Channel pub/sub and seamless movement | `mmo-seamless/my-game/server/zone/src/main/kotlin/MyGame/border/BorderRoutes.kt`: border subscription handlers; pair with `MyGame/zone/Zone.kt` for publication and `MyGame/zone/Handlers.kt` for movement |
| HTTP lobby | `match-http-lobby/my-game/server/lobby/src/main/kotlin/MyGame/lobby/infrastructure/HttpLobby.kt`: HTTP endpoint and room creation; pair with `application/Matchmaker.kt` |
| Typed messages | `board-tables/my-game/server/shared/src/main/kotlin/MyGame/Contracts.kt`: shared packet classes and their names |

The channels in `mmo-world-channels` are player-selected game spaces within a world. They differ from messaging subscription channels, and that sample registers no pub/sub subscriptions. Use the actual registrations and handlers in `mmo-seamless` for border messaging subscriptions.

After each addition, extend settings and registrations, `zlink.structure.json`, its checker, and e2e together. Keep game rules in application modules apart from the Framework callbacks and transport adapters that call them. Do not create empty layers in this starter.

## Public API boundaries

Use the [documentation](https://docs.zlink.systems/), [Kotlin server guide](https://docs.zlink.systems/kotlin/guide/server/), [API reference](https://docs.zlink.systems/kotlin/reference/), and [agent index](https://docs.zlink.systems/llms.txt). Check API availability against `build.gradle.kts` because online guides may describe a newer release.

Use public APIs and the Framework default typed JSON serializer. Do not introduce reflection, internal APIs, raw frames, manual codecs, sleeps, retries, extended deadlines, or weakened assertions to compensate for a Framework failure.

Each fact has one state owner. When gameplay is added, replies inside an Actor handler go through that Actor's bound Session. Other pushes address the Actor by player ID; the receiving Actor handler forwards them through its bound Session. A Spot must not retain Actor references for later delivery because host relocation does not repeat `onJoinedActor` on the target. Framework owns routing, leases, execution gates, and membership; relocation adapters capture application state only.

## Changes and verification

Run the README commands from this directory. For debugger attachment, use `--local` or `-Local` and inspect the node logs in the run log directory. The `connect` scenario owns the current smoke check; add gameplay scenarios without replacing it. Preserve received-result assertions and check failure exit codes as well as success.

Use OS-assigned ports, unique Redis key prefixes, and unique Compose projects. Clean up only resources created by the run. A cleanup error or an intermittent failure is a failure; preserve the first logs before another run.

`Program` prints the scenario failure first and then any failure from `Nodes.close`, which collects every cleanup error; either kind makes the exit code 1. Format Kotlin sources with ktfmt `--kotlinlang-style` from this directory only.

## Debugging settings

`configure` in `Hosting.kt` enables dispatch diagnostics at `Normal` and writes Framework message flow to node stderr. Node stdout and stderr are stored separately as `session-stdout.log` and `session-stderr.log` under `.logs/run-<id>/session-<id>/`. Do not add a second flow listener.

Read the first exception and the preceding flow/correlation records. A connection-only run may contain no application message-flow events because it dispatches no game packets. Startup failures before dispatch must retain their exception and stack.

For supplementary STREAM connector tracing in the e2e runner, set `ZLINK_JAVA_STREAM_TRACE=1` in the parent process environment before a `--local` run. The Docker runner and nodes do not inherit host variables; add diagnostic environment values explicitly to the Compose service or node creation when required. This version-specific switch is for diagnosis, not routing.

## Defect reporting

Compare public API usage with the guide and a working sample first. Preserve the pinned versions, OS, reproduction command, first node failure, and flow/correlation records. Check existing [ZLink issues](https://github.com/zlink-systems/zlink/issues) and record the matching issue number in README Known issues. If a coding agent prepares a new public issue, show its body to the developer before publication. Remove credentials and personal information from attached logs.
