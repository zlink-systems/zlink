# Project instructions

This template implements a MORPG lobby with Party and Dungeon User Spots on the public ZLink C++ API.

## Node responsibilities

| Node | Responsibility and extension location |
| --- | --- |
| session | STREAM login, Actor creation and binding, relay; `server/session/` |
| lobby | Stateless Entry and Party membership/readiness; `server/lobby/` |
| instanceA, instanceB | Dungeon preparation, admission and stage; `server/instance/` |
| location | Redis Location and relocation storage |

## Public API boundaries

Use typed messages and the default JSON serializer. Replies to handler requests use the received Actor's bound session. Other pushes use player IDs and the public Actor send API; the receiving Spot forwards them to the bound session. Spots never retain Actor references. SessionInfo reads identity from the binding. Party and Dungeon own their application state; relocation adapters serialize only that state.

Do not add internal APIs, reflection, raw frame parsing, manual codecs, transport policy, retries, fixed sleeps or relaxed assertions. A public package defect must remain a documented blocker. Framework owns execution gates, routing, membership, binding and relocation.

Lobby and Instance register the shared application Entry and first-admission handlers. Declare exactly one Entry for each node with either role; omit Framework infrastructure Entry instances on other roles. Keep party state in the Party User Spot. Structure checks must reject missing Entry owners and missing dynamic Party or Dungeon types. Keep the one-Dungeon instance capacity that makes replacement admission deterministic.

## Changes and verification

Keep the party, reconnect and update scenarios and structure checks. Build and run on Windows and Linux. Any intermittent failure is a defect. Logs and generated configurations are under `.logs/run-<id>/`. The runner owns only its child processes and its unique Compose project. Validate structure checks against both valid files and temporary invalid copies outside committed sources.
