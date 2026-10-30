[English](./framework-java-0.31.0.md) | [한국어](./framework-java-0.31.0.ko.md)

# ZLink Java·Kotlin Framework 0.31.0 Release Notes

Framework 0.31.0 uses Java·Kotlin binding 1.21.0 and Core 1.21.0.

## Behavior Changes

- Message-flow records are now written through the SLF4J provider. Applications that configure only JUL handlers must add an SLF4J-to-JUL binding (for example `org.slf4j:slf4j-jdk14`) to keep receiving message-flow records. When no provider is present, no records are built. (#2758, #3246)

## Bug Fixes

- Disconnect cleanup proceeds while a Session handler is waiting. (#3187)
- Explicit Close access and Join lifecycle owner handling are retained; Spot Close uses the Close operation deadline, and admission is scoped to the Spot incarnation. (#2672, #2758, #2849, #3130, #3178)
- Actor request, Join, and teardown handling preserves cancellation, admission, completion, and failure results. (#2287, #2426, #2507, #2530, #2738, #2758, #2790, #2830, #2878, #2815, #2978, #3071, #3132)
- Node and Channel handlers start in FIFO order without waiting for the preceding handler to complete. (#2758, #2859)
- Instance activation deadline expiration is preserved as `DEADLINE_EXCEEDED`. (#2242, #2471, #2879)
- Instance Spot send/request uses the resolved Ready route. (#1631, #2666)
- Duplicate connections to the same endpoint are avoided, and the WebSocket connector bypasses the OS proxy. (#1710, #2534, #2648, #2797)
- Startup cancellation and owner lease handling, Host shutdown, and Spot closing lifecycle handling are corrected. (#2233, #2286, #2289, #2470)
- Continued ordering during User Spot retirement is preserved, and relocation/Join completion follows its lifecycle owner. (#2207, #2208, #2278, #2526, #2627, #2870)
- Unclassified failures before submission are reported as `InternalFailure` with their cause. (#2360, #2380)
- Java binding registration and Session owner identity handling are corrected. (#2140, #2172, #2236)
- Actor typed replies use a single-part STREAM frame and the specified Error payload. (#2267, #2507)
- A single admission owner now decides remote requests that arrive before the connection is ready. Requests on a ready connection are submitted directly without a wait subscription. (#1710, #3257)
- `GetOrCreate` now checks the existing authority before new-creation capacity. Requests for an existing User Spot no longer fail when creation capacity is exhausted. (#2799, #3242)

## Performance Improvements

- Intermediate allocations and copies are reduced in send, request, receive, dispatch, and envelope paths. (#1640, #1642, #1648, #1673, #2126, #2159, #2165, #2168, #2182, #2185, #2216, #2252, #2254, #2272, #2281, #2321, #2352, #2354, #2608, #2770)
- Synchronous waits in receive/submit and repeated receive-pump posts are removed; a deadline-driven worker and per-rotation permit reservation are used. (#1640, #1642, #1648, #2126, #2165, #2182, #2185)

## Diagnostics and Metrics

- Runtime metric observation points and the labels/Spot kinds for `zlink.spot.count` are aligned. (#2149, #2189, #2190, #2263, #2886)
- Channel receive/handler tracing is improved, missing Actor targets are diagnosed as stale, and telemetry provider delivery is isolated. (#2412, #2799, #3150, #3181)
- Flow is omitted from Errors diagnostic records. (#3168)

## Sample Updates

- The Bingo sample gives late observers the current room state and committed reward, and reads the diagnostics level from configuration. (#2208, #2224, #2427)

## Known Limitations

- Moving private member access in test code to public paths has progressed through its ninth pass for Node, Java, and .NET. The remaining scope continues in the next release. This does not affect product behavior. (#2309)
- On Windows, when the mailbox signaler falls back from AF_UNIX datagram to stream, creating and deleting a temporary path for each socket costs about 2.5 ms. The fix ships in the next Core release. (#3053)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.31.0")
    implementation("systems.zlink:zlink-http-client:0.31.0")
}
```

The release tag is [`framework-java/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.31.0).
