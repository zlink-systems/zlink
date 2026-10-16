# Project guidelines

This template implements an action/FPS entry lobby with persistent matchmaking and authoritative rooms using the public ZLink .NET packages.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| Session | STREAM connection, Actor GetOrCreate and bind, SessionInfo and relay in PlayerSession |
| Match | Stateless Entry forwarding, matchmaking in Domain/MatchmakingState, combat rules in Domain/RoomState; public Framework adapters in Infrastructure/ZLink |
| Location | Redis location and relocation storage; no application decisions |
| Players | Public connector client and scenario runner in e2e/MyGame.E2E |

## Public API boundaries

Use typed packets and the default JSON serializer. Handler responses go directly to the received Actor's bound session. Other pushes use stored player IDs and the public Actor send API; concrete Spot delivery handlers forward to bound sessions. Never store Actor references in Spot state. Relocation adapters serialize only application state. Actor identity and binding remain Framework-owned.

Do not use internal APIs, reflection, raw frames, manual codecs, transport retry logic, fixed sleeps, larger deadlines or weaker assertions to compensate for Framework failures. Record reproducible failures with node logs and message-flow records. Keep package versions in Directory.Packages.props.

## Changes and verification

Run `./run-e2e.sh --build-only` (PowerShell: `pwsh -NoProfile -File ./run-e2e.ps1 -BuildOnly`) to build Docker images. The default `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` runs nodes and the e2e runner in Docker. Use `--local` or `-Local` for host process debugging with the .NET 8 SDK; only that mode accepts `--no-build` or `-NoBuild`. Pass `--check-structure` to check without starting nodes. Preserve stdin lifecycle control and inspect `.logs/docker-<project>/` and `.logs/run-<id>/`.

Preserve the hit, reconnect and update scenarios and their order. Validate the structure before starting infrastructure. Run builds and e2e on Windows and Linux. A single intermittent failure is a defect. Logs are in `.logs/run-<id>/`; clean only processes and Compose projects owned by that run. Domain state is serialized by the Framework SpotWide execution gate; do not add parallel locks or queues for it.
