# Project guidelines

This C++ megaserver template runs persistent player Actors in capacity-limited Region User Spots, with a Session gateway and a separate Dungeon host.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| Session | `server/session/session.hpp`: Actor GetOrCreate, binding, identity inspection and relay. |
| Region | `server/region/domain/positions.hpp`: game state and capacity. `server/region/infrastructure/zlink`: Entry selection, handlers and application relocation adapters. |
| Instance | `server/instance/dungeon.hpp`: Dungeon User Spot handlers. |
| Shared host | `server/shared/hosting.hpp`: stores, diagnostics, readiness, drain and shutdown protocol. |

## Public API boundaries

Use published packages and public typed JSON APIs. Entry alone selects the next instance after Region rejects capacity with `region-full`. Region alone owns membership, position and sequence. Never replicate Framework route, lease, binding or execution state in application adapters.

A request handler replies through the received Actor's bound Session. Other pushes target stored player IDs through the Actor client and the Region delivery handler forwards them to bound Sessions. Never store Actor references in Region state. SessionInfo comes from the actual Session binding. C++ lacks typed Session reply; retain the documented typed send/wait exchange until that public contract exists.

Use public host relocation and SafeToShutdown observation. Report missing contracts and Framework defects with logs; do not add internal APIs, reflection, raw frames, manual message codecs, sleeps, retries, extended deadlines or relaxed assertions.

## Changes and verification

Keep the scenario order `region,dungeon,reconnect,update` in the runner and structure metadata. Validate actual files before starting Docker. Exercise structure changes with valid and invalid temporary copies outside tracked files. Keep build and formatter commands inside this project.

Run build and e2e on Windows and Linux. Treat every intermittent failure as a defect. Preserve `.logs/run-<id>/` including message-flow output. Cleanup must affect only the deployment's own processes and Compose project; cleanup failure returns exit code 1.
