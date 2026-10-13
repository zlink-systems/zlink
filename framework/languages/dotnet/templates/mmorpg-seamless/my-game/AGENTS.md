# MyGame development guidelines

This project is a runnable server starting point for an MMORPG seamless world. Game engine projects are outside its scope. `zlink.structure.json` describes the initial composition; Redis and Framework own the current runtime locations.

## Node responsibilities

| Project | Responsibility | Game logic extension point |
| --- | --- | --- |
| `MyGame.Session` | Terminate STREAM connections, locate the player's Actor, bind the Session, and relay typed messages. | Authentication and account validation before Actor binding. |
| `MyGame.Zone` | Host geographical Zone User Spots and player Actors, validate movement, exchange border presence, and request Dungeon creation. | Combat, NPCs, interest management, and authoritative simulation in Spot and Actor handlers. |
| `MyGame.Instance` | Host dynamically created Dungeon User Spots. | Party admission, encounters, rewards, and Dungeon lifecycle. |
| `MyGame.Shared` | Own shared message contracts, configuration, and common process hosting. | Add typed application messages here when multiple processes need the same contract. |
| `MyGame.E2E` | Start isolated infrastructure and nodes, exercise public client APIs, and verify observable results. | Add client scenarios for new gameplay behavior. |

The player Actor owns its coordinate and movement sequence. A Zone's presence records are projections used for display and border updates. Keep game decisions in the owner that has the authoritative state. Framework owns relocation, membership, route convergence, execution gates, leases, and message serialization.

## Changes and verification

Read `README.md` before changing node registration or deployment. Update `zlink.structure.json` and node configurations together when the initial composition changes. Run the structure consistency check and e2e after changes. Keep the four scenario assertions: cross, border, reconnect, and update. A single intermittent failure is a defect; repeated success does not cancel the failure.

Run `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1`. Logs are retained under `.logs/`; inspect node and flow logs from the first failing run. The runner owns its process IDs and Compose project and removes them on completion. Do not stop unrelated processes or containers.

## Public API boundaries

- Use published NuGet packages at the centrally pinned versions. Project references may point only to this generated project's own projects.
- Use typed messages and the default Framework JSON serializer. Relocation adapters serialize only application state; they must not serialize Framework route, lease, membership, or queue state.
- Use Actor `JoinSpot(...).Defer()` for zone transitions. Report completion through the public completion callback. Use public host relocation and runtime observations for drain.
- Reconnect by finding the same Actor ID and binding the new Session. Do not create an application route cache or reset the player's authoritative state.
- Keep the Session relay path visible. Do not implement game movement in the Session node.
- Do not use internal APIs, reflection, raw transport frames, private codecs, extra pollers, or application retries to compensate for Framework behavior.
- Do not add fixed sleeps or enlarge deadlines to make tests pass. Wait for actual replies, events, or lifecycle observations.
- If the required behavior is unavailable through public APIs, report the failing scenario and API boundary instead of introducing a workaround.

Code comments must use English. Do not commit `.logs/`, `bin/`, or `obj/`.
