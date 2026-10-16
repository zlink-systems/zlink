# Project guidelines

This template implements a megaserver world with capacity-limited Region instances, Player Actors, Session relay, and dynamically created Dungeons.

## Node responsibilities

| Role | Responsibility and extension location |
| --- | --- |
| Session | STREAM connection, Actor GetOrCreate and binding, SessionInfo, relay in `server/MyGame.Session` |
| Region | Entry target selection, Region admission, position, fanout and relocation in `server/MyGame.Region` |
| Instance | Dungeon creation and typed queries in `server/MyGame.Instance` |
| Shared | Typed contracts, Player factory, hosting, diagnostics and process control in `server/MyGame.Shared` |
| Location | Redis Location and Relocation stores in `docker-compose.yml` |

## Public API boundaries

Use public Framework and stream connector APIs and the default typed JSON serializer. A request handler replies directly through the received Actor's bound Session. Other pushes use player IDs and Actor send; the current Spot's delivery handler forwards to the bound Session. Never retain Actor references in Region state. Region owns its population and positions, and Entry alone chooses the next instance after Region rejects a join. The Actor forwards the rejection without choosing another target.

Relocation adapters serialize application state only. Framework owns identity, membership, routes, leases, execution gates, queues and transport. Do not add internal APIs, reflection, raw frames, manual codecs, extra transport state, fixed sleeps, retries, larger deadlines, or weaker assertions to compensate for Framework defects.

## Changes and verification

Maintain `region,dungeon,reconnect,update` in that order and keep the README commands identical to `zlink.structure.json`. Run both shell runners in their respective environments. Any intermittent failure is a defect. Preserve `.logs/run-<id>/`, including stderr message-flow records, and report the first failing transition. Test structural validation with valid files and temporary invalid copies outside the deliverable. Never commit logs, package caches, credentials or license files. Code comments are English.
