# Project guidelines

This template implements MORPG town channels and party-owned dungeons through public ZLink Framework APIs.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| Session | Authenticate and bind Player Actors; extend `PlayerSession` for authentication. |
| Town | Fixed channels, party creation and dungeon admission; extend `TownSpot` and its typed handlers. |
| Instance | Dynamic Dungeon User Spots, roster and stage; extend `DungeonSpot` and its typed handlers. |
| Shared | Typed contracts, stateless Player Actor, Redis hosting, diagnostics and node control. |
| E2E | Public stream client, process ownership and scenario assertions. |

## Public API boundaries

Use typed messages and the default Framework JSON serializer. Reply to the requesting Actor through its bound Session. Send other pushes by player ID through Actor send; the receiving Spot handler forwards them to the bound Session. Spots never retain Actor references. Session binding owns Actor identity; game state views must not include Actor generation or Session identity. Relocation adapters serialize application state only. Do not use internal APIs, reflection, raw frames, custom codecs, transport retries, fixed sleeps or expanded deadlines to hide Framework failures.

## Changes and verification

Run `./run-e2e.sh --build-only` (PowerShell: `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly`) to build Docker images. The default `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` runs nodes and the e2e runner in Docker. Use `--local` or `-Local` for host process debugging with the .NET 8 SDK; only that mode accepts `--no-build` or `-NoBuild`. Pass `--check-structure` to check without starting nodes. Preserve stdin lifecycle control and inspect `.logs/docker-<project>/` and `.logs/run-<id>/`.

Keep all four scenario IDs and their order in `zlink.structure.json` aligned with the runner. Validate structure before Redis or nodes start. The checker examines real files only. Verify invalid structures with temporary files outside this project, never embed checker self-tests. Run build and e2e on Windows and WSL; repeat successful complete runs three times. A single intermittent failure is a defect. Preserve `.logs/run-<id>/` including message flow in stderr. Clean up only processes and the Compose project owned by the run. Preserve the single owners of Town and Dungeon state and let the Framework execution gate serialize handlers.

Town and Instance register the shared application Entry and first-admission handler. Declare one Entry for each of these nodes and none for roles without application Entry registration; Framework default Entries are not part of the structure. Keep dynamic Dungeon declarations complete for every owning Instance. Preserve one Dungeon slot per Instance in the e2e configuration; it makes replacement placement deterministic. Keep the Message Follow duration and derived SafeToShutdown observation budget under their shared lifecycle constants.

Bootstrap Town providers sequentially, waiting for each READY before starting the next. Set each Town provider's public stable-type capacity to exactly its configured fixed Town count. Earlier providers are full and later providers are absent during bootstrap, so only the declared owner can create its fixed Towns. The structure checker enforces the exact capacity.
