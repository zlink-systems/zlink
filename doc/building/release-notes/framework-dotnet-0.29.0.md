[English](./framework-dotnet-0.29.0.md) | [한국어](./framework-dotnet-0.29.0.ko.md)

# ZLink .NET Framework 0.29.0 Release Notes

Framework 0.29.0 uses .NET binding 1.17.0 and Core 1.17.0. (#1554)

## Contract Changes

- A cold activation for a Missing Instance Spot is submitted to the target through Core request/reply. The target owns reservation and activation. A request that loses the Reserve race ends with one `Unavailable` result; it is not forwarded to another target and does not wait for the winner. If no serving node can provide the requested type, the result is `NotFound`; if an omitted type matches multiple candidates, it is `InvalidOperation`; insufficient capacity returns `Unavailable`. (#1467)
- Constructor dependencies declared by Spot, Actor, and handler registrations are validated from metadata at host startup using optional, collection, and name/qualifier selection rules. Startup fails when a required dependency is missing or ambiguous. .NET uses keyed services and `IServiceProviderIsService` metadata; explicit factories are not invoked. (#1549)
- On target restart, recovery releases Creating reservations from the previous lifecycle and returns their pending capacity. (#1532)

## Changes

- Stream nodes now expose `SetHeartbeat` and `SetIdleTimeout` to configure the heartbeat interval and timeout, and the application idle timeout. The heartbeat defaults are 1 second and 5 seconds. The idle timeout now defaults to 0, which disables idle closure. Heartbeat control frames do not count as application activity. (#1538, #1541)

## Defect Fixes

- A Store provider failure now returns the public `Unavailable` error from Spot Send and Request operations. The provider cause is preserved. (#1466)
- Core 1.17.0 sends each STREAM message over WS and WSS as one WebSocket message. TCP batching is unchanged. (#1545)
- .NET now classifies a fixed RoutingId conflict as a configuration error and removes a duplicate Store lookup. (#1525)
- When an Actor returns to an Entry Spot, the source leave notification is submitted one-way and the Join coordinator owns processing order. Callback lookup no longer depends on a disposed Actor scope. (#1490)
- An accepted authority terminal during target shutdown now completes its Store write before the record is released. (#1083)
- SpotWide Actor queues are consumed through the shared Spot execution gate. (#1496)
- The ZoneWorld sample chooses scenarios from the actual owner placement and processes restart notifications in arrival order. (#1490)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.29.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.29.0" />
```

The release tag is [`framework-dotnet/v0.29.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.29.0).
[English](./framework-dotnet-0.29.0.md) | [한국어](./framework-dotnet-0.29.0.ko.md)
