# Project instructions

This template implements a two-player arena with an HTTP matchmaking lobby and ZLink STREAM gameplay. Extend game rules in `server/match/domain.hpp` and keep transport wiring separate.

## Node responsibilities

| Node | Responsibility and extension point |
| --- | --- |
| lobby | HTTP pairing and Room preparation: `server/lobby/http.hpp` |
| session | Player GetOrCreate, binding, SessionInfo push and relay: `server/session/session.hpp` |
| matchA, matchB | Entry admission, Room callbacks and state relocation: `server/match/` |
| location | Redis authority and relocation persistence configured in shared hosting |
| players | Public connector and HTTP e2e client in `e2e/` |

## Public API boundaries

Use only public Framework and connector APIs and the default typed JSON serializer. Session binding owns Actor identity; do not put it in game state. Request handlers send responses to the Actor's bound session. Other pushes use player IDs and the public Actor send API, then a Room Actor handler forwards to the bound session. Never store Actor references in Room state. Relocation adapters serialize only application state. Room owns positions, HP, sequence and winner; Actor has no application state. Do not implement routing, retries, leases, lifecycle or codec workarounds. Do not use raw frames, internal APIs, reflection, sleeps, larger deadlines, or weaker assertions to conceal failures.

## Changes and verification

Use `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1` for the default Docker build and e2e. Build-only uses `--build-only` or `-BuildOnly`. For local process debugging, use `--local` or `-Local`; add `--no-build` or `-NoBuild` to reuse a local build. Preserve stdin/stdout lifecycle control in both modes. Keep Docker build and cleanup logs in `.logs/docker-<project>/` and node logs in `.logs/run-<id>/`. Cleanup must remove only this run's containers, network and image tags, and must fail the run on errors.

Keep ordered scenarios `lobby`, `hit`, `reconnect`, `update` consistent with the structure file and README commands. Validate structure before services start and test malformed input separately during development. Run build and e2e on Windows and Linux. A single intermittent failure is a defect. Preserve the first failure and Normal message-flow logs under `.logs/run-<id>/`. Cleanup only the runner's own child processes and uniquely named Compose project; cleanup failures must fail the run. Keep package versions centralized in `vcpkg.json`. Keep README and these instructions in English, with a short Korean section only at the end of README; code comments use English.
