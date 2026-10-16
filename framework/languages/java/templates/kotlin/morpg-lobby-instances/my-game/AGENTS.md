# Project instructions

This template implements a MORPG lobby with parties and dynamically allocated dungeon User Spots through public ZLink Framework Kotlin APIs.

## Node responsibilities

| Node | Responsibility and extension point |
| --- | --- |
| `session` | STREAM connections, Player GetOrCreate and binding, typed relay, SessionInfo. Extend `server/session/.../Program.kt`. |
| `lobby` | Entry admission and Party User Spots. Extend `PartyState.kt` for domain rules and `Spots.kt` / `Handlers.kt` for Framework integration. |
| `instanceA`, `instanceB` | Dungeon preparation, admission, stage and relocation state. Extend `server/instance/.../Dungeon.kt` and `Handlers.kt`. |
| `location` | Redis Location and Relocation stores, configured by shared hosting. |

## Public API boundaries

Use public typed messages, coroutine APIs, and the default Framework JSON serializer. Request handlers send responses directly to the supplied Actor's bound Session. Other pushes address Actors by player ID and their delivery handlers forward to the bound Session. Spots never retain Actor references. Session binding owns Actor identity; game views do not duplicate it.

Party owns roster, leadership order and readiness. Dungeon owns the prepared party and stage. Entry owns no party state. Relocation adapters serialize application state only; do not duplicate Framework membership, routes, leases, queues, or placement decisions. Keep game rules independent of hosting. Do not add internal APIs, reflection, raw frame handling, manual message codecs, sleep, retry, deadline increases, or relaxed assertions to compensate for Framework defects.

Lobby and Instance register the shared application Entry and first-admission handlers. Declare exactly one Entry for each node with either role; omit Framework infrastructure Entry instances on other roles. Keep party state in the Party User Spot. Structure checks must reject missing Entry owners and missing dynamic Party or Dungeon types. Keep the one-Dungeon instance capacity that makes replacement admission deterministic.

## Changes and verification

Keep `party`, `reconnect`, and `update` in that order in the runner and structure file. Preserve the shutdown and replacement assertions. Validate the structure before starting nodes. Run the relevant domain tests and e2e on Windows and Linux. A single intermittent failure is a defect; retain its first message-flow and node logs in `.logs/run-<id>/`.

Change only resources belonging to this project. The runner owns its unique Compose project and node processes; never stop unrelated resources. Do not commit `.logs/`, build output, package caches, or `zlink.license`.
