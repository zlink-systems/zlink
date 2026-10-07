[English](./framework-cpp-0.29.0.md) | [한국어](./framework-cpp-0.29.0.ko.md)

# ZLink C++ Framework 0.29.0 Release Notes

Framework 0.29.0 uses C++ binding 1.17.0 and Core 1.17.0. (#1554)

## Contract Changes

- A cold activation for a Missing Instance Spot is submitted to the target through Core request/reply. The target owns reservation and activation. A request that loses the Reserve race ends with one `Unavailable` result; it is not forwarded to another target and does not wait for the winner. If no serving node can provide the requested type, the result is `NotFound`; if an omitted type matches multiple candidates, it is `InvalidOperation`; insufficient capacity returns `Unavailable`. (#1467)
- Constructor dependencies declared by Spot, Actor, and handler registrations are validated from metadata at host startup using optional, collection, and name/qualifier selection rules. Startup fails when a required dependency is missing or ambiguous. Explicit factories are not invoked. (#1549)
- On target restart, recovery releases Creating reservations from the previous lifecycle and returns their pending capacity. (#1532)

## Changes

- Stream nodes now expose `set_heartbeat` and `set_idle_timeout` to configure the heartbeat interval and timeout, and the application idle timeout. The heartbeat defaults are 1 second and 5 seconds. The idle timeout now defaults to 0, which disables idle closure. Heartbeat control frames do not count as application activity. (#1538, #1541)

## Defect Fixes

- A Store provider failure now returns the public `Unavailable` error from Spot Send and Request operations. The provider cause is preserved. (#1466)
- Core 1.17.0 sends each STREAM message over WS and WSS as one WebSocket message. TCP batching is unchanged. (#1545)
- ClientServer worker shutdown and transport poller waiting now share one owner, preventing a worker from using a closed connection during shutdown. The extra monitor poller on each socket is removed; the shared poller delivers new input. (#1423)
- An Actor Join into a Zone under maintenance now ends as a normal rejection and preserves source membership instead of returning `ProtocolError`. (#1490)
- SpotWide Actor queues are consumed through the shared Spot execution gate. A C++ diagnostics file logger failure no longer aborts the process. (#1496, #1466)
- The ZoneWorld sample chooses scenarios from the actual owner placement and processes restart notifications in arrival order. (#1490)

## Install

Select a platform Framework archive from the [`framework-cpp/v0.29.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.29.0), or download it with `bootstrap.cmake`. The server Framework requires C++ binding 1.17.0 and Core 1.17.0. Clients using only the Stream Connector do not require Core or binding.

The release tag is [`framework-cpp/v0.29.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.29.0).
[English](./framework-cpp-0.29.0.md) | [한국어](./framework-cpp-0.29.0.ko.md)
