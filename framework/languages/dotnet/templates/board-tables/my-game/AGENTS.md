# MyGame development guidelines

This project is a board table server template that uses published NuGet packages. Follow README.md for how to run it. Write code comments in English.

## Node responsibilities and extension points

| Project | Responsibility | Extension point |
| --- | --- | --- |
| `MyGame.Session` | Terminate STREAM, GetOrCreate and bind the Player Actor, relay | Authentication before bind |
| `MyGame.Table` | Entry Spot, dynamic Table User Spots, hosting Player Actors | Join, game rules, settlement |
| `MyGame.Shared` | Messages, constants, node configuration, hosting, control protocol | Shared contracts |
| `MyGame.E2E` | Isolated runs, public client, judging observed results | Game scenarios |

The Table Spot owns seats, chips, deck, cards, turns, and pot. The Player Actor handles only identity and the bound session. Send only to the playerId of a Table seat, and keep no copy of Actor references. The Framework owns routes, leases, membership, queues, and the execution gate.

## Public API boundaries

- Use only published NuGet packages and the template's own project references.
- Use typed messages and the default JSON serializer. The relocation adapter serializes only application state.
- Send `SessionInfoRequest` after `JoinTableRequest`. The Session node replies with `SessionInfo` containing the bound Actor's `ActorId` and `ObjectGeneration`. The Table keeps no Actor identity.
- The Entry Spot uses the public `GetOrCreate` and `JoinSpot(...).Defer()`. A request handler replies to the requester through `Context.BoundSession.Send` of the Actor it receives. Any other push calls `IZLinkActorClient.SendToActor` with the seat's playerId, and the public Spot-to-Actor delivery handler sends through the real Actor's bound session.
- Reconnect binds the same Actor to the new Session. Do not reset game state or create an application route cache.
- Drain uses public host relocation and the `SafeToShutdown` observation.
- Do not work around a Framework defect with internal APIs, reflection, raw frames, manual codecs, pollers, sleeps, retries, longer deadlines, or weaker assertions.

## Verification

Run `./run-e2e.sh --build-only` (PowerShell: `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly`) to build Docker images. The default `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` runs nodes and the e2e runner in Docker. Use `--local` or `-Local` for host process debugging with the .NET 8 SDK; only that mode accepts `--no-build` or `-NoBuild`. Pass `--check-structure` to check without starting nodes. Preserve stdin lifecycle control and inspect `.logs/docker-<project>/` and `.logs/run-<id>/`.

When you change the composition, update `zlink.structure.json` and the configurations together. Run the structure check and the table, reconnect, and update scenarios. Keep the checks for card privacy, chip conservation, the Actor generation in `SessionInfo`, and connection continuity. Do not add a self-test to the structure check; during development, confirm with a temporary modified file that invalid input is rejected.

Verify the build and e2e on both Windows and WSL. Read `.logs/` from the first run to classify a failure. An intermittent failure is a defect even if it happens once. Clean up only the Compose project and processes the runner owns. Do not commit `.logs/`, `bin/`, `obj/`, or licenses.

Node stderr records the message flow through the dispatch diagnostics `Normal` logger. Do not add a separate ActivityListener.
