[한국어](./framework-cpp-0.26.0.ko.md) | [English](./framework-cpp-0.26.0.md)

# ZLink C++ Framework 0.26.0 Release Notes

Framework 0.26.0 uses C++ binding 1.10.0 and Core 1.10.0.

## Changes

- The Stream Connector closes a connection with reason `ProtocolError` after frame decode failure or an inbound payload limit violation. Under §6.3, it validates every option before connecting: out-of-range values return `ValidationFailed`, and incompatible options return `ConfigurationError`. Closing discards unwritten frames and fails pending requests.
- Spot context `Close` returns a result. Completion is reported after authority is released. This preserves the result-returning contract established on main after 0.25.0.
- Relocation `Restore` is decided only by the source. The wire schema no longer includes `remainingDeadlineMs`; compatibility with pre-1.0 wire formats is not provided. Store retention is rounded up when converted to milliseconds.
- Listener status returns the endpoint confirmed by bind. Placement counts use MeshNode activation records, and Session `actor_slot` is resolved at the start of the turn.
- In C++, set each mesh's placement weight with `mesh(name).placement_weight()`. `IsAvailable` reflects whether the Store blocks new work.

## Install

Choose a platform archive from the [`framework-cpp/v0.26.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.26.0), or download it with `bootstrap.cmake`. The server framework requires C++ binding 1.10.0 and Core 1.10.0. A client using only the Stream Connector does not require Core or the binding.

The release tag is [`framework-cpp/v0.26.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.26.0).
