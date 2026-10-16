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

Use `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` for the default Docker build and e2e. Build-only uses `--build-only` or `-BuildOnly`. For local process debugging, use `--local` or `-Local`; add `--no-build` or `-NoBuild` to reuse a local build. Preserve stdin/stdout lifecycle control in both modes. Keep Docker build and cleanup logs in `.logs/docker-<project>/` and node logs in `.logs/run-<id>/`. Cleanup must remove only this run's containers, network and image tags, and must fail the run on errors.

Preserve the `channel`, `dungeon`, `reconnect`, `update` scenario order and the structure checker. Build and run on Windows and Linux. An intermittent failure is a defect even if a later execution passes. Every successful environment requires three complete e2e runs. Logs and per-run configurations remain under `.logs/run-<id>/`.

Run formatting only inside this template directory. The runner owns only the processes it starts and the uniquely named Compose project it creates. Cleanup failures must produce a nonzero exit code.

Town and Instance register the shared application Entry and first-admission handler. Declare one Entry for each of these nodes and none for roles without application Entry registration; Framework default Entries are not part of the structure. Keep dynamic Dungeon declarations complete for every owning Instance. Preserve one Dungeon slot per Instance in the e2e configuration; it makes replacement placement deterministic. Keep the Message Follow duration and derived SafeToShutdown observation budget under their shared lifecycle constants.

Bootstrap Town providers sequentially, waiting for each READY before starting the next. Set each Town provider's public stable-type capacity to exactly its configured fixed Town count. Earlier providers are full and later providers are absent during bootstrap, so only the declared owner can create its fixed Towns. The structure checker enforces the exact capacity.
