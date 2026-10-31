[English](./framework-node-0.31.0.md) | [한국어](./framework-node-0.31.0.ko.md)

# ZLink Node.js Framework 0.31.0 Release Notes

Framework 0.31.0 uses Node.js binding 1.21.0 and Core 1.21.0.

## Changes

- Restore the received message class using the handler's static `messageType`. (#1731, #2715)
- Send Actor typed messages and replies according to the wire contract, and validate responses against the request packet's reply contract. (#2267, #2507)
- Align the labels and values for `zlink.spot.count` and improve capacity and runtime metrics. (#2149, #2189, #2199)
- Report Instance activation timeouts as `DeadlineExceeded` and record one-way creation failures in flow diagnostics. (#2242)
- Report target Join lifecycle failures and apply the relocation outcome contract. (#2278, #2426)

## Fixes

- Make Join and Close requests for a Spot that is closing follow the terminal result of the operation accepted first. (#2672, #2675)
- Fix an AutoConnectLoop deadlock caused by waiting for another state lane's completion. (#2673)
- Close admission first when liveness expires, and treat cleanup of an already absent physical RID as complete. (#2369, #2717, #2734)
- Complete rejected admission requests with domain replies. (#2625)
- Preserve `Continued` callback ordering while retiring a User Spot. (#2627)
- Handle Actor ingress acceptance, Follow, and stale checks in one path, and classify late membership generation conflicts. (#2799, #2738)
- Perform the currentness check and frame submission for a bound Session push in the same registry lane turn. (#2757)
- Use the Close operation's deadline for an explicit Spot close. (#2849)
- Start Node and Channel handlers in FIFO order without waiting for the preceding handler to finish. (#2758)
- Confirm RouteMesh peer readiness only after receiving the peer's ADMIT and send the first HELLO immediately. (#2790)
- Fix duplicate connects to the same endpoint, bind registration, raw host access after a failed close, Actor cleanup during Session awaits, and callback ordering during User Spot retirement. (#1710, #2140, #2627, #2797, #3177, #3191)
- Correct the Flow inclusion condition in Errors diagnostic records and heartbeat shutdown handling. (#2243, #3170)
- Reject an enum ordinal used as a schema name. (#1713)
- A single admission owner now decides remote requests that arrive before the connection is ready. Requests on a ready connection are submitted directly without a wait subscription. (#1710, #3258)
- `GetOrCreate` now checks the existing authority before new-creation capacity. Requests for an existing User Spot no longer fail when creation capacity is exhausted. (#2799, #3242)

## Performance improvements

- Consolidate completion handling and admission callbacks in request, send, and mesh dispatch paths; reduce unnecessary Promise boundaries, array iterators, and intermediate copies. (#1640, #1675, #2232, #2237, #2351, #2603)
- Reuse the receive buffer during request backpressure and reserve permits once per ClientServer receive rotation. (#1648, #2349)
- Remove temporary arrays and iterators when creating service reply multipart messages. (#3193, #3194)

## Known Limitations

- Moving private member access in test code to public paths has progressed through its ninth pass for Node, Java, and .NET. The remaining scope continues in the next release. This does not affect product behavior. (#2309)
- On Windows, when the mailbox signaler falls back from AF_UNIX datagram to stream, creating and deleting a temporary path for each socket costs about 2.5 ms. The fix ships in the next Core release. (#3053)

## Installation

```bash
npm install @zlink-systems/framework@0.31.0 @zlink-systems/http-client@0.31.0
```

Release tag: [`framework-node/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.31.0).
