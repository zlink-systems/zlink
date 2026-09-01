---
title: "16. Options — Setting List And Defaults · Java"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.en.md) | [Previous: E2E Testing](15-e2e-testing.en.md) | [Next: Where To Use ZLink](17-alternative.en.md)
<!-- framework-adapter-nav:end -->

# 16. Options — Setting List And Defaults

> **The document that owns this chapter's contract** —
> covered by the [Java configuration and host public contract](../../../common/spec/server/languages/java/interfaces/configuration-host.en.md).
> This chapter organizes that surface as a list, showing what you can set and what happens
> when you don't.

This chapter gathers **what you can set and what happens if you don't.** What each option
changes is explained by that feature's own chapter — here we look at where it lives and its
default.

## 1. Where Settings Apply

| Location | Scope | When it can change |
| --- | --- | --- |
| `options` inside `ZLinkFrameworkConfigurer` | Process-wide defaults | Only before context startup |
| A builder | That single node / channel / STREAM node | Only before context startup |
| A runtime option bean | Part of an already-running value | While running (§7) |

```java
@Bean
ZLinkFrameworkConfigurer zlink(PlaySettings settings) {
    return options -> {
        // (1) Root -- applies to every payload in this process.
        options.codecs().use(ZLinkProtobufCodec.getDefault());
        options.setDefaultRequestTimeout(Duration.ofSeconds(30));

        // (2) Builder -- applies only to this single node.
        ZLinkMeshNodeBuilder mesh = options.addRouteMesh("play");
        mesh.listen(settings.meshEndpoint())
            .setRoutingIdPrefix("play")
            .setSpotCapacity(2_000);
        mesh.channelName("room").server();
    };
}
```

No surface calls a builder again after the Spring context starts. An invalid combination
isn't deferred until the first call — it's **blocked by an exception at context startup.**

## 2. Root Options

| Option | What it sets | Default |
| --- | --- | --- |
| `codecs()` | Payload serialization format | Built-in JSON |
| `setDefaultRequestTimeout(Duration)` | The cap on waiting for a request reply | 30 seconds |
| `addHandlersFromPackageOf(Class)` | Handler-discovery starting point | Doesn't scan |
| `configureMetadata()` | Metadata propagation policy | — |
| `configureDispatch()` | Diagnostics level/message flow (§4), Core HWM, and the application job queue (§3.3) | `ERRORS`; both profiles use `BALANCED` |
| `configureLocations()` | Location store behavior (§5) | The §5 table |
| `configureNetwork()` | Listener bind/advertise host defaults | bind `0.0.0.0` |
| `configureWorkers()` | The CPU worker pool (§3.2) | The §3.2 table |
| `configureStreamCompression()` | STREAM compression | No compression |
| `useFilter(Class)` | Register a handler filter. Call order is execution order | None |
| `addLocationStore` · `addRelocationStore` | The location-resolution and relocation stores | Single-node configuration if omitted |
| `setApplicationVersion(long)` | The rolling-update version | Unset |
| `setMaintenanceWave(String)` | Marks the same maintenance batch | Unset |
| `useVirtualThreadHandlers()` | Run handlers on a virtual thread | Platform thread |
| `useHandlerExecutor(Executor)` | Specify the handler executor directly | The framework default |

`setDefaultRequestTimeout` **rejects anything at or below 0.**

**Don't use `useVirtualThreadHandlers()` and `useHandlerExecutor(...)` together.** Both set
the handler executor, so whichever is called later overrides the earlier one.

## 3. MeshNode Options

Specified on the builder that `addRouteMesh(name)` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `listen(endpoint)` · `listen(port)` · `listen()` | The address other nodes connect to | Must be specified |
| `setBindHost` · `setAdvertiseHost` | Splitting the bind address from the advertised address | The `configureNetwork()` value |
| `setRoutingId(...)` · `setRoutingIdPrefix(String)` | This node's identifier | Auto-generated |
| `objects()` | Object role — Spot/Actor placement | Doesn't place |
| `channelName(name)` | Register a channel role | — |
| `setPlacementWeight(int)` | Selection weight for new object placement | 100 |
| `setActorCapacity` · `setSpotCapacity` | This node's capacity cap | Unlimited |
| `setActivationConcurrency(int)` | How many cold activations run concurrently | Runtime default |
| `setDefaultRequestTimeout(Duration)` | This node's call reply cap | The root value |
| `peerConnections()` | Manual peer connections | Location-store auto-discovery |
| `configureRouterSocket()` | See §3.1 below | The table below |
| `configureSpotPublisher()` | The Logical Multicast publish socket | Runtime default |

