---
title: "16. Options — Setting List And Defaults · Node/TypeScript"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: E2E Testing](15-e2e-testing.ko.md) | [Next: Where To Use ZLink](17-alternative.en.md)
<!-- framework-adapter-nav:end -->

# 16. Options — Setting List And Defaults

> **The document that owns this chapter's contract** —
> covered by the [Node.js foundation and configuration public contract](../../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md).
> This chapter organizes that surface as a list, showing what you can set and what happens
> when you don't.

This chapter gathers **what you can set and what happens if you don't.** What each option
changes is explained by that feature's own chapter — here we look at where it lives and its
default.

**Every time value is a number in milliseconds.** A name ending in `...Ms` is the marker.

## 1. Where Settings Apply

| Location | Scope | When it can change |
| --- | --- | --- |
| The `zlinkFramework()` builder | Process-wide defaults | Only before module initialization |
| A nested builder | That single node / channel / STREAM node | Only before module initialization |
| A runtime option token | Part of an already-running value | While running (§7) |

```typescript
ZLinkModule.forRootFactory({
  useFactory: () => {
    const builder = zlinkFramework();

    // (1) Root -- applies to this whole process.
    builder.codecs().use(ZLinkProtobufCodec.default);
    builder.setMessageFollowDuration(30_000);

    // (2) Builder -- applies only to this single node.
    const mesh = builder.addRouteMesh('play')
      .listen(config.meshEndpoint)
      .setRoutingIdPrefix('play')
      .setSpotLimit(2_000);
    mesh.channel('room').server();

    return builder;   // Nothing turns on if you don't return it.
  }
});
```

No surface calls a builder again after module initialization. An invalid combination isn't
deferred until the first call — it's **blocked by an exception at initialization.**

## 2. Root Options

| Option | What it sets | Default |
| --- | --- | --- |
| `codecs()` | Payload serialization format | Built-in JSON |
| `configureNetwork()` | Listener bind/advertise host defaults | bind `0.0.0.0` |
| `configureWorker(options)` | The CPU worker pool (§3.2) | The §3.2 table |
| `configureInboundDispatch()` | Host-wide receive cap (§3.3) | Auto-calculated |
| `configureDispatch()` | Diagnostics level and message flow (§4) | `ErrorsOnly` |
| `configureLocations()` | Location store behavior (§5) | The §5 table |
| `configureStreamCompression()` | STREAM compression | No compression |
| `addLocationStore` · `addRelocationStore` | The location-resolution and relocation stores | Single-node configuration if omitted |
| `setApplicationVersion(bigint)` | The rolling-update version | Unset |
| `setMaintenanceWave(string)` | Marks the same maintenance batch | Unset |
| `setActorTransferTimeout(ms)` | The cap on Actor relocation | Runtime default |
| `setMessageFollowDuration(ms)` | How long a message keeps following a target that's relocating | 30,000 |
| `addRouteMesh` · `addClientServerChannel` · `addFanoutChannel` · `addStreamNode` | Topology registration | — |

**`setApplicationVersion` takes a `bigint`.** Suffix the numeric literal with `n` — `12n`.

## 3. MeshNode Options

Specified on the builder that `addRouteMesh(name)` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `listen(endpoint)` · `listen(port?)` | The address other nodes connect to | Must be specified |
| `setBindHost` · `setAdvertiseHost` | Splitting the bind address from the advertised address | The `configureNetwork()` value |
| `routingId(...)` · `setRoutingIdPrefix(string)` | This node's identifier | Auto-generated |
| `objects()` | Object role — Spot/Actor placement | Doesn't place |
| `channel(name)` | Register a channel role | — |
| `setPlacementWeight(number)` | Selection weight for new object placement | 100 |
| `setActorLimit` · `setSpotLimit` | This node's capacity cap | Unlimited |
| `setActivationConcurrency(number)` | How many cold activations run concurrently | Runtime default |
| `setDefaultRequestTimeout(ms)` | This node's call reply cap | 30,000 |
| `peerConnections()` | Manual peer connections | Location-store auto-discovery |
| `configureRouterSocket()` | See §3.1 below | The table below |
| `configureSpotPublisher()` | The Logical Multicast publish socket | Runtime default |

### 3.1 Socket Caps

Fields of the `ZLinkMeshNodeSocketConfig` that `configureRouterSocket()` returns.

| Field | What it sets |
| --- | --- |
| `maxMessageSize` | Max size of a single accepted message |
| `sendHighWaterMark` · `receiveHighWaterMark` | Bytes kept queued per peer |
| `receiveTimeoutMs` · `sendTimeoutMs` | If set, the cap on waiting in that direction |

> **Two values exist in the spec but not in the implementation.** The public contract
> places `mailboxMessageBudget` and `mailboxByteBudget` on this config, but they aren't on
> the current builder surface.

How it works and how to pick values is covered by
[4. Backpressure](04-backpressure.en.md).
`0` is not the default — it means **unlimited.**

### 3.2 CPU Worker Pool

Passed all at once via `configureWorker({...})` — not a chain of method calls like the
other languages.

| Field | What it sets |
| --- | --- |
| `minThreads` · `maxThreads` | Pool size |
| `idleTimeoutMs` | How long an idle thread is kept before folding |
| `maxQueueLength` | Wait-queue length |

