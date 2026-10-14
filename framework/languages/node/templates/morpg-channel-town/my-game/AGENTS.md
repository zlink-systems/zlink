# Project instructions

This template implements MORPG town channels and party Dungeons with public ZLink Framework APIs. Extend game rules in the owning Spot rather than Session routing or infrastructure.

## Node responsibilities

| Role     | Responsibility and extension location                                            |
| -------- | -------------------------------------------------------------------------------- |
| session  | STREAM, Player GetOrCreate, bind and relay in `server/session/session.ts`.       |
| town     | Fixed Town, party rules and Entry admission in `server/town/`.                   |
| instance | Dynamic Dungeon, member admission and stage in `server/instance/`.               |
| shared   | Contracts, stateless Player, diagnostics and hosting in `server/shared/`.        |
| e2e      | Public STREAM client, scenarios, process control and structure checks in `e2e/`. |

## Public API boundaries

Use typed messages and the Framework default JSON serializer. Requests reply through the handler Actor's bound Session. Other pushes use player IDs and public Actor sends; the receiving Spot delivery handler forwards to the bound Session. Spots never retain Actor references. The packet-name-to-class delivery table preserves declared DTO classes for issue #1731.

Town owns players and parties. Dungeon owns members and stage. Player has no application state. Relocation adapters serialize only these application facts. Framework owns identity, binding, placement, relocation and execution gates. Do not use internal APIs, reflection, raw frames, custom message codecs, sleeps, retries or longer deadlines to hide failures.

## Changes and verification

Keep channel, dungeon, reconnect and update scenarios and their order. Update contracts, structure and configuration together when changing composition. Run build, typecheck, lint, format checks and e2e on Windows and Linux. A single intermittent failure is a defect. Preserve first-failure message-flow logs in `.logs/run-<id>/`; do not weaken assertions. Cleanup only the runner's own processes and Compose project.
