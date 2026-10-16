# MyGame project instructions

This Kotlin template demonstrates independent MMORPG worlds with channels, typed STREAM clients, HTTP world discovery, and application gold persistence.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| accounts | World list HTTP endpoint in `server/accounts` |
| session | Actor GetOrCreate, binding and relay in `server/session`; add client authentication here |
| channel | Gold rules in `server/channel/src/main/kotlin/MyGame/channel/domain`; persistence port in `application`; channel admission and handlers in `infrastructure/zlink` |
| world-db | Separate Redis for each world in `docker-compose.yml` |
| players | Public connector client and assertions in `e2e` |

## Public API boundaries

Use only public Framework and connector APIs with the default typed JSON serializer. A request handler replies through its supplied Actor's bound Session. Other pushes use player IDs through Actor send, followed by the Spot Actor delivery handler. Spots never retain Actor references. Player owns gold; its relocation adapter captures only gold. Zone identity and membership belong to Framework. World configuration selects one mesh and one Redis store. Do not implement route, lease, reconnect, codec, or handoff policies here. Do not use internal APIs, reflection, raw frames, manual message codecs, sleeps, retries, or increased deadlines to compensate for Framework defects.

## Changes and verification

Run `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` for Docker e2e. Use `--local` or `-Local` for JDK 25 process debugging. Build images with `--build-only` or `-BuildOnly`. Preserve the Docker socket control path and remove only resources owned by the current run.

Keep the ordered worlds, channel, reconnect, and update scenarios and structure checks consistent. Validate real responses and events on Windows and Linux. An intermittent failure is a defect. Preserve the first failure and message-flow output in `.logs/run-<id>/`; investigate API usage before reporting a Framework blocker. Keep build artifacts and license files out of source control.