### 3.1 Socket Caps

Values of the `ZLinkMeshNodeSocketConfig` that `configureRouterSocket()` returns.

| Method | What it sets |
| --- | --- |
| `setMaxMessageSize(long)` | Max size of a single accepted message |
| `setSendHighWaterMark(...)` | Bytes kept queued per peer to send |
| `setReceiveHighWaterMark(...)` | Bytes kept queued per peer after receipt |
| `setReceiveTimeout` · `setSendTimeout` | If set, the cap on waiting in that direction |
| `setMailboxMessageBudget(long)` | Message count this node's service mailbox holds |
| `setMailboxByteBudget(long)` | Bytes this node's service mailbox holds |

How the two high-water marks work and how to pick their values is covered by
[4. Backpressure](04-backpressure.en.md).
`0` is not the default — it means **unlimited.**

**The four HWM values are `long`.** They're in bytes, so `int` can't reach past 2 GiB.

### 3.2 CPU Worker Pool

Values of the `ZLinkWorkerOptions` that `configureWorkers()` returns. Sets the single
elastic pool that `context.runCpuWorker(...)` uses.

| Option | Default |
| --- | --- |
| `minThreads(int)` | 0 |
| `maxThreads(int)` | `max(2, CPU count × 2)` |
| `idleTimeout(Duration)` | 30 seconds |
| `maxQueueLength(int)` | 1024 |

**When the queue fills, submit fails immediately.** There's no policy that waits or runs on
the caller's thread. Before raising the queue length, first look at how long the work you're
handing to the worker actually runs.

### 3.3 Core HWM And The Application Job Queue

These are values on `ZLinkDispatchOptions`, returned by `configureDispatch()`. Core HWM
limits accounted bytes in ordinary queues; the
application job queue limits jobs waiting for handler start across the host instance.

| Method | What it sets | Default |
| --- | --- | --- |
| `setCoreHwmMemoryLimitBytes(long)` | Memory-limit hint forwarded for Core budget calculation | Unset |
| `setCoreHwmBudgetBytes(long)` | Manual Core budget that takes precedence over the profile | Unset (Auto) |
| `setCoreHwmProfile(ZLinkCoreHwmProfile)` | Core Auto-budget profile | `BALANCED` |
| `setApplicationJobQueueProfile(ZLinkApplicationJobQueueProfile)` | Queued-job Auto profile | `BALANCED` |
| `setMaxQueuedApplicationJobs(long)` | Exact manual queued-job limit | Unset (Auto) |

