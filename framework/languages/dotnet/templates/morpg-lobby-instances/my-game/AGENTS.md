# Project instructions

This template provides a MORPG lobby with dynamic Party and Dungeon User Spots.

## Node responsibilities

| Role | Responsibility and extension location |
| --- | --- |
| Session | STREAM, Player Actor creation/binding and relay; `server/MyGame.Session` |
| Lobby | Party admission; shared Entry login in `server/MyGame.Shared/LobbyEntry.cs`; party rules in `Domain/PartyState.cs` |
| Instance | Dungeon roster and stage; `server/MyGame.Instance/DungeonSpot.cs` |
| Shared | Contracts, stateless Player Actor, hosting and stdin control; `server/MyGame.Shared` |

## Public API boundaries

Use public ZLink APIs and the default typed JSON serializer. Request handlers reply through the received Actor's bound Session. Other pushes use player IDs and Actor send, then a concrete Spot delivery handler forwards to the bound Session. Spots never retain Actor references. SessionInfo is read from the bound Actor reference. Relocation adapters serialize application state only. Entry has no party state. Framework owns routing, leases, membership, reconnect and execution gates; do not add locks, queues, retry loops, internal API calls, reflection, raw frames or manual message codecs.

Lobby and Instance register the shared application Entry and first-admission handlers. Declare exactly one Entry for each node with either role; omit Framework infrastructure Entry instances on other roles. Keep party state in the Party User Spot. Structure checks must reject missing Entry owners and missing dynamic Party or Dungeon types. Keep the one-Dungeon instance capacity that makes replacement admission deterministic.

## Changes and verification

Preserve `party,reconnect,update` and their order in the runner and structure manifest. Run the structure check before node startup and use the public STREAM client. A single intermittent failure is a defect. Run-specific configurations and node/message-flow logs are in `.logs/run-<id>/`. Cleanup failures return exit code 1. Verify Windows and Linux builds and e2e before deployment.
