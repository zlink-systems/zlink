[English](./framework-node-0.32.0.md) | [한국어](./framework-node-0.32.0.ko.md)

# ZLink Node.js Framework 0.32.0 Release Notes

Framework 0.32.0 uses the Node.js binding 1.23.0 and Core 1.23.0. (#3843)

## Behavior Changes

- Unknown control packets and heartbeat payload versions no longer close the connection. (#3773, #3790)
- The Actor owner rejects duplicate or out-of-order relayed session sequences. (#1721, #3763)
- `MessageContext.receivedAt` is the UTC time when Framework accepts a message. DTO properties without defaults are required in framework-json-v1. Duplicate envelope header fields are rejected. (#2029, #3602, #1705, #3592, #3376, #3396)
- Duplicate-bind decisions go through the owner Actor's `onDuplicateBind` callback. (#2205, #3640)
- The public `configureRouterSocket()` HWM setting is passed to the RouteMesh ROUTER socket. (#2567, #3570)

## Defect Fixes

- Late physical results invalidate a route cache entry only when its fence still matches; cache invalidation follows the trigger table in 08-routing §2. Forced-stop Store cleanup uses one deadline and sweeps by authority; cleanup releases reservations when target lifecycles end. (#3571, #3771, #2989, #3409, #2646, #3462, #3583, #3697)
- Relocation terminal results are recorded in message flow, with cutover flow and Bingo join/observer policy preserved. (#2465, #3621)
- Callback failures preserve the original exception. A malformed request with readable correlation receives a terminal protocol error. (#2098, #3429, #2512, #3418)
- Instance-intent requests reject stale routes before admission and follow current authority within the request deadline. Liveness cleanup completes peer cleanup and notification even when disconnect fails. (#3353, #3442, #2370, #3434)
- Auto-connect keeps one lifecycle across stop/start, waits for descriptor publication, and does not reconcile peers when its own descriptor publication fails. (#3569, #3679, #3723, #3730, #3664)
- Auto-connect excludes only descriptors for the same endpoint. Late terminal replies release the Actor slot and use a Session reply after binding completes. (#2155, #3433, #2757, #3419)
- The 0.31.0 `mmo-world-channels` template update no longer loses the first ChannelView while reacquiring a record lease. (#3344, #3363)
- Discovery resumes a pending HELLO after it observes the peer. (#3326, #3370)

## Performance Improvements

- Payload-sized Framework buffers use an internal slab, and the shared receive queue reuses its permit context. (#1675, #3422, #3655)

## Tests and CI

- M6B stale-route contract tests now establish the state their scenarios require. (#3778, #3789)

## Known Limitations

- The Node `mmo-seamless` template border E2E can intermittently time out while waiting for `PlayerAppeared`. (#3680)

## Install

```bash
npm install @zlink-systems/framework@0.32.0 @zlink-systems/http-client@0.32.0
```

The release tag is [`framework-node/v0.32.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.32.0).