The memory limit and Core budget must be positive. The manual queued-job limit is
`1..2,147,483,647`; `0` is a startup configuration error, not unlimited. The two profiles
use the same labels but are independent enums and calculations. See
[4. Backpressure](04-backpressure.en.md) and [Common Perf §23](../../../common/perf/README.en.md#23-measuring-production-values-for-core-hwm-and-the-application-job-queue)
for saturation behavior and production measurement.

## 4. Diagnostics

The surface `configureDispatch()` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `messageFlow(ZLinkMessageFlowLogMode)` | Recording level | `ERRORS` |
| `traceSampleRate(double)` | Sampling ratio | 1.0 |
| `includeMessageSizes(boolean)` | Whether to include payload byte size | Not recorded |
| `unhandled()` | The behavior for a dispatch with no handler | Below |

`unhandled()` sets the behavior per branch with `setRequest` / `setSend` / `setPublish`, and
the recording level with `setSendLogLevel` / `setPublishLogLevel`.

What's recorded per level is covered by the `11. Monitoring` chapter.

## 5. Location Options

Values of the `ZLinkLocationOptions` that `configureLocations()` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `setOwnerLeaseRenewInterval(Duration)` | Owner lease renewal interval | 5 seconds |
| `setOwnerLeaseTtl(Duration)` | Lease validity period | 30 seconds |
| `setOwnerLeaseRenewTimeout(Duration)` | The cap on a renewal call | 3 seconds |
| `setOwnerLeaseFencingMargin(Duration)` | Margin that excludes the previous owner | 5 seconds |
| `setPollingInterval(Duration)` | Store query interval | 1 second |
| `setStoreFailureGrace(Duration)` | How long a store outage is tolerated | 30 seconds |
| `setRouteCacheMaxAge(Duration)` | Route cache validity period | 15 seconds |
| `setMessageFollowDuration(Duration)` | How long a message keeps following a target that's relocating | 30 seconds |

> **The owner lease default differs across all three languages.** The TTL-to-renewal-
> interval ratio is 6× for Java (5s : 30s), 3× for C++ (5s : 15s), and 1.5× for Node
> (10s : 15s). If you mix nodes from different languages in the same mesh, specify the
> values explicitly to match. The same gap ledger's G6 entry covers this discrepancy.

## 6. STREAM Options

Specified on the builder that `addStreamNode(name)` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `bind(endpoint)` · `bind(port)` · `bind()` | The address clients connect to | Must be specified |
| `setBindHost` · `setAdvertiseHost` | The bind address and advertised address | The `configureNetwork()` value |
| `enableActorDispatch()` | Lets a session relay to an Actor | Not enabled |
| `registerSession(Class)` | The session type created per connection | Must be specified |
| `setTlsServer(cert, key[, requireClientCert])` | TLS configuration | Plaintext |

**`enableActorDispatch` also takes a mesh name.** It's the argument that specifies which
mesh to find the Actor in — a slot the other languages don't have.

## 7. What You Can Change While Running

Only **two weights** can change after startup. Inject the `ZLinkRouteMeshRuntimeOptions`
bean to use them.

| Value | Surface | What it's for |
| --- | --- | --- |
| Placement weight | `mesh(name).setPlacementWeight(int)` | Remove or restore this node as a new-object placement target |
| Channel weight | `channel(name).weight(int)` | Remove or restore this node as a new select-one target |

Setting either to `0` **only stops new assignments.** Existing objects and connections stay
alive as-is.

## 8. What You Must Set

| Value | Where |
| --- | --- |
| A MeshNode's `listen` address | `addRouteMesh(...).listen(...)` |
| A STREAM node's `bind` address and session type | `addStreamNode(...)` |
| A fanout publisher's endpoint | `addFanoutChannel(...).enablePublisher(...)` |
| The Object role of a node that places Spots/Actors | `objects().server()` |
| The location store, when using multiple nodes | `addLocationStore(...)` |
| Handler-discovery starting point | `addHandlersFromPackageOf(...)` |

## 9. Common Problems

- **Handlers aren't registering** → you didn't call `addHandlersFromPackageOf(...)`, or the
  discovery starting point doesn't cover the handler package. Putting `@Component` on a
  handler doesn't register it either.
- **I left it at `0` and memory keeps growing** → `0` on a high-water mark isn't the
  default — it means unlimited.
- **I set timeout to 0 and startup fails** → that's expected.
  `setDefaultRequestTimeout` rejects anything at or below 0.
- **CPU worker submit fails immediately** → the queue is full. Before raising
  `maxQueueLength`, look at how long the worker's task actually runs. There's no waiting
  policy.
- **The virtual-thread setting doesn't take effect** → `useHandlerExecutor(...)` may have
  been called afterward and overridden it. Use only one of the two.
- **I set weight to 0 and thought it dropped existing connections** → weight blocks **only
  new assignments.**
- **Owner determination differs after mixing nodes from two languages** → the
  `ownerLeaseTtl` default differs per language (§5). Specify the values explicitly to match.

## 10. Related Documents

- The formal contract: [Java configuration and host public contract](../../../common/spec/server/languages/java/interfaces/configuration-host.en.md)
- What each cap changes: [4. Backpressure](04-backpressure.en.md)
- The procedure for draining traffic with weights: [12. Operations](12-operations.en.md)
