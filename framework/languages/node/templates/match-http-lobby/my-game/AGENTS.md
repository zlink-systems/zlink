# Project instructions

This template implements an HTTP lobby and server-authoritative duel rooms for action and FPS games through public ZLink Framework APIs.

## Node responsibilities

| Role           | Responsibility and extension point                                                                                                                                             |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| http-lobby     | `server/lobby/matchmaker.ts` owns mode queues, pending participants, and room sequence. `http.ts` prepares Rooms through the public object client and serves HTTP assignments. |
| session        | `server/session/session.ts` creates and binds Player Actors, relays requests, and reports binding identity.                                                                    |
| match          | `server/match/domain.ts` owns combat rules. `spots.ts` owns Room state integration; `handlers.ts` handles Entry admission and Actor delivery.                                  |
| location-store | Redis supplies Framework Location and Relocation stores.                                                                                                                       |

## Public API boundaries

Use typed messages and the default Framework JSON serializer. Session binding owns Actor identity; game state contains only player IDs. Lobby owns matchmaking and is not relocated. Room owns positions, HP, action sequences, and winner. Entry and Player retain no application state. Relocation adapters serialize application state only.

Reply to the requesting Actor through its bound session. Other pushes use player IDs and the public Actor send API; the Room delivery handler forwards them to the bound session. Spots must not retain Actor references. Retain the packet-name-to-class table when forwarding typed pushes (#1731).

Do not add internal APIs, reflection, raw frames, custom message codecs, transport retries, fixed sleeps, deadline increases, or weaker assertions to compensate for Framework defects. Preserve the first failure and message-flow log. Framework owns routing, binding, execution gates, and relocation continuity.

## Changes and verification

Keep the structure, role entrypoints, configuration, README commands, and ordered `lobby,hit,reconnect,update` scenarios consistent. Each run validates the structure before creating isolated Redis and server processes. Logs remain in `.logs/run-<id>/`; cleanup errors fail the run. An intermittent failure remains a defect even if a subsequent run passes.

Run build, lint, structure checks, and relevant e2e scenarios on Windows and Linux. Check invalid structures in a disposable development copy; do not put self-test mutations into the structure checker.
