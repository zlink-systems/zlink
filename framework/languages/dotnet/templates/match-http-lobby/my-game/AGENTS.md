# Project guidelines

This is a standalone MyGame HTTP matchmaking and STREAM arena template. The lobby prepares rooms before returning assignments; the match nodes execute gameplay and preserve application state during planned maintenance.

## Node responsibilities

| Node | Responsibility and extension location |
| --- | --- |
| lobby | HTTP queue and pairing in `server/MyGame.Lobby/Application/Matchmaker.cs`; object preparation adapter in `Infrastructure/ZLink/RoomProvisioner.cs`. |
| session | STREAM Actor creation, binding, SessionInfo and relay in `server/MyGame.Session/PlayerSession.cs`. |
| matchA, matchB | Entry and Actor callbacks in `server/MyGame.Match/Infrastructure/ZLink/`; combat rules in `Domain/RoomState.cs`. |
| location | Redis authority and relocation storage configured by shared hosting. |
| players | HTTP and public STREAM connector verification in `e2e/MyGame.E2E/`. |

## Public API boundaries

Use public ZLink APIs and the default typed JSON serializer. Replies inside an Actor request handler use the received Actor's bound Session. Other pushes use player IDs from Room state with the public Actor send API; the received Actor delivery handler forwards them to its bound Session. Do not retain Actor references in Spot state. SessionInfo reads Actor identity and generation from the Session binding, never from gameplay state.

Relocation adapters serialize application state only. Do not copy Framework routes, leases, membership or queues. Do not use internal APIs, reflection, raw frames, manual message codecs, sleeps, retries, enlarged deadlines or weakened assertions to compensate for Framework defects. Report the first failure with message-flow logs instead.

## Changes and verification

Run `./run-e2e.sh --build-only` (PowerShell: `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly`) to build Docker images. The default `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` runs nodes and the e2e runner in Docker. Use `--local` or `-Local` for host process debugging with the .NET 8 SDK; only that mode accepts `--no-build` or `-NoBuild`. Pass `--check-structure` to check without starting nodes. Preserve stdin lifecycle control and inspect `.logs/docker-<project>/` and `.logs/run-<id>/`.

Keep the declared scenarios in canonical order: lobby, hit, reconnect, update. Run the structure check before startup and verify the affected scenario on Windows and WSL. A single intermittent failure is a defect; a successful rerun does not invalidate it. Build and execution logs belong to `.logs/`; never commit logs, package caches, bin, obj or license files. Keep package versions centralized and the project independently copyable. README commands and `run.commands` must agree. Code comments are English.
