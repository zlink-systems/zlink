# MyGame instructions

This is a standalone C++ seamless-world template using public ZLink Framework packages. Keep changes within this project and preserve the four e2e scenarios.

## Node responsibilities

| Node | Responsibility | Extension point |
| --- | --- | --- |
| Session | Accept stream clients, create or locate Actors, bind them, and relay typed packets. | `server/session/session.hpp` |
| Zone A / Zone B | Host WorldEntry, Player Actors and Zone User Spots; relocate players between Zones and publish border visibility. | Movement rules in `server/zone/domain.hpp`; Framework callbacks in `server/zone/player.hpp` and `server/zone/zone.hpp`. |
| Instance | Create Dungeon User Spots on demand. | `server/instance/dungeon.hpp` |
| Redis | Store Framework Location and relocation authority. | `server/shared/hosting.hpp` configures public Redis providers. |

## Public API boundaries

Use typed messages and the Framework's default JSON serializer. Do not register message-specific codecs or manually encode game packets. `SessionInfo` reads identity from the actual Session binding. Game state messages omit Session and Actor identity.

Request handlers send responses directly through the Actor's bound Session. Other pushes route by player ID through the public Actor client and are forwarded by the receiving Spot's Actor handler. Spots retain player IDs and application projections, not Actor references. Player movement has one authoritative owner. Relocation adapters serialize application state only; Framework owns routes, leases, membership, queues, timers, and handover.

Use public host relocation and runtime observation for `drain`; require `SafeToShutdown` before shutdown. Report a missing or failing Framework public capability. Do not introduce internal APIs, reflection, raw frames, manual codecs, sleeps, retries, expanded deadlines, or weakened assertions. Do not replace the `border` channel with MeshName to compensate for a Framework subscription defect.

## Changes and verification

Keep `zlink.structure.json`, server configuration, contracts, and scenario order consistent. Run the smallest relevant build or check first, then Windows and Linux e2e. An isolated intermittent failure is a defect; a successful rerun does not cancel it. Preserve the first failing message-flow log and report any Framework blocker separately from application errors.

Run formatters and installation commands only from this project directory, with explicit file paths that exclude `.logs`, `.validation`, and build outputs. Node logging writes one Normal message-flow stream to stderr. The runner retains logs under `.logs/run-<id>/`, uses unique Redis prefixes and Compose project names, and cleans up only processes and containers it created. Cleanup failures must return exit code 1.

README and instructions are English. Code comments are English. Keep the short Korean execution guidance at the end of README.
