# Project instructions

This template implements a seamless MMO world with public ZLink Node.js APIs. Keep the application independent of Framework repository sources.

## Node responsibilities

| Role           | Responsibility                                                          | Game extension location                                                   |
| -------------- | ----------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| Session        | STREAM lifecycle, Actor creation and binding, packet relay, SessionInfo | `server/session/session.ts`                                               |
| Zone           | Player state, Zone admission, visibility and border publication         | `server/zone/player.ts`, `server/zone/zone.ts`, `server/zone/handlers.ts` |
| Instance       | Dungeon User Spot creation and requests                                 | `server/instance/main.ts`                                                 |
| Shared hosting | Redis stores, diagnostics, READY and drain/shutdown protocol            | `server/shared/hosting.ts`                                                |

## Public API boundaries

Use typed messages and the default Framework JSON serializer. Reply to the requesting Actor through the handler's bound Session. For other pushes, address an Actor by playerId and forward the received packet through that Actor's bound Session. Do not retain Actor references in Spot state. SessionInfo is the source of Actor identity; WorldState carries only game state and current host information.

Player owns movement state. Zone visibility is an application projection, not a second movement authority. Relocation adapters serialize application state; Framework owns routes, membership, timers, queues and Session bindings. Use public deferred join, public host relocation and runtime observation. Do not add raw frames, private APIs, reflection, custom message codecs, retry loops, sleeps or increased deadlines to compensate for Framework defects. Reconstruct public DTO classes before forwarding typed handler fields (#1731).

## Changes and verification

Keep `cross,border,reconnect,update` in the declared order. Maintain configuration and fixed Zone consistency through the structure checker. Verify valid and invalid structure inputs during development; do not embed checker self-tests in the template. Run build and e2e on Windows and Linux. Any intermittent isolated failure is a defect. Preserve `.logs/run-<id>/` and inspect message-flow records before adding diagnostics. Report Framework failures separately and retain assertions. Every process and Compose project created by a run belongs to that run; cleanup must not affect other runs.
