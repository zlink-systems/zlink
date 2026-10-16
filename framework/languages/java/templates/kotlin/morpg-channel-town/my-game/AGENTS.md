# Project instructions

This Kotlin template implements MORPG channel towns and party dungeons with public ZLink Framework APIs.

## Node responsibilities

| Node | Responsibility and extension location |
| --- | --- |
| session | STREAM, Actor GetOrCreate, bind and relay: `server/session` |
| townA, townB | Entry, fixed Town, party membership and channel moves: `server/town` |
| instanceA, instanceB | Dynamic Dungeon preparation, admission and progress: `server/instance` |
| location | Redis Location and Relocation stores: `docker-compose.yml` |
| players | Public stream connector client and runner: `e2e` |

## Public API boundaries

Use typed messages and the Framework default JSON serializer. A response inside an Actor handler goes directly to that Actor's bound Session. Other pushes use player IDs and the public Actor send API; the receiving Spot handler forwards the message to the bound Session. Spots do not retain Actor references. SessionInfo reads the current binding. Town owns channel and party state; Dungeon owns party members and stage. Relocation adapters serialize application state only.

Use public GetOrCreate, deferred Actor joins, host relocation, runtime observation, and stream connector APIs. Do not implement route, lease, membership, queue, reconnect, or generation policies. Do not add internal APIs, reflection, raw frames, manual message codecs, sleeps, retries, or larger deadlines to conceal failures.

## Changes and verification

Keep `zlink.structure.json`, configuration, server registrations and the ordered channel, dungeon, reconnect, update scenarios consistent. Run the build and e2e on Windows and Linux. Any intermittent failure is a defect; preserve the first failure and investigate message-flow records in `.logs/run-<id>/`. Structure validation examines real files. Preserve cleanup errors and fail the run when cleanup fails. Keep README and this file in English, with a short Korean section only at the end of README; write code comments in English.

Every Object Server that registers Player must register the shared Entry and first-admission handler. Keep Entry and dynamic Dungeon declarations complete for every owning node. Preserve one Dungeon slot per Instance in the e2e configuration; it makes replacement placement deterministic. Keep the Message Follow duration and derived SafeToShutdown observation budget under their shared lifecycle constants.
