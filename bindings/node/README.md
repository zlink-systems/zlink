# Node Bindings

Aligned Node bindings for `libzlink`.

## Canonical Raw API

- `new PairSocket(ctx)`, `new DealerSocket(ctx)`, `new RouterSocket(ctx)`,
  `new StreamSocket(ctx)`
- `new PubSocket(ctx)`, `new XPubSocket(ctx)`
- `new SubSocket(ctx)`, `new XSubSocket(ctx)`
- `Context.options: ContextOptions`, `Context.shutdown()`, `Context.close()`
- connection lifecycle: `bind(endpoint)`, `unbind(endpoint)`
- connectable sockets: `connect(endpoint)`, `disconnect(endpoint)`
- discovery attachment: `attachDiscovery(discovery)` on `DealerSocket`,
  `RouterSocket`, `PubSocket`, `SubSocket`
- publisher sockets: `publish(topic).message(...).submit()` (synchronous
  void-or-throw)
- message sockets: async `send().message(...).submit()` (Core-completion Promise)
  and sync `submit_sync(SendFlags)`,
  `recv(flags?)`
- routed sockets: async `send(routingId).message(...).submit()`
  (Core-completion Promise) and sync `submit_sync(SendFlags)`, `recv(flags?)`
- requests: async `request(...).message(...).submit()` returns
  `Promise<Message[]>`; `submit_sync(flags)` returns `Message[]`, while
  `submit_sync(flags, callback)` returns after admission and delivers the reply
  to `(error, reply)`. `submit_sync(SendFlags.None)` can block the Node event
  loop, so use it only when another execution context can make reply progress.
- subscriber sockets: `setSubscription(topicOrPattern)`,
  `unsetSubscription(topicOrPattern)`, `subscribe(topicMessage, flags?)`
- `XPubSocket`: `receiveSubscriptionEvent(subscriptionEvent, flags?)`
- `StreamSocket`: `setRoutingId()`, `getRoutingId()`, managed Core-completion
  `send(routingId)`, immediate `trySend(routingId)`, `recv(flags?)`,
  `onPacket(handler)`
- TLS helpers: `setTlsServer(cert, key, requireClient?)`,
  `setTlsClient(ca, host, trust?)` on sockets, `Registry`, and `SpotNode`
- canonical option facades:
  - `CommonSocketOptions`
  - `DealerSocketOptions`
  - `RouterSocketOptions`
  - `StreamSocketOptions`
  - `PubSocketOptions`
  - `SubSocketOptions`
- context option facade: `ContextOptions`
- socket option access: `socket.options.*`
- monitors: `monitorOpen(events?, monitorHwmBytes?)` with default `ALL` events
  and the Core-default queue HWM, then `recv(flags?)`,
  `onEvent()`

`Context.options` should be configured immediately after constructing the
context and before creating sockets.

`Message.from(...)` is the canonical from-source factory. `Message.data` and
`Message.size` expose the canonical payload view. `Message.close()`,
`Symbol.dispose`, and `Symbol.asyncDispose` are the cleanup hooks.
`Message.getProperty(name)` and `Message.refCount()` expose diagnostic
metadata on canonical receive results.
Canonical raw sockets intentionally hide opposite-direction methods, so
`PubSocket` does not expose `send()` or `recv()`, `SubSocket` does not expose
`send()`, and `StreamSocket` does not expose `connect()` or active stream
helpers on the canonical path.

Canonical receive results are domain objects:

- `Received`: `{ routingId: Buffer | null, parts: Message[] }`, with
  `send(...)` for sending a normal routed message back over the original
  receive context and `reply(...)` for request-reply messages
- `Subscribed`: `{ routingId: Buffer | null, topic: string, parts: Message[] }`
- `SubscriptionEvent`:
  `{ routingId: Buffer | null, topic: string, subscribed: boolean }`
- `SendResult`: `Sent`, `Backpressured`, `NotReady`

- generic `Socket` / `BaseSocket` are not exported from the aligned public API
- legacy flags-based send/recv, raw stream attach/detach helpers, and raw
  socket option bags are not part of the public package surface

Not part of the canonical stream API contract:
length-prefixed stream framing such as `len32be` is only a sample helper and
is not exposed as a public `StreamSocket` method.

## Callback Delivery

Send completion and request reply callbacks are installed once per socket and
use N-API thread-safe functions only to deliver completion data into JavaScript.
The callback does not submit, wait, retry, or own a binding queue. Stream packet,
socket monitor, and timer callbacks retain their existing native callback
delivery limits; there is no send-ready or publisher-admission callback surface.

