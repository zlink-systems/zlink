[English](./framework-dotnet-0.30.2.md) | [한국어](./framework-dotnet-0.30.2.ko.md)

# ZLink .NET Framework 0.30.2 Release Notes

Framework 0.30.2 uses .NET binding 1.20.0 and Core 1.20.0.

## Changes

- Request admission is distinct from reply completion, so callers can observe acceptance. Deferred Join deadlines use the registration clock, and callback suspension after cancellation is observed. (#2069, #2070, #2270, #2253)
- Relocation, Session, and Join paths use the source request owner and Session owner identity consistently. Location runtime metrics now follow Store records and polling points. (#2150, #2151, #2280, #2285, #1933, #1992, #1995)
- Reduced per-message Store reads and completion work. Reusing ingress envelope headers lowers processing cost for small messages. (#1817, #1992, #2093, #2178)
- Aligned Actor population and runtime metric observation with object and lifecycle lanes, and count liveness probes before submission. (#2261, #2269, #2381)

## Defect Fixes

- Fixed deadlocks and ordering errors when Join completion and Actor handoff waited on the same turn. Restored frames are now admitted to the Actor FIFO before execution, and shutdown reclaims retired Actors. (#2117, #2118, #2261, #2291)
- Fixed failure classification and binding cleanup for ClientServer, Spot, and Session paths. Cleanup of an already-absent physical RID now completes, and requests without a valid source descriptor end with the appropriate error. (#2114, #2132, #2137, #2147, #2391)
- Fixed stalls or failures involving Join deadlines, route replacement during relocation, and callback cancellation during liveness management. (#1937, #1954, #2270, #2253)
- Reduced unnecessary idle worker and timer waits and CPU use, and corrected RouteMesh selection and head-fence evaluation. (#2279, #1817, #1992)
- When target Join lifecycle work fails, the original exception is reported through the existing error reporter. (#2459)
- Relocation rollback now waits only until restored frames are admitted to the FIFO. Typed transport results from a replaced connection determine replay. (#2462)
- Updated the Bingo sample so an observer looks up the room's current state and confirmed rewards after joining. (#2427)
- STREAM connections closed by server drain now report the spec close reason `server_shutdown`. (#2189)
- Core 1.20.0 no longer stalls the temporary transport owner's executor when a completion poller is registered during the handshake. (#2504)
- Actor replies are sent as a single STREAM frame, and error payloads use the spec's `{code, message}` form. All four languages verify against the same shared vectors. (#2507)
- Service wire commands 21, 22, 24, and 25 are read and written with the schema-generated codec and use the same layout as the other languages. (#2507)
- The Session bind fence is built only from the row that describes the submitted ActorRef, which fixes bind rejections caused by values from another row after relocation. (#2507)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.30.2" />
<PackageReference Include="Zlink.HttpClient" Version="0.30.2" />
```

The release tag is [`framework-dotnet/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.30.2).
[한국어](./framework-dotnet-0.30.2.ko.md) | [English](./framework-dotnet-0.30.2.md)
