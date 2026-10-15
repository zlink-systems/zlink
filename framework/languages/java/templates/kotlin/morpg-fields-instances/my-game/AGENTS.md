# Project instructions

This Kotlin template implements a MORPG with shared Fields and party Dungeon instances across Session, Field and Instance processes. Extend game rules through the public ZLink Framework API.

## Node responsibilities

| Role | Responsibility and extension location |
| --- | --- |
| Session | Stream connections, Actor creation and binding; `server/session` |
| Field | Player movement, Field admission, border visibility and local parties; `server/field` |
| Instance | On-demand Dungeon User Spots, party admission and stage progression; `server/instance` |
| Shared | Contracts, Redis stores, diagnostics, host control; `server/shared` |
| E2E | Public stream client, process orchestration and assertions; `e2e` |

## Public API boundaries

Player owns authoritative movement state. Field owns local parties; Dungeon owns its prepared party and stage. Field stores a visibility projection keyed by player ID, never Actor references. Request handlers reply through the supplied Actor's bound Session. Other pushes use Actor send and a delivery handler that forwards to the bound Session. SessionInfo derives Actor identity from the Session binding; game-state messages contain no binding identity.

Use typed messages and the default Framework JSON serializer. Relocation adapters serialize application state only. Framework owns routes, membership, leases, queues, connection recovery and execution gates. Do not introduce internal APIs, reflection, raw frames, manual message codecs, sleeps, retries, larger deadlines or weaker assertions to hide Framework failures.

## Changes and verification

Keep `field`, `dungeon`, `reconnect` and `update` in that order. Update configuration and `zlink.structure.json` together. Run the structure checker and the selected e2e scenarios after changes. A single intermittent failure is a defect. Preserve the first failure and message-flow records in `.logs/run-<id>/`; do not treat a later successful run as a correction.
