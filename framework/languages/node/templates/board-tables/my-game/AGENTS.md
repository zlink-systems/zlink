# MyGame agent guidelines

This project is a runnable server starting point for a board-table game: a Session node and Table nodes that share a Redis Location Store. `zlink.structure.json` describes the initial composition; Redis and the Framework own the current runtime locations.

## Node responsibilities

| Node            | Owned responsibility                                                                                         |
| --------------- | ------------------------------------------------------------------------------------------------------------ |
| Session         | Terminate STREAM, verify the identity in the first Join, GetOrCreate and bind the Actor, relay, SessionInfo. |
| Table           | Entry Spot, Table User Spot, Player Actor host.                                                              |
| Table User Spot | Seats, chips, deck, hands, turn, hand number, stake, settlement.                                             |
| Player Actor    | Public Actor identity and the bound Session; holds no copy of game state.                                    |
| Redis           | Framework Location and Relocation stores.                                                                    |

The Actor identity is answered as SessionInfo from the public ref of the Session binding. The Table owns only game state. A request reply is sent directly to the bound Session of the Actor that received it; state pushes go to the Actor by the seat's playerId. TableView, HandDealt, and HandResult use three public registrations and one delivery handler implementation. HandDealt is sent only at hand start and in replies to reconnection and TableViewRequest.

## Extension points

Change the game rules in `server/table/domain/table-state.ts`. Infrastructure owns the public callbacks and I/O. Manage DTOs in `server/shared/messages.ts`, and stable types and packet names in `server/shared/constants.ts`. `server/session/main.ts` and `server/table/main.ts` compose the builder and provider of each role. `server/shared/hosting.ts` owns the Location and Relocation stores, diagnostics, and the READY, drain, and shutdown controls.

## Public API boundaries

- Use only the public Framework and binding packages.
- The Framework owns route, lease, membership, queue, and binding state; do not put it in the application snapshot.
- The Table uses the SpotWide execution gate. Do not compensate for relocation or lifecycle failures with extra locks, queues, retries, or sleeps.
- Use the Framework default typed JSON serializer for messages.
- The Table snapshot holds only application state. The Player relocation adapter uses an empty payload because it has no application state. When moving to another node, the Framework preserves the Actor ID, generation, membership, and binding.

## Verification

Run `./run-e2e.sh --build-only` and `./run-e2e.sh` for Docker verification. On Windows use `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly` and `pwsh -NoProfile -File ./run-e2e.ps1`. The default runs the runner and nodes in Docker with unique project resources. Preserve stdin/stdout lifecycle control and inspect `.logs/docker-<project>/` as well as `.logs/run-<id>/`. Use `--local` or `-Local` for local process debugging; Node.js and npm are required in that mode. Keep resource limits in the validation environment rather than template scripts.

For local development, run `npm ci && npm run build`, `npm run typecheck`, `npm run lint`, and `npm run format:check`. Windows `run-e2e.ps1` and Linux `run-e2e.sh` perform the actual structure check and the public connector e2e. Verification runs the full e2e once and the `table,reconnect` selection twice. Preserve the first failure and the message flow; do not ignore an intermittent failure because a rerun passed.

Do not fix or work around Framework defects in this template. After checking the usage against the public guide and samples, report the reproduction, the first failure log, and the relevant source locations. Cleanup is limited to the processes and Compose project owned by the runner.

Write code comments in English and reports in formal technical Korean.
