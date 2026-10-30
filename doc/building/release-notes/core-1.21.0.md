[English](./core-1.21.0.md) | [한국어](./core-1.21.0.ko.md)

# libzlink 1.21.0 release notes

## Changes

- Added per-connect connection-attempt snapshots and `POLLCONNECT`. Snapshots expose connection state, typed failure causes and stages, and opaque identifiers. Recovery preserves the last failure (#1710, #2692).
- Fixed premature request termination when a reply was already queued before pair termination or handover (#2981, #3021).
- Removed unnecessary network-interface lookup for numeric IP binds and peer termination-ACK waiting during inproc unbind (#3035, #3064, #3072).
- Drain outstanding accept completions during Asio listener shutdown and use the logical-ready count as the monitor READY source (#2658, #2967, #2396, #2876).
- Corrected pending inproc-connect completion and scheduled-reconnect control-runtime lifetime during context termination (#2899, #2901).
- Removed the unsupported Windows AF_UNIX datagram signaler attempt. The existing stream pair and conditional TCP fallback remain (#3053, #3158).

## Compatibility

Existing public C ABI symbols and linker SOVERSION 0 are retained while adding the new API. Use 1.21-series bindings with Core 1.21 as required by the [version policy](../versioning.md). Framework versions and their exact binding pins remain independently owned.
