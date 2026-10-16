# Project guidelines

This template implements a Kotlin MMO megaserver with capacity-limited Region User Spots, Player Actors, Dungeon User Spots, Redis stores, and a public stream e2e runner.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| session | STREAM connection, Actor GetOrCreate and binding, typed relay; `server/session/.../infrastructure/zlink/GameSession.kt` |
| region | Entry instance selection; Region membership, movement and visibility; `server/region/.../domain/Positions.kt` and `infrastructure/zlink/` |
| instance | Dungeon GetOrCreate and typed replies; `server/instance/.../infrastructure/zlink/` |
| location | Redis Location and relocation stores; shared hosting and Compose |
| players | Stream client and scenario assertions in `e2e/` |

## Public API boundaries

Use only public Framework and stream connector APIs and the default typed JSON serializer. A request handler replies through the received Actor's bound Session. Other pushes use stored player IDs with the public Actor send API; the receiving Actor handler forwards to its bound Session. Do not store Actor references in Region state. Region owns membership and position; Entry owns the next instance index after `region-full`. Relocation adapters serialize application state only.

Do not add internal APIs, reflection, raw frame parsing, custom codecs, reconnect state, transport retries, fixed sleeps, longer deadlines, or weaker assertions to compensate for Framework defects. Record the first failure and message flow before changing application code.

## Changes and verification

Run `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` for Docker e2e. Use `--local` or `-Local` for JDK 25 process debugging. Build images with `--build-only` or `-BuildOnly`. Preserve the Docker socket control path and remove only resources owned by the current run.

Keep the region, dungeon, reconnect, and update scenarios and their canonical order. Run structure validation before node startup. Verify changes on Windows and Linux; any intermittent failure is a defect. Preserve logs in `.logs/run-<id>/`. Cleanup failures must produce a nonzero exit code. Keep each role's separate entry point and update the structure declaration when composition changes.
