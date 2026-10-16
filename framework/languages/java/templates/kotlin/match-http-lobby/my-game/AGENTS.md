# Template instructions

This Kotlin template implements an HTTP matchmaking lobby and ZLink STREAM duels with relocatable Room User Spots.

## Node responsibilities

| Node | Responsibility | Extension point |
| --- | --- | --- |
| lobby | Pair same-mode HTTP requests and prepare Rooms | `server/lobby/.../application/Matchmaker.kt` and infrastructure adapters |
| session | GetOrCreate Player Actors, bind and relay; report SessionInfo | `server/session/.../Program.kt` |
| matchA / matchB | Entry admission, Room state and Player delivery | `server/match/.../domain/RoomState.kt`, `infrastructure/zlink/Room.kt`, `infrastructure/zlink/Actions.kt` |
| location | Redis Location and relocation stores | Shared `Hosting.kt` and `docker-compose.yml` |
| players | HTTP and public STREAM connector verification | `e2e/.../Scenarios.kt` |

## Public API boundaries

Use published ZLink APIs and the default typed JSON serializer. Replies from an Actor request handler go directly to that Actor's bound session. Other pushes use player IDs through the Actor send API and the Room delivery handler forwards to the bound session. Room stores player IDs, never Actor references. Session binding supplies Actor identity; RoomView contains game state only. Relocation adapters serialize application state only.

Do not add internal APIs, reflection, raw-frame parsing, manual message codecs, retries, sleeps, larger deadlines or weaker assertions to compensate for Framework failures. Keep domain decisions in RoomState and matching decisions in Matchmaker. Framework owns routing, membership, execution serialization and relocation.

## Changes and verification

Keep the canonical scenario order `lobby,hit,reconnect,update` in the runner and structure file. Run the structural check before node startup and preserve `.logs/run-<id>/` files. Verify Windows and Linux builds and e2e. A single intermittent failure is a defect; retain its first failure and message-flow records. Use only the runner's own Compose project and child processes for cleanup. Maintain the shared hosting and control protocol rather than duplicating it per role.
