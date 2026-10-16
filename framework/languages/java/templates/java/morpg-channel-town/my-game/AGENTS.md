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

Town and Instance register the shared application Entry and first-admission handler. Declare one Entry for each of these nodes and none for roles without application Entry registration; Framework default Entries are not part of the structure. Keep dynamic Dungeon declarations complete for every owning Instance. Preserve one Dungeon slot per Instance in the e2e configuration; it makes replacement placement deterministic. Keep the Message Follow duration and derived SafeToShutdown observation budget under their shared lifecycle constants.

Bootstrap Town providers sequentially, waiting for each READY before starting the next. Set each Town provider's public stable-type capacity to exactly its configured fixed Town count. Earlier providers are full and later providers are absent during bootstrap, so only the declared owner can create its fixed Towns. The structure checker enforces the exact capacity.
