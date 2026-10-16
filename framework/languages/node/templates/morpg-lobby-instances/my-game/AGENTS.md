# Project instructions

This template implements MORPG parties in a lobby and relocatable dungeon instances using public ZLink Framework APIs.

## Node responsibilities

| Role           | Responsibility and extension points                                                                                              |
| -------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| Session        | `server/session/session.ts`: Player GetOrCreate, binding, SessionInfo, and relay.                                                |
| Lobby          | `server/lobby/domain.ts`: party rules. `spots.ts` and `handlers.ts`: Party admission, readiness, dungeon preparation and travel. |
| Instance       | `server/instance/dungeon.ts`: party admission and stage. `handlers.ts`: typed requests and delivery.                             |
| Shared hosting | `server/shared/hosting.ts`: stores, listener readiness, relocation observation, and shutdown.                                    |
| Client         | `e2e/client.mts`: public STREAM scenarios and assertions.                                                                        |

## Public API boundaries

Use public APIs and the default typed JSON serializer. Reply inside a request handler through the received Actor's bound Session. Other pushes address player IDs through Actor send and the current Spot's delivery handler. Spots never retain Actor references. Session binding alone owns Actor identity; game-state DTOs must not copy Session identity or generation.

Party owns party membership and readiness. Dungeon owns its admitted party and stage. Relocation adapters capture application state only. Framework owns routes, leases, membership, queues, execution gates, and relocation. Do not compensate for Framework errors through internal APIs, reflection, raw frames, manual codecs, sleeps, retries, larger deadlines, or weaker assertions. Delivery handlers restore declared DTO classes using their packet table until #1731 is fixed.

Lobby and Instance register the shared application Entry and first-admission handlers. Declare exactly one Entry for each node with either role; omit Framework infrastructure Entry instances on other roles. Keep party state in the Party User Spot. Structure checks must reject missing Entry owners and missing dynamic Party or Dungeon types. Keep the one-Dungeon instance capacity that makes replacement admission deterministic.

## Changes and verification

Keep `party`, `reconnect`, and `update` scenarios in their declared order. Run build, typecheck, lint, format checks, structure validation, and the affected e2e scenarios. Validate on Windows and Linux; an intermittent failure remains a defect. Inspect the first failure and message flow in `.logs/run-<id>/` before changing code. The checker validates real project files; temporary negative fixtures belong outside the template.

Code comments are English. Keep project tokens `my-game` and `MyGame` for Initializr substitution. Update structure declarations and README commands with composition changes. Keep the Framework versions in `package.json` and do not commit local package paths or validation caches.
