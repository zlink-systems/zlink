# MyGame channel town

This C++20 template implements two town channels and party-owned dungeon instances using the public ZLink Framework packages. Keep this directory independently downloadable.

## Node responsibilities

| Role | Responsibility | Extension points |
| --- | --- | --- |
| Session | STREAM connection, Player GetOrCreate, binding and relay | `server/session/session.hpp` |
| Town | Entry admission, fixed Town Spots, parties and channel travel | `server/town/domain.hpp`, `server/town/town.hpp` |
| Instance | Dynamic Dungeon Spots, party admission and stage | `server/instance/domain.hpp`, `server/instance/dungeon.hpp` |
| Shared | Typed contracts, stateless Player, Redis hosting and node control | `server/shared/` |

## Public API boundaries

Use typed messages and Framework's default JSON serializer. Request handlers reply through the received Actor's bound Session. Other pushes address player IDs through the public Actor client; Spot delivery handlers forward the received typed message to the bound Session. Spots retain player IDs, never Actor references.

Town owns players and parties. Dungeon owns its party and stage. Relocation adapters capture only application state; Framework owns identity, routes, leases, membership and execution gates. Keep application rules in domain files and transport callbacks in the Spot and Session files.

Use public APIs only. Do not introduce internal APIs, reflection, raw frames, manual message codecs, transport retries, fixed sleeps, expanded deadlines, additional execution locks or weakened assertions to compensate for Framework defects. Record public-package failures with the first node error and message-flow record.

## Changes and verification

Preserve the `channel`, `dungeon`, `reconnect`, `update` scenario order and the structure checker. Build and run on Windows and Linux. An intermittent failure is a defect even if a later execution passes. Every successful environment requires three complete e2e runs. Logs and per-run configurations remain under `.logs/run-<id>/`.

Run formatting only inside this template directory. The runner owns only the processes it starts and the uniquely named Compose project it creates. Cleanup failures must produce a nonzero exit code.
