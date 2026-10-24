# MyGame development guide

This Node.js 22 starter provides one Session node, Redis hosting, diagnostics, stdin control, and a connection e2e runner. It has no game state or game packets. [README](README.md) owns build, run, and deployment instructions; [한국어](AGENTS.ko.md) provides the same development guide in Korean.

## Node responsibilities

| Module                      | Responsibility and extension point                                                            |
| --------------------------- | --------------------------------------------------------------------------------------------- |
| `server/session/main.ts`    | Register STREAM and the mesh listener; add registrations when introducing application objects |
| `server/session/session.ts` | Connection callbacks; add authentication and application packet handling                      |
| `server/shared/`            | Settings, hosting, and the single `Control` marker definition                                 |
| `e2e/client.mts`            | Observable scenarios; add new scenario IDs to `Scenarios` and the descriptor together         |
| `e2e/runner.ts`             | Start nodes, capture logs, and clean up owned resources                                       |
| `e2e/structure.ts`          | Check actual configuration and registration declarations before startup                       |

`zlink.structure.json` describes initial composition, not runtime placement. This starter declares no application channels or Spots. When adding them, extend the checker to compare their names and types with registration constants.

## Development from samples

Download and run a structure from [ZLink Start](https://zlink.systems/start) before introducing its features here. The sample collection contains nine structures in five server languages. Keep the sample separate and transfer only the registrations, application state, and tests required by your feature.

Choose the sample according to the composition your game needs.

| Structure                | Selection criterion                                                  |
| ------------------------ | -------------------------------------------------------------------- |
| `board-tables`           | Table-based board games with player Actors and a shared table Spot   |
| `match-entry-lobby`      | STREAM entry, matchmaking, and dynamically allocated match rooms     |
| `match-http-lobby`       | HTTP matchmaking followed by STREAM room entry                       |
| `morpg-lobby-instances`  | Party formation in a lobby followed by a private dungeon             |
| `morpg-channel-town`     | Town channels, parties, and private dungeon instances                |
| `morpg-fields-instances` | Shared fields across nodes, border visibility, and party dungeons    |
| `mmo-world-channels`     | Separate worlds with their own storage and switchable field channels |
| `mmo-megaserver`         | Additional region instances when one region reaches capacity         |
| `mmo-seamless`           | Continuous movement across zone boundaries and border subscriptions  |

The feature paths below are relative to the ZLink repository's `framework/languages/node/templates/`. In a downloaded sample, use the portion after `<structure>/my-game/`. Short companion paths refer to the main file's directory or the explicitly named project root.

| Feature                               | Sample file and work to transfer                                                                                                                                     |
| ------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Spot                                  | `board-tables/my-game/server/table/infrastructure/table.ts`: table-owned state and admission; register it in `board-tables/my-game/server/table/main.ts`             |
| Actor                                 | `board-tables/my-game/server/table/infrastructure/player.ts`: minimal player Actor, join completion, and factory                                                     |
| Entry and first admission             | `morpg-channel-town/my-game/server/shared/entry.ts`: first-packet handler and deferred Town join                                                                     |
| Session binding                       | `board-tables/my-game/server/session/session.ts`: find/create, bind, and relay the original typed packet                                                             |
| Host relocation                       | `board-tables/my-game/server/table/infrastructure/table.ts`: relocation adapter capture/restore; pair with `e2e/client.mts` update checks                            |
| Dynamic dungeon                       | `morpg-lobby-instances/my-game/server/lobby/spots.ts`: getOrCreate and preparation; pair with `server/instance/dungeon.ts` and `server/instance/main.ts`             |
| World/channel selection               | `mmo-world-channels/my-game/server/channel/handlers.ts`: ChangeChannel handler uses deferred membership changes; inspect `server/accounts/main.ts` for world listing |
| Channel pub/sub and seamless movement | `mmo-seamless/my-game/server/zone/zone.ts`: border subscription/publication; pair with `handlers.ts` for delivery and `player.ts` for movement                       |
| HTTP lobby                            | `match-http-lobby/my-game/server/lobby/http.ts`: HTTP endpoint; pair with `matchmaker.ts` for matching; room creation is in `http.ts`                                |
| Typed messages                        | `board-tables/my-game/server/shared/messages.ts`: shared packet classes                                                                                              |

The channels in `mmo-world-channels` are player-selected game spaces within a world. They differ from messaging subscription channels, and that sample registers no pub/sub subscriptions. Use the actual registrations and handlers in `mmo-seamless` for border messaging subscriptions.

After each addition, extend settings and registrations, `zlink.structure.json`, its checker, and e2e together. Keep game rules in Domain/Application modules when they appear; put Framework callbacks and transport adapters under Infrastructure/ZLink. Do not create empty layers in this starter.

The build generates packet schemas with `scripts/generate-framework-json-schemas.mjs` and hosting registers them. When adding typed packets, follow the sample handler's packet-name-to-class restoration (#1731); this starter registers no typed handlers.

## Public API boundaries

Use the [documentation](https://docs.zlink.systems/), [Node.js server guide](https://docs.zlink.systems/node/guide/server/), [API reference](https://docs.zlink.systems/node/reference/), and [agent index](https://docs.zlink.systems/llms.txt). Check API availability against `package.json` because online guides may describe a newer release.

Use public APIs and the Framework default typed JSON serializer. Do not introduce reflection, internal APIs, raw frames, manual codecs, sleeps, retries, extended deadlines, or weakened assertions to compensate for a Framework failure.

Each fact has one state owner. When gameplay is added, replies inside an Actor handler go through that Actor's bound Session. Other pushes address the Actor by player ID; the receiving Actor handler forwards them through its bound Session. A Spot must not retain Actor references for later delivery because host relocation does not repeat `onJoinedActor` on the target. Framework owns routing, leases, execution gates, and membership; relocation adapters capture application state only.

## Changes and verification

Run the README commands from this directory. For debugger attachment, use `--local` or `-Local` and attach to the spawned node process. The `connect` scenario owns the current smoke check; add gameplay scenarios without replacing it. Preserve received-result assertions and check failure exit codes as well as success.

Use OS-assigned ports, unique Redis key prefixes, and unique Compose projects. Clean up only resources created by the run. A cleanup error or an intermittent failure is a failure; preserve the first logs before another run.

The runner owns the run result and collects connector cleanup failures separately. It reports the scenario exception before cleanup errors and returns exit 1 for either kind of failure. Node, container, and Redis cleanup continue after an individual cleanup error.

## Debugging settings

`runHost` enables dispatch diagnostics at `Normal` and writes Framework message flow to node stderr. Node stdout and stderr are stored separately as `session-0.out.log` and `session-0.err.log` under `.logs/run-<id>/`. Do not add a second flow listener.

Read the first exception and the preceding flow/correlation records. A connection-only run may contain no application message-flow events because it dispatches no game packets. Startup failures before dispatch must retain their exception and stack.

For supplementary local tracing, set the parent process environment before running.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_RELOCATION = "1"
```

Bash uses `export ZLINK_DEBUG_FRAMEWORK_RELOCATION=1`. Docker nodes do not inherit host variables; add diagnostic environment values explicitly to node creation when required. This version-specific switch is for diagnosis, not routing.

## Defect reporting

Compare public API usage with the guide and a working sample first. Preserve the pinned versions, OS, reproduction command, first node failure, and flow/correlation records. Check existing [ZLink issues](https://github.com/zlink-systems/zlink/issues) and record the matching issue number in README Known issues. If a coding agent prepares a new public issue, show its body to the developer before publication. Remove credentials and personal information from attached logs.
