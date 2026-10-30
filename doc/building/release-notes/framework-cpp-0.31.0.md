[English](./framework-cpp-0.31.0.md) | [한국어](./framework-cpp-0.31.0.ko.md)

# ZLink C++ Framework 0.31.0 Release Notes

Framework 0.31.0 uses C++ binding 1.21.0 and Core 1.21.0. (#3164)

## Changes

- Unified the message type used by STREAM packet and relay APIs as `framework::message_t`. (#2681, #2711)
- The host records all instruments through the single OpenTelemetry `MeterProvider` supplied by the application. (#2874, #2887)
- Unified DTO typed JSON conversion with a common `dto_traits` traversal and added a C++ wire example. (#1713, #1757, #2751, #2951)
- Message-flow and dispatch-error telemetry is delivered on one process delivery worker. Loggers and observers no longer run on the calling thread; when more than 8192 records are pending, the drop count is exported as the `zlink.observability.events.overflow` metric. (#2758, #3241)

## Defect Fixes

- Return the route worker while a handler is pending and retain the invocation with executor credit so Host.stop waits for the handler to physically finish. (#2758, #3201)
- STREAM disconnect cleanup proceeds while a Session handler is waiting. (#3195)
- Spot Close deadlines now remain in effect across Store lookup and remote work. (#3179, #3188)
- Manual dispatch drains through the existing owner mailbox; fixed FIFO and shutdown handling for ClientServer receive, dispatch, and reply. (#2758, #2877, #2885, #3183)
- Route handlers are looked up once, permits are acquired immediately before handler execution, and receive codecs follow the wire content type. (#2758, #2844)
- Corrected Spot shutdown ordering and callback cancellation/drain so Core contexts and callbacks are released during shutdown. (#2758, #2848, #2929, #2950)
- Join failures now report the failure Kind specified by the contract and preserve Store membership conflicts. (#2738, #2913)
- Fixed Join and Close results for Closing Spots and authority lookup for relocated User Spots in `GetOrCreate`. (#2672, #2675, #2607, #2683)
- Channel send retains call arguments across suspension, and Actors on a bound Session use that Session's exact route. (#2507, #2593, #2611)
- Fixed STREAM host binding for `port "*"` and the lifetimes of disconnect and `dispatch_ready` callbacks. (#2295, #2302, #2618, #2619)
- Fixed Actor bind, relocation, deferred Join handling, and delivery of late terminal results. (#1786, #2109, #2119, #2128, #2227, #2261, #2385, #2406, #2507, #2544, #2573)
- Aligned Session and Spot lifecycle, connection ownership, liveness cleanup, request failure classification, and Actor reply wire format with the contracts. (#1710, #2189, #2263, #2360, #2369, #2380, #2513, #2797)
- Corrected Actor terminal drop diagnostics and `zlink.spot.count` labels and values, and consolidated drop metric instrument ownership. (#2149, #2886, #3162, #3166, #3169)
- Work is rejected atomically when worker creation fails; Actor durable admission and supply timing now follow the owner turn and idle queue head. (#3167, #3172, #3174)
- Fixed state lane infrastructure worker sharing, Session control priority, and ClientServer/Fanout FIFO and drain behavior. (#2758, #2791, #2885, #3136)
- A single admission owner now decides remote requests that arrive before the connection is ready. Requests on a ready connection are submitted directly without a wait subscription. (#1710, #3256)
- `GetOrCreate` now checks the existing authority before new-creation capacity. Requests for an existing User Spot no longer fail when creation capacity is exhausted. (#2799, #3242)
- Fixed Host.stop finishing before the handler terminated when continuation registration allocation failed while a handler waited for another task. The registration path no longer allocates (2→0). `task.hpp` changed, so rebuild your application. (#2758, #3260)

## Performance Improvements

- Reduced costs in request transport, local enqueue, send producer and saturation, route submission, and receive paths. (#1672, #2579, #2616)
- Improved state lane completion and idle admission paths. (#1672, #2610)
- Reduced coroutine frame allocations in general claim dispatch. Measured results were 12 calls and 10.8 KB down to 7 calls and 1.2 KB, with a 31% speedup. (#1672, #2418)
- Removed lane closure and forwarding coroutine costs from public node send. (#1672, #2377)
- Write Actor STREAM headers directly into the final message buffer and avoid copying typed envelope headers. (#1672, #2258, #2364)
- Receive roles are handed to idle host workers. (#1672, #1867)

## Known Limitations

- Moving private member access in test code to public paths has progressed through its ninth pass for Node, Java, and .NET. The remaining scope continues in the next release. This does not affect product behavior. (#2309)
- On Windows, when the mailbox signaler falls back from AF_UNIX datagram to stream, creating and deleting a temporary path for each socket costs about 2.5 ms. The fix ships in the next Core release. (#3053)

## Install

Select a platform Framework archive from the [GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.31.0), or download it with `bootstrap.cmake`. A server Framework requires C++ binding 1.21.0 and Core 1.21.0. A client that uses only the Stream Connector does not require Core or the binding.

The release tag is [`framework-cpp/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.31.0).
