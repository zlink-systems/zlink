# Project guidelines

This standalone Java template implements an action/FPS entry lobby with a fixed matchmaking queue and dynamic two-player rooms. Keep the structure manifest, server registrations, and e2e scenarios consistent.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| Session | STREAM, Actor GetOrCreate, binding, relay; `server/session/.../Program.java` |
| Match | Entry admission, matchmaking, Room state; `server/match/.../RoomState.java`, `MatchmakingState.java`, `Actions.java` |
| Shared | Typed contracts, Redis stores, runtime observation, stdin controls; `server/shared/` |
| Players | Public stream connector and scenario assertions; `e2e/` |
| Location Store | Redis via the run-owned Compose project |

## Public API boundaries

Use public ZLink APIs and the default typed JSON serializer. Replies to a request use the handler Actor's bound Session. Other pushes use the player IDs in Spot state and public Actor send, then a typed Spot Actor handler forwards the message to the bound Session. Spots never retain Actor references. RoomState owns combat facts; MatchmakingState owns queues and room IDs. Session binding owns identity. Relocation adapters serialize application state only.

Do not use internal APIs, reflection, raw frames, per-message codecs, manual packet encoding, sleeps, retries, enlarged deadlines, or weaker assertions to compensate for Framework defects. Report the first failure and message-flow logs. Do not duplicate Framework route, lease, membership, completion, or execution-gate decisions in application state.

## Changes and verification

Run `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` for Docker e2e. Build images with `--build-only` or `-BuildOnly`; check structure with `--check-structure` or `-CheckStructure`. Use `--local` or `-Local` for local process debugging with JDK 25.

Keep the ordered scenarios `hit,reconnect,update`. Run structure validation before starting nodes. Verify Windows and Linux builds and e2e with public packages. A single intermittent failure is a defect. Logs are in `.logs/run-<id>/`; preserve the first failure. Every run must use unique Redis keys and a Compose project, OS-assigned ports, and cleanup only its own processes and containers. Check performance, POSDDD ownership, and unused code in changed functions before completion. Code comments are English.