## Service Surface

- `new Discovery(ctx, serviceType, serviceName)`
- `new Registry(ctx)` + `registry.bind(pubEndpoint, routerEndpoint)`
- `new RegistryQueryClient(ctx)`
- `new SpotNode(ctx)`
- `new Spot(node)`

`SpotNode` exposes `createRouteBridge()` for caller-owned channel sockets and
`createPublisher()` for publishing into the local SPOT topic plane.

`Spot` is service-aware and uses explicit service names on the data plane:
`publish(serviceName, topic, ...)`, `sendChannel(channelName, ...)`,
`requestChannel(channelName, ...)`, `setSubscription()` /
`unsetSubscription()`, `subscribe(topicMessage, flags?)`,
  `receiveSubscriptionEvent(subscriptionEvent, flags?)`, `recvRouted(received, flags?)`,
  `recvActorLifecycle(flags?)`, and `onDispatchEvent()`.

`Discovery` uses `connectRegistry()`, `setValue()` / `getValue()`,
`setMetadata()` / `getMetadata()`, `memberPeers()`,
`memberPeerMetadata()`, `monitorOpen(events?, monitorHwmBytes?)`, `setTlsClient()`.

`SpotNode` uses `setPubBind()`, `setRouterBind()`,
`connectPeer()` / `disconnectPeer()`,
`attachDiscovery()`, `status()`, `peers()`,
`peers(filter)()`, `subjects(filter?)`, `setTlsServer()`,
`setTlsClient()`.

`Registry` uses `bind()`, `setId()`, `addPeer()`, `setHeartbeat()`,
`setBroadcastInterval()`, `setTlsServer()`, `setTlsClient()`,
`status()`,
  `serviceSummary(filter?)`, `memberPeers()`,
  `memberPeerMetadata()`, `topology()`, `topology(filter)(filter?)`.

`RegistryQueryClient` uses `connect()` and `snapshot(filter?)`.

`SocketMonitor` uses `recv(flags?)`, `onEvent()`, `snapshot()`,
  `close()`.

`*_READY_CHANGED` monitor events are readiness edge/state notifications.
Node bindings must not interpret `event.value` as an aggregate ready count, and
`snapshot()` must not be used as a ready-count gate.

`Receiver` is removed from the aligned public API.
`Discovery` requires a non-empty `serviceName`.
`Registry.bind()` maps directly to the native bind lifecycle and replaces legacy
`setEndpoints()` / `start()`.
`Registry.setSockOpt()` and `SpotNode.setDiscovery()` are not part of the
aligned canonical surface. `Spot` also does not expose raw `setSockOpt()`;
raw socket options are exposed through typed `socket.options` facades instead
of raw option bags or per-socket setter aliases. `Spot` keeps service-level
typed setters such as `setLinger()` and `setNoDrop()`.
After `attachDiscovery()`, manual socket/node connect-disconnect entry points
are blocked by the native lifecycle contract.

## Verification

```bash
cd bindings/node && npm run build
cd bindings/node && npm run rebuild-native
cd bindings/node && npm test
cd bindings/node && npm run samples
cd bindings/node && npm run perf:single -- --recv callback --pattern PAIR --warmup 0.2 --duration 0.5
cd bindings/node && npm run perf:multi -- --recv recv --pattern STREAM --warmup 0.2 --duration 0.5
```

## Perf Status

- single perf is implemented for `PAIR`, `PUBSUB`, `DEALER_DEALER`,
  `DEALER_ROUTER`, `ROUTER_ROUTER`, `SPOT`
- single perf supports `--recv callback` only
- multi perf is implemented for `MULTI_DEALER_DEALER`, `MULTI_PUBSUB`,
  `STREAM`
- multi perf supports:
  - `MULTI_DEALER_DEALER`: `--recv recv`
  - `MULTI_PUBSUB`: `--recv recv`
  - `STREAM`: `--recv recv|callback`
- perf structure and review criteria are defined by
  [`bindings/README.md`](/home/hep7/project/kairos/zlink/doc/spec/bindings/README.md)
  and the shared policy docs under
  [`doc/perf/`](/home/hep7/project/kairos/zlink/doc/perf)
- readiness gates in binding perf must use low-cost event counting, not
  aggregate ready counts from monitor payloads or snapshots
- raw sockets: `CONNECTION_READY` event counting
- SPOT: explicit benchmark barrier protocol; no separate service-event gate
