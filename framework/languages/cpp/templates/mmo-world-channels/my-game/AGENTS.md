# MyGame project guidelines

This template implements independent MMORPG worlds with channel relocation using public C++ ZLink Framework APIs. Keep both world meshes and player data isolated.

## Node responsibilities

| Node | Responsibility and extension location |
| --- | --- |
| accounts | Publish worlds and Session endpoints over HTTP; extend `server/accounts/main.cpp`. |
| sessionA, sessionB | Accept STREAM clients, bind Actors, and relay typed packets; extend `server/session/session.hpp`. |
| channelA1, channelA2, channelB | Host Entry and Zone Spots and Player Actors; extend `server/channel/zone.hpp` and `player.hpp`. |
| dbA, dbB | Store world-specific player gold and independent Framework stores; extend application storage in `server/channel/data.hpp`. |

## Public API boundaries

Player owns gold. Zone identity and membership belong to Framework. Never keep Actor references in Spot state. Request handlers send to the supplied Actor's bound Session; other pushes use Actor send followed by the Zone's delivery handler. Relocation adapters capture application state only.

Use Framework's default typed JSON serializer. Do not introduce internal APIs, raw Framework frames, per-message codecs, manual packet encoding, reflection, retry loops, sleeps, deadline increases, or weakened assertions. `SessionInfoRequest` must use request/reply; report a missing public typed reply API rather than replacing it with a push. Application Redis protocol code does not implement Framework transport or state.

## Changes and verification

Keep `worlds`, `channel`, `reconnect`, and `update` in canonical order. Maintain both runners, the structure declaration, and README commands together. Run builds and e2e on Windows and Linux/WSL. A single intermittent failure is a defect. Check actual messages and lifecycle observations; do not infer successful relocation or safe shutdown from command acceptance.

Keep configurations and logs in `.logs/run-<id>/`. Preserve the first error and message-flow diagnostics. Stop only processes and Compose projects owned by the current run. Cleanup failures must fail the run. Keep source comments in English.
