<!-- SPDX-License-Identifier: LicenseRef-Zlink -->
# Network sync server plugin

`Zlink.NetworkSync.Server` is the server plugin that runs one match in one ZLink Framework User Spot. `Simulation/` is the match tick and input buffer core, which does not reference the Framework. `Server/` connects that core to the public Framework API (Spot timer, SpotWide execution, typed Actor packet handlers, `BoundSession.Send`). The wire format is owned by [`../protocol/`](../protocol/README.md).

## Usage

- A match Spot derives from `NetworkSyncSpot<TSpot, TActor, TInput, TAction, TState>` and implements `AdmitPlayerAsync`, `GetMissingInput`, `Step` and `CaptureSnapshot`. Register it with SpotWide execution and relocation disabled.
- **Session dispatch must go through `NetworkSyncSession.TryRelayAsync`.** The input byte limit is checked only at the Session ingress against the original payload length, because the typed Spot handler does not receive the original bytes. Input forwarded to the Actor without this call skips the size check.
- Each recipient holds at most one send admission in progress and the newest full snapshot not yet sent. Ticks never wait for an admission, so a slow client does not block the Spot.
- If the simulation (`Step` or `CaptureSnapshot`) throws, the timer stops and the Spot closes.

## Verification

`dotnet test Simulation.Tests` and `dotnet test Server.Tests` (Server.Tests needs Redis at `ZLINK_REDIS_TEST_ENDPOINT`, default `127.0.0.1:6379`). CI is `.github/workflows/network-sync-server.yml`.
