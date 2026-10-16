# Template guidelines

This template implements independent MMORPG worlds with account discovery, STREAM Sessions, fixed channel Zones, Player Actors and world-specific Redis stores.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| accounts | `server/MyGame.Accounts/Program.cs`: HTTP world discovery. |
| session | `server/MyGame.Session/PlayerSession.cs`: authenticate and bind players; return SessionInfo and relay typed packets. |
| channel | `server/MyGame.Channel/Domain/`: gold rules. `Infrastructure/ZLink/`: Zone and Player callbacks, handlers and relocation adapters. `Infrastructure/Redis/`: player persistence. |
| world-db | `docker-compose.yml`: independent world stores. |
| players | `e2e/MyGame.E2E/`: public connector client, scenario runner and structure validation. |

## Public API boundaries

Use public ZLink APIs and the default typed JSON serializer. Do not add raw frames, reflection, internal APIs or message codecs. SessionInfo derives identity from the bound Actor reference. In request handlers, send replies directly through that Actor's bound session. Other pushes address player IDs through the public Actor client and pass through the Zone delivery handler. Do not retain Actor references in Spots.

Player Actor is the only gold owner. Zone identity comes from Framework Spot context; do not cache channel or membership. Relocation adapters serialize only application state. Redis application keys use `player:<id>` under the world's run prefix, separately from Framework state keys. Never share world storage or mesh names.

## Changes and verification

Run `./run-e2e.sh --build-only` (PowerShell: `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly`) to build Docker images. The default `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` runs nodes and the e2e runner in Docker. Use `--local` or `-Local` for host process debugging with the .NET 8 SDK; only that mode accepts `--no-build` or `-NoBuild`. Pass `--check-structure` to check without starting nodes. Preserve stdin lifecycle control and inspect `.logs/docker-<project>/` and `.logs/run-<id>/`.

Keep the ordered worlds, channel, reconnect and update scenarios. Update structure and configuration together when extending topology. Logical Spot IDs are unique within a world mesh; composition IDs are globally unique. Run structure validation before startup, and verify invalid structures are rejected using temporary validation copies outside the deliverable.

Run builds and e2e on Windows and Linux. Treat every intermittent failure as a defect. Preserve the first failure and message flow from `.logs/run-<id>/`; do not increase deadlines, sleep, retry or weaken assertions. A Framework defect is a blocker rather than a reason to bypass its contracts. Cleanup must affect only this run's resources. Keep README commands identical to `run.commands`.
