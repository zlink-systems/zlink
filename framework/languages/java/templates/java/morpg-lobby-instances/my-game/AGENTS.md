# Project guidelines

This template implements a Java MORPG lobby with Party User Spots and Dungeon User Spots using published ZLink Framework packages.

## Node responsibilities

| Role | Responsibility and extension point |
|---|---|
| session | STREAM login, Player Actor GetOrCreate, binding, relay; extend `GameSession` for authentication. |
| lobby | Entry admission and Party state; extend `PartyState`, `Party`, and their typed handlers. |
| instance | Dungeon admission and stage; extend `DungeonState`, `Dungeon`, and their typed handlers. |
| shared | Typed contracts, stateless Player, stores, hosting, control protocol, and version-independent names. |
| e2e | Public connector clients, composition validation, process ownership, and scenario assertions. |

## Public API boundaries

Use public APIs and default typed JSON serialization. Reply to a requester through the handler Actor's bound Session. Send other pushes by player ID through the Actor client; the destination Actor handler forwards to its bound Session. Never retain Actor references in Spot game state. Read Actor identity from the Session binding. Preserve only application state in relocation adapters.

Do not add internal APIs, reflection, raw frames, manual message codecs, runtime routing state, second queues or locks around the execution gate, sleeps, retries, or enlarged deadlines to conceal Framework defects. Report the first failure with message-flow logs and the related public operation.

## Changes and verification

Keep `party`, `reconnect`, and `update` and their order in the structure declaration and runner. Preserve actual response/event assertions, Actor generation checks, and replacement placement checks. Validate structure changes against real files, including negative mutations outside the committed template. Run builds and e2e on Windows and Linux; one intermittent failure remains a defect.

Keep node-role entry points separate and shared hosting in one place. Keep package versions centralized. Run formatters and installation commands from this template directory only. Logs and effective run configurations belong in `.logs/run-<id>/`. Cleanup must affect only processes and Compose projects created by that run and must report failures.
