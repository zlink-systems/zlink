This template implements one megaserver world with capacity-based Region instances, persistent Player Actors and a separate Dungeon host. Extend game rules in the Region domain; preserve the public Framework boundaries below.

## Node responsibilities

| Role           | Responsibility and extension location                                                                                                                                                                 |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| session        | STREAM binding and relay in `server/session/session.ts`.                                                                                                                                              |
| region         | Entry selection in `server/region/infrastructure/zlink/spots.ts`; admission and movement in `server/region/domain/positions.ts`; packet handlers in `server/region/infrastructure/zlink/handlers.ts`. |
| instance       | Dungeon creation and information replies in `server/instance/infrastructure/zlink/spots.ts`.                                                                                                          |
| location-store | Redis infrastructure in `docker-compose.yml`.                                                                                                                                                         |

## Public API boundaries

Use published Framework packages and the default typed JSON serializer. Replies inside Actor packet handlers go directly to the received Actor's bound Session. Other pushes use player IDs with the public Actor send API; the Region delivery handler forwards to the bound Session. Never retain Actor references in Region state. SessionInfo derives identity from the bound Actor reference.

Region owns membership, positions and sequence validation. Entry owns index selection after an explicit `region-full` rejection. Relocation adapters serialize application state only. Framework owns routing, leases, membership infrastructure, queues and execution gates. Do not add internal APIs, reflection, raw frames, per-message codecs, sleeps, transport retries or larger deadlines to compensate for Framework defects. Preserve the #1731 packet class restoration table until the published fix is available.

## Changes and verification

Run `./run-e2e.sh --build-only` and `./run-e2e.sh` for Docker verification. On Windows use `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly` and `pwsh -NoProfile -File ./run-e2e.ps1`. The default runs the runner and nodes in Docker with unique project resources. Preserve stdin/stdout lifecycle control and inspect `.logs/docker-<project>/` as well as `.logs/run-<id>/`. Use `--local` or `-Local` for local process debugging; Node.js and npm are required in that mode. Keep resource limits in the validation environment rather than template scripts.

Keep `region,dungeon,reconnect,update` and the structure declaration consistent. Run build, typecheck, lint, formatting and e2e after changes. Structure checks read actual files and reject inconsistent declarations. Verify both valid and invalid structure inputs outside permanent test code. A single intermittent failure is a defect; preserve the first failure and message-flow stderr records under `.logs/run-<id>/`. Report Framework defects with reproduction and public API evidence.
