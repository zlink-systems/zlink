# Project guidelines

This template implements MORPG fields and party-owned dungeons through public ZLink Framework APIs.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| Session | Authenticate and bind Player Actors; extend `PlayerSession` for authentication. |
| Field | Fixed fields, border subscriptions, party creation and dungeon admission; extend `FieldSpot` and its typed handlers. |
| Instance | Dynamic Dungeon User Spots, roster and stage; extend `DungeonSpot` and its typed handlers. |
| Shared | Typed contracts, Player Actor position and sequence, Redis hosting, diagnostics and node control. |
| E2E | Public stream client, process ownership and scenario assertions. |

## Public API boundaries

Use typed messages and the default Framework JSON serializer. Reply to the requesting Actor through its bound Session. Send other pushes by player ID through Actor send; the receiving Spot handler forwards them to the bound Session. Spots never retain Actor references. Session binding owns Actor identity; game state views must not include Actor generation or Session identity. Relocation adapters serialize application state only. Do not use internal APIs, reflection, raw frames, custom codecs, transport retries, fixed sleeps or expanded deadlines to hide Framework failures.

## Changes and verification

Run `./run-e2e.sh --build-only` (PowerShell: `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly`) to build Docker images. The default `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` runs nodes and the e2e runner in Docker. Use `--local` or `-Local` for host process debugging with the .NET 8 SDK; only that mode accepts `--no-build` or `-NoBuild`. Pass `--check-structure` to check without starting nodes. Preserve stdin lifecycle control and inspect `.logs/docker-<project>/` and `.logs/run-<id>/`.

Keep all four scenario IDs and their order in `zlink.structure.json` aligned with the runner. Validate structure before Redis or nodes start. The checker examines real files only. Verify invalid structures with temporary files under `.logs/`, never embed checker self-tests. Run build and e2e on Windows and WSL; repeat successful complete runs three times. A single intermittent failure is a defect. Preserve `.logs/run-<id>/` including message flow in stderr. Clean up only processes and the Compose project owned by the run. Preserve the single owners of Field and Dungeon state and let the Framework execution gate serialize handlers.

Register the shared Entry and first-login handler on every node that registers the Player Actor factory. Declare Entry only on roles registering application Entry and its first-entry handler: Field and Instance in this template. Do not declare internal Framework default Entries. Structure validation must require those Entries and every registered User Spot type. Initial Field hosts start sequentially, awaiting READY after bootstrap; each Field capacity must equal its configured Field ID count so earlier hosts are full. Keep Message Follow and the derived safe-shutdown observation deadline in shared timing constants.
