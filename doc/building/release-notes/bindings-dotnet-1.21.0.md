[English](./bindings-dotnet-1.21.0.md) | [한국어](./bindings-dotnet-1.21.0.ko.md)

# ZLink .NET binding 1.21.0 release notes

Uses Core 1.21.0.

## Changes

- Added connection-attempt snapshots and PollConnect, and reject loading when the required Core export is missing (#1710).

## Core 1.21.0 included

- Added per-connect connection-attempt snapshots and POLLCONNECT readiness. Snapshots provide opaque tokens and typed failure causes, retaining the last failure after recovery (#1710, #2692).
- Consume replies queued before pair termination, including handover, before terminating outstanding requests (#2981, #3021).
- Resolve numeric bind addresses without a network-interface lookup (#3035, #3064).
- Release inproc endpoints without waiting for a peer termination ACK (#3035, #3072).
- Finish outstanding Asio accept completions before listener shutdown (#2658, #2967).
- Report monitor READY from the logical-ready count (#2396, #2876).
- Complete pending inproc connects during context termination and use the active control runtime for scheduled reconnect work (#2899, #2901).
- Remove the unsupported Windows AF_UNIX datagram signaler attempt while retaining the stream pair and conditional TCP fallback (#3053, #3158).
- Join private inproc reconnect work before socket close, so `zlink_close` no longer fails with `EBUSY` when no user call is running on the socket (#3197, #3199).