**I/O workers don't use this pool.** `runIoWorker(...)` runs on the event loop.

### 3.3 Host-Wide Receive Cap

The surface `configureInboundDispatch()` returns. It differs in nature from the per-
connection cap — it applies to the **total payload** of messages that haven't yet started
handler execution.

> This unit and cap are confirmed by contract but **the runtime doesn't use them yet.**
> See [4. Backpressure §6](04-backpressure.en.md#6-framework-runtime-coverage).

## 4. Diagnostics

The surface `configureDispatch()` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `messageFlow(ZLinkMessageFlowLogMode)` | Recording level | `ErrorsOnly` |
| `traceLogFile(path)` | A file written separately from app logs | Not separated |
| `traceLabel(name)` | The instance name attached to records | None |
| `setMessageFlowObserver(...)` | Receiving records in your program | None |

There are four levels: `Off`, `ErrorsOnly`, `KeyTransitions`, `Verbose`. **They're
PascalCase values**, unlike the SCREAMING_SNAKE of the other languages.

## 5. Location Options

Values of the `ZLinkLocationOptions` that `configureLocations()` returns. All of these are
method chains.

| Option | Default |
| --- | --- |
| `ownerLeaseRenewIntervalMs(...)` | 10,000 |
| `ownerLeaseTtlMs(...)` | 15,000 |
| `ownerLeaseRenewTimeoutMs(...)` | 3,000 |
| `ownerLeaseFencingMarginMs(...)` | 5,000 |
| `pollingIntervalMs(...)` | 1,000 |
| `storeFailureGraceMs(...)` | 30,000 |
| `routeCacheMaxAgeMs(...)` | 15,000 |
| `messageFollowDurationMs(...)` | 30,000 |
| `maxActiveOutboundRelocations(...)` · `maxActiveInboundRelocations(...)` | 64 |
| `maxConcurrentRelocationCaptures(...)` · `maxConcurrentRelocationRestores(...)` | 8 |
| `maxRelocationPayloadInFlightBytes(...)` | 268,435,456 (256 MiB) |

> **The TTL-to-renewal-interval ratio is 3×** (5 seconds : 15 seconds). That means the
> lease survives up to two missed renewals. All four languages share this value, so mixing
> nodes from different languages in one mesh doesn't shift when owner determination
> happens. If you change the value, **change the ratio along with it.**

## 6. STREAM Options

Specified on the builder that `addStreamNode(name)` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `bind(endpoint)` · `bind(port?)` | The address clients connect to | Must be specified |
| `setBindHost` · `setAdvertiseHost` | The bind address and advertised address | The `configureNetwork()` value |
| `enableActorDispatch()` | Lets a session relay to an Actor | Not enabled |
| `registerSession(factory)` | The session factory used per connection | Must be specified |
| `setTlsServer(certPath, keyPath, requireClientCert?)` | TLS configuration | Plaintext |

**`registerSession` takes a factory, not a class.** This is a spot where the other
languages pass a type.

## 7. What You Can Change While Running

Only **two weights** can change after startup. Inject them via the
`ZLINK_ROUTE_MESH_RUNTIME_OPTIONS` token.

| Value | Surface | What it's for |
| --- | --- | --- |
| Placement weight | `mesh(name).placementWeight = 0` | Remove or restore this node as a new-object placement target |
| Channel weight | `channel(name).weight = 0` | Remove or restore this node as a new select-one target |

**Node uses property assignment.** This differs in shape from a setter method in the other
languages.

Setting either to `0` **only stops new assignments.** Existing objects and connections stay
alive as-is.

## 8. What You Must Set

| Value | Where |
| --- | --- |
| `useFactory` returning the builder | `ZLinkModule.forRootFactory({ useFactory })` |
| A MeshNode's `listen` address | `addRouteMesh(...).listen(...)` |
| A STREAM node's `bind` address and session factory | `addStreamNode(...)` |
| A fanout publisher's endpoint | `addFanoutChannel(...).enablePublisher(...)` |
| The Object role of a node that places Spots/Actors | `objects().server()` |
| The location store, when using multiple nodes | `addLocationStore(...)` |

## 9. Common Problems

- **Nothing turns on** → `useFactory` didn't return the builder. It has to end with
  `return builder`.
- **The timeout is oddly short or long** → the argument is a **number in milliseconds**.
  If you mistake it for seconds and pass `3`, that's 3 milliseconds.
- **`setApplicationVersion` throws a type error** → it takes a `bigint`. Write it as `12n`.
- **I left it at `0` and memory keeps growing** → `0` on a high-water mark means unlimited.
- **The enum value doesn't match** → Node uses PascalCase (`ErrorsOnly`). Don't copy the
  SCREAMING_SNAKE straight over from another language's docs.
- **I passed a class to `registerSession` and it doesn't work** → it takes a factory.
- **I set weight to 0 and thought it dropped existing connections** → weight blocks **only
  new assignments.**
- **Owner determination differs after mixing nodes from different languages** → the lease
  default differs per language (§5).

## 10. Related Documents

- The formal contract: [Node.js foundation and configuration public contract](../../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md)
- What each cap changes: [4. Backpressure](04-backpressure.en.md)
- The procedure for draining traffic with weights: [12. Operations](12-operations.en.md)
- Injection token list: [13. Key Interface Usage Index](13-interface-catalog.en.md) §1
