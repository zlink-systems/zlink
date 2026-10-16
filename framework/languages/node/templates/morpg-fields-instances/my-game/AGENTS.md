# Project instructions

This Node.js MORPG template implements public fields, same-field parties and dynamically created dungeons with the public ZLink Framework API.

## Node responsibilities

| Role     | Responsibility                                                                | Extension point                                                                        |
| -------- | ----------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| Session  | STREAM, Actor GetOrCreate, binding, relay and SessionInfo                     | `server/session/session.ts`                                                            |
| Field    | Fixed Field User Spots, Entry Spot, movement, border publications and parties | `server/field/domain.ts`, `server/field/spots.ts`, `server/field/handlers.ts`          |
| Instance | Dungeon User Spots, member admission and stage                                | `server/instance/domain.ts`, `server/instance/spots.ts`, `server/instance/handlers.ts` |
| Shared   | Contracts, Player state, diagnostics and lifecycle control                    | `server/shared/`                                                                       |
| Redis    | Public Location and Relocation stores                                         | `docker-compose.yml`                                                                   |

## Public API boundaries

Use typed messages with the Framework's default JSON serializer and public STREAM connector. SessionInfo comes from the bound Actor reference, never from game-state messages. A request handler replies through the supplied Actor's bound Session. Other pushes use player IDs and public Actor send, followed by a Spot Actor delivery handler that forwards to the bound Session. Keep the packet-name-to-class table for #1731 when forwarding typed messages.

Spots do not retain Actor references. Each application decision has one state owner; adapters capture application state only. Framework owns routes, leases, membership, queues, execution gates and reconnect behavior. Do not add internal API calls, reflection, raw frames, manual message codecs, retry loops, fixed sleeps, increased deadlines or weakened assertions to compensate for Framework defects.

## Changes and verification

Keep `zlink.structure.json`, shared constants, configuration and the ordered scenarios `field,dungeon,reconnect,update` consistent. Validate the structure before starting processes. Run build, typecheck, lint and e2e on Windows and Linux; verify that intentionally invalid structures are rejected during development without adding self-tests to the shipped checker. Preserve the four scenarios when extending game rules.

Any intermittent failure is a defect. Preserve the first failing node log and message-flow record under `.logs/run-<id>/`, identify the public API call, and report Framework defects instead of compensating for them. Cleanup must release only this run's processes and Compose project; failures must remain visible in the exit code. Keep README and comments in English, except for the final Korean execution section in README.

Register the shared Entry and first-login handler on every node that registers the Player Actor factory. Structure validation must require exactly one Entry for each Field and Instance owner and every registered User Spot type. Keep Message Follow and the derived safe-shutdown observation deadline in shared timing constants.
