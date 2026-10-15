# Project instructions

This Java MORPG template provides fixed town channels, parties, and dynamic dungeons using public ZLink Framework APIs.

## Node responsibilities

| Node | Responsibility and extension point |
|---|---|
| session | STREAM login, Actor GetOrCreate, binding and relay in GameSession |
| townA, townB | Entry, Player Actors, fixed Town Spots; channel and party rules in TownState |
| instanceA, instanceB | Dynamic Dungeon Spots; roster and stage in DungeonState |
| location | Redis Location and Relocation stores |

## Public API boundaries

Use typed messages and the default Framework JSON serializer. Reply to the requester through the handler Actor's bound Session. Send other pushes through player IDs and Actor delivery handlers. Spots retain no Actor references. SessionInfo reads identity from the Session binding. Relocation adapters preserve only application state. Framework owns routes, memberships, leases, queues, scheduling, placement and relocation. Do not use internals, reflection, raw frames, manual codecs, sleeps, retries, or deadline increases to compensate for Framework failures.

## Changes and verification

Keep channel, dungeon, reconnect and update scenarios and their order. Run the structure check and e2e runner after changes. Inspect `.logs/run-<id>/` node stderr, including message-flow diagnostics. A single intermittent failure is a defect. Cleanup errors must fail the run. Keep public dependency versions centralized in build.gradle.kts. README and AGENTS are English; code comments are English. The final README section provides brief Korean execution instructions.
