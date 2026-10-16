# Project instructions

This template implements an Entry Spot lobby and server-authoritative duel rooms for action and FPS games using public ZLink Framework APIs.

## Node responsibilities

| Role           | Responsibility and extension point                                                                                                                          |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| session        | `server/session/session.ts` creates and binds Player Actors, relays requests, and reports binding identity.                                                 |
| match          | `server/match/domain.ts` owns matchmaking and combat rules; `spots.ts` integrates them with Matchmaking and Room User Spots. `main.ts` composes the server. |
| location-store | Redis supplies Framework Location and Relocation stores.                                                                                                    |

## Public API boundaries

Use typed messages and the default Framework JSON serializer. Session binding owns Actor identity. Matchmaking owns the queue and room sequence; Room owns positions, HP, action sequences, and winner. Entry and Player retain no application state. Relocation adapters serialize application state only.

Reply to a requesting Actor through its bound session. Other pushes use player IDs and the public Actor send API, then the Room delivery handler forwards them to the bound session. Spots must not retain Actor references. Keep the packet-name-to-class table in `handlers.ts` when forwarding typed packets (#1731).

Do not add internal APIs, reflection, raw frames, custom message codecs, transport retries, fixed sleeps, deadline increases, or weaker assertions to compensate for Framework defects. Record the first failure and message-flow log instead. Framework owns routing, binding, execution gates, and relocation continuity.

## Changes and verification

Keep `zlink.structure.json`, role entrypoints, configuration, README commands, and the ordered `hit,reconnect,update` scenarios consistent. Each run validates structure before starting isolated Redis and server processes. Logs are retained under `.logs/run-<id>/`; cleanup errors fail the run. An intermittent failure is a defect even if a later run passes.

Run the build, lint, structure check, and relevant e2e scenarios on Windows and Linux. Do not place self-test mutations in the structure checker. Verify invalid structures in a disposable development copy.
