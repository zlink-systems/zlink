[English](./framework-dotnet-0.31.0.md) | [한국어](./framework-dotnet-0.31.0.ko.md)

# ZLink .NET Framework 0.31.0 Release Notes

Framework 0.31.0 uses .NET binding 1.21.0 and Core 1.21.0.

## Defect Fixes

- Binding cleanup now proceeds after a STREAM disconnect even while a Session handler is waiting. (#3185, #3189)
- Payloads received before Join completes are held and replayed in order after relocation. (#2483, #3054)
- Actor Join generation and Store membership failures now follow the activation and Store results. (#2738, #2873, #2903)
- Channel handlers start in FIFO order without waiting for the preceding handler to complete. (#2758, #2858)
- RouteMesh peer readiness is established by receiving ADMIT from the peer. (#2790, #2807)
- Fixed Join and Close results for Spots that are closing, and host draining after a rejected Join. (#2672, #2675, #2682, #2683, #2688)
- The Session bind fence now uses the Store row for the submitted ActorRef, and related service-wire commands use schema-generated codecs. (#2507, #2514, #2517, #2588)
- Actor replies now match the contract for STREAM frame parts and Error payloads; target lifecycle failures after Join commit are reported. (#2513, #2426)
- Fixed liveness probe submission and physical RID cleanup result handling. Unclassified failures before submission are reported as InternalFailure with their original cause. (#2369, #2380, #2381)
- The WebSocket connector connects directly to the configured endpoint without using the OS proxy. (#2534, #2648)
- Fixed relocation rollback admission and transport replay, and the ordering for releasing deferred Join claims. (#2408, #2227, #2261)
- Service commands now use the source lifecycle confirmed at ingress. A control-send failure alone no longer ends route admission. (#2914, #2923, #2784)
- Aligned owner boundaries for bind registration, request admission, actor activation and completion, host shutdown, and relocation outcomes across framework runtimes. (#2140, #2242, #2260, #2267, #2270, #2275, #2276, #2280, #2284, #2285)
- A single admission owner now decides remote requests that arrive before the connection is ready. Requests on a ready connection are submitted directly without a wait subscription. (#1710, #3259)
- `GetOrCreate` now checks the existing authority before new-creation capacity. Requests for an existing User Spot no longer fail when creation capacity is exhausted. (#2799, #3242)

## Performance Improvements

- Reduced node turns, closures, empty Lists, reply header copies, and an unnecessary forwarding await in request-serial and direct-request paths. (#1674, #2241, #2604)
- Reduced header recreation, body copies, success-path string costs, and request cancellation observer allocations in client send/request paths. (#1674, #2363, #2371)
- Envelope decoding no longer copies the entire frame, and header validation uses allocation-free range checks. (#1674, #2255, #2905, #2912)
- Improved receive batching, header reuse, worker pooling, and idle worker stopping. (#1640, #1643, #1648, #2164, #2171, #2178, #2186, #2206, #2273, #2279)
- Logical request deadlines are managed by one owner-specific index instead of per-request timers, while preserving single ownership of request submission and physical completion. (#2758, #2900, #3142)
- Added and aligned runtime metrics and diagnostic instrumentation for Channel receive and handler phases. (#2189, #2263, #2412, #2799, #3171, #3180)
- When the default `AddLogging` registration has no provider, message-flow records are not built. A user-registered `ILoggerFactory` is used as before. `Zlink.Framework.AspNetCore` now references `Microsoft.Extensions.Logging` 8.0.0 directly. (#2758, #3246)

## Known Limitations

- Moving private member access in test code to public paths has progressed through its ninth pass for Node, Java, and .NET. The remaining scope continues in the next release. This does not affect product behavior. (#2309)
- On Windows, when the mailbox signaler falls back from AF_UNIX datagram to stream, creating and deleting a temporary path for each socket costs about 2.5 ms. The fix ships in the next Core release. (#3053)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.31.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.31.0" />
```

The release tag is [`framework-dotnet/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.31.0).
