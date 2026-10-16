# Project instructions

This template implements isolated MMO worlds with fixed channel Zones, persistent
Player gold, an HTTP world directory and STREAM Sessions.

## Node responsibilities

| Role     | Responsibility and extension point                                                                      |
| -------- | ------------------------------------------------------------------------------------------------------- |
| accounts | HTTP world directory in `server/accounts/main.ts`.                                                      |
| session  | Actor creation, Session binding and relay in `server/session/session.ts`.                               |
| channel  | Entry admission, Zone handlers and Player state in `server/channel/`. Extend game rules in `domain.ts`. |
| world-db | Separate Redis services in `docker-compose.yml`; Player data is owned by `data.ts`.                     |
| players  | Public HTTP and STREAM verification in `e2e/client.mts`.                                                |

## Public API boundaries

Use public Framework APIs and the default typed JSON serializer. Reply to a
request through the received Actor's bound Session. Other pushes go through
Actor send and the Zone delivery handler. Spots must not retain Actor references.
The packet-name-to-class table restores forwarded DTO classes for #1731.

Player Actor owns gold; Redis persists it. Relocation adapters capture only
application state. Framework owns routing, leases, membership, Session binding
and relocation completion. Keep worlds isolated by mesh and Redis endpoint.
Do not use internal APIs, reflection, raw frames, manual message codecs, sleeps,
retries, increased deadlines or weaker assertions to bypass Framework defects.

## Changes and verification

Keep the ordered `worlds`, `channel`, `reconnect`, `update` scenarios and the
structure manifest consistent. Run build, lint, structure checking and e2e on
Windows and WSL. An intermittent failure is a defect; retain the first failure
and message-flow evidence in `.logs/run-<id>/`. The runner owns only its spawned
processes and uniquely named Compose project. Cleanup failures must fail the run.
