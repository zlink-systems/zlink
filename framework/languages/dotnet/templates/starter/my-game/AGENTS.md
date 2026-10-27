# MyGame development guide

This .NET 8 starter provides one Session node, Redis hosting, diagnostics, stdin control, and a connection e2e runner. It has no game state or game packets. [README](README.md) owns build, run, and deployment instructions; [한국어](AGENTS.ko.md) provides the same development guide in Korean.

## Node responsibilities

| Module | Responsibility and extension point |
| --- | --- |
| `server/MyGame.Session/Program.cs` | Register STREAM and the mesh listener; add registrations when introducing application objects |
| `server/MyGame.Session/PlayerSession.cs` | Connection callbacks; add authentication and application packet handling |
| `server/MyGame.Shared/` | Settings, hosting, and the single `NodeProtocol` marker definition |
| `e2e/MyGame.E2E/Scenarios.cs` | Observable scenarios; add new scenario IDs to `All` and the descriptor together |
| `e2e/MyGame.E2E/WorldRun.cs` | Start nodes, capture logs, and clean up owned resources |
| `e2e/MyGame.E2E/RunOutcome.cs` | Collect cleanup failures for Program to report after the scenario failure |
| `e2e/MyGame.E2E/StructureCheck.cs` | Check actual configuration and registration declarations before startup |

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

The feature paths below are relative to the ZLink repository's `framework/languages/dotnet/templates/`. In a downloaded sample, use the portion after `<structure>/my-game/`. Short companion paths refer to the main file's directory or the explicitly named project root.

| Feature | Sample file and work to transfer |
| --- | --- |
| Spot | `board-tables/my-game/server/MyGame.Table/TableSpot.cs`: table-owned state and admission; register it in `Program.cs` |
| Actor | `board-tables/my-game/server/MyGame.Table/PlayerActor.cs`: minimal player Actor, join completion, and factory |
| Entry and first admission | `morpg-channel-town/my-game/server/MyGame.Shared/TownEntry.cs`: first-packet handler and deferred Town join |
| Session binding | `board-tables/my-game/server/MyGame.Session/PlayerSession.cs`: find/create, bind, and relay the original typed packet |
| Host relocation | `board-tables/my-game/server/MyGame.Table/TableSpot.cs`: TableRelocation capture/restore; pair with `e2e/MyGame.E2E/Scenarios.cs` update checks |
| Dynamic dungeon | `morpg-lobby-instances/my-game/server/MyGame.Lobby/PartySpot.cs`: GetOrCreate and preparation; pair with `server/MyGame.Instance/DungeonSpot.cs` and `server/MyGame.Instance/Program.cs` |
| World/channel selection | `mmo-world-channels/my-game/server/MyGame.Channel/Infrastructure/ZLink/Handlers.cs`: ChangeChannel uses deferred membership changes; inspect `server/MyGame.Accounts/Program.cs` for world listing |
| Channel pub/sub and seamless movement | `mmo-seamless/my-game/server/MyGame.Zone/ZoneSpot.cs`: border subscription/publication; pair with `Handlers.cs` for delivery and `PlayerActor.cs` for movement |
| HTTP lobby | `match-http-lobby/my-game/server/MyGame.Lobby/Program.cs`: HTTP endpoint; pair with `Application/Matchmaker.cs` and `Infrastructure/ZLink/RoomProvisioner.cs` |
| Typed messages | `board-tables/my-game/server/MyGame.Shared/Contracts.cs`: shared packet records |

The channels in `mmo-world-channels` are player-selected game spaces within a world. They differ from messaging subscription channels, and that sample registers no pub/sub subscriptions. Use the actual registrations and handlers in `mmo-seamless` for border messaging subscriptions.

After each addition, extend settings and registrations, `zlink.structure.json`, its checker, and e2e together. Keep game rules in Domain/Application modules when they appear; put Framework callbacks and transport adapters under Infrastructure/ZLink. Do not create empty layers in this starter.
## Public API boundaries

Use the [documentation](https://docs.zlink.systems/), [.NET server guide](https://docs.zlink.systems/dotnet/guide/server/), [API reference](https://docs.zlink.systems/dotnet/reference/), and [agent index](https://docs.zlink.systems/llms.txt). Check API availability against `Directory.Packages.props` because online guides may describe a newer release.

Use public APIs and the Framework default typed JSON serializer. Do not introduce reflection, internal APIs, raw frames, manual codecs, sleeps, retries, extended deadlines, or weakened assertions to compensate for a Framework failure.

Each fact has one state owner. When gameplay is added, replies inside an Actor handler go through that Actor's bound Session. Other pushes address the Actor by player ID; the receiving Actor handler forwards them through its bound Session. A Spot must not retain Actor references for later delivery because host relocation does not repeat `OnJoinedActor` on the target. Framework owns routing, leases, execution gates, and membership; relocation adapters capture application state only.

## Changes and verification

Run the README commands from this directory. For debugger attachment, use `--local` or `-Local` and inspect the PID files in the run log directory. The `connect` scenario owns the current smoke check; add gameplay scenarios without replacing it. Preserve received-result assertions and check failure exit codes as well as success.

Use OS-assigned ports, unique Redis key prefixes, and unique Compose projects. Clean up only resources created by the run. A cleanup error or an intermittent failure is a failure; preserve the first logs before another run.

Each run owns a RunOutcome instance. Record process and connector cleanup failures there; Program reports the scenario exception first and cleanup errors afterward, and returns exit 1 for either kind of failure.

## Debugging settings

`GameHosting.Configure` enables dispatch diagnostics at `Normal` and writes Framework message flow to node stderr. Node stdout and stderr are stored separately as `session.out.log` and `session.err.log` under `.logs/run-<id>/`. Do not add a second flow listener.

Read the first exception and the preceding flow/correlation records. A connection-only run may contain no application message-flow events because it dispatches no game packets. Startup failures before dispatch must retain their exception and stack.

For supplementary local tracing, set the parent process environment before running.

```powershell
$env:ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY = "1"
```

Bash uses `export ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`. Docker nodes do not inherit host variables; add diagnostic environment values explicitly to node creation when required. This version-specific switch is for diagnosis, not routing.

## Defect reporting

Compare public API usage with the guide and a working sample first. Preserve the pinned versions, OS, reproduction command, first node failure, and flow/correlation records. Check existing [ZLink issues](https://github.com/zlink-systems/zlink/issues) and record the matching issue number in README Known issues. If a coding agent prepares a new public issue, show its body to the developer before publication. Remove credentials and personal information from attached logs.
