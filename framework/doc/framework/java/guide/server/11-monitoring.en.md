---
title: "11. Monitoring — Status Observation And Diagnostics · Java"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Location](10-location.en.md) | [Next: Operations — metrics · drain · readiness](12-operations.en.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — Status Observation And Diagnostics

> **The document that owns this chapter's contract** — covered by the
> [Java monitoring public interface](../../../common/spec/server/languages/java/interfaces/monitoring.en.md).
> This chapter explains the four observation surfaces that contract exposes, focused on
> usage.

Handler calls alone can't show you all of operations. Whether a connection is ready, which
peer dropped, where a message failed — you also have to read this from the framework
surface. The Java framework exposes this through **three channels** — a status snapshot and
`Flow.Publisher`, message flow records, and Micrometer instruments.

There's no surface that receives a runtime event as a bean handler. Observation always goes
through one of the three channels below.

## 1. Observation Surfaces

| What you're watching | Surface | Where it's covered |
|---|---|---|
| Host lifecycle (relocate / drain / readiness) | `ZLinkFrameworkRuntime.status()` · `observe()` | [12. Operations](12-operations.en.md) §6.1 |
| A MeshNode's node / peer / channel readiness | `ZLinkRouteMeshRuntime.snapshot(...)` · `observe(...)` | [12. Operations](12-operations.en.md) §5 |
| A ClientServer channel's target status | `ZLinkClientServerRuntime` | This chapter §2 |
| A pub/sub channel's publisher status | `ZLinkFanoutRuntime` | This chapter §2 |
| Location store status and topology | `ZLinkLocationRuntimeQuery` | [10. Location](10-location.en.md) §4 |
| Message receive / dispatch / failure and flow | The message flow in `configureDispatch()` | This chapter §3 |
| Numbers like CCU and queue depth | The Micrometer `MeterRegistry` | This chapter §4 |

All four are **injected as beans.** They're registered in the Spring container, so just
declare them as constructor arguments.

## 2. Status Snapshot And Status Stream

Every status surface has the same shape — `snapshot(...)` gives one immutable record, and
`observe(...)` gives every change after that as a `Flow.Publisher`.

```java
@Service
public class MeshWatcher {
    private final ZLinkRouteMeshRuntime meshRuntime;

    public MeshWatcher(ZLinkRouteMeshRuntime meshRuntime) {
        this.meshRuntime = meshRuntime;
    }

    public boolean ready() {
        // One snapshot of the current value.
        ZLinkMeshNodeSnapshot snapshot = meshRuntime.snapshot("game.room");
        return meshRuntime.isReady("game.room");
    }

    public void watch(Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> subscriber) {
        // A slow subscriber that exceeds capacity skips intermediate values.
        // The skipped count arrives in each item's loss().
        meshRuntime.observe("game.room", 64).subscribe(subscriber);
    }
}
```

**`observe(...)` gives you a `ZLinkObservedStatus<T>`.** `status()` is the complete
snapshot after the change — a full record every time, not an event with only the changed
fields — and `loss()` is how many this subscription has missed so far. If you need to
compare against the previous value, the subscribing side has to keep it.

`loss()` splits into two. `coalescedCount()` is the number of intermediate states skipped by
coalescing, and `discardedTerminalCount()` is the number of terminal states that exceeded
the retention cap and are gone for good. The first still means you got the latest value; the
second doesn't.

| | `snapshot(...)` | `observe(...)` |
| --- | --- | --- |
| What it gives | One record, at call time | The complete record on every change |
| When to use it | An operations endpoint response, a one-off check | Recording or reacting to state transitions |
| Can it miss anything | N/A | Skips intermediate values past capacity |

`Flow.Publisher` is the JDK's standard reactive stream. If you use Reactor, wrap it with
`JdkFlowAdapter.flowPublisherToFlux(...)`; for Kotlin, `asFlow()`.

Peer status carries only the Node RID, its current state, and why it's unavailable.
Connection intent, discovery source, and lifecycle generation are framework-internal state
and aren't exposed.

## 3. Message Flow Tracing

Where and how an individual message ended is seen through message flow. `configureDispatch()`
sets the level.

```java
@Bean
ZLinkFrameworkConfigurer zlink(PlaySettings settings) {
    return options -> {
        options.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.ERRORS_ONLY) // Default -- failures and backpressure only.
            .traceLogFile(settings.logDir() + "/flow.jsonl")  // Written separately from app logs.
            .traceLabel(settings.instanceName());             // Marks which instance the record is from.
    };
}
```

| Level | Recording scope |
| --- | --- |
| `OFF` | Records nothing |
| `ERRORS_ONLY` (default) | Dispatch failures and backpressure |
| `KEY_TRANSITIONS` | The above + major transitions like receive/dispatch/complete |
| `VERBOSE` | The above + a record for every individual message |
| `DIAGNOSTIC` | The above + diagnostic detail |

**Keep operations at `ERRORS_ONLY` and raise it only when needed.** `VERBOSE` and above
record something for every message, so on high-throughput paths it becomes a load in itself.

To receive records in your program, register an observer.

```java
options.configureDispatch().setMessageFlowObserver(error -> {
    // Runs on the runtime thread -- don't block or call back into a framework surface here.
    auditSink.append(error);
    return CompletableFuture.completedFuture(null);
});
```

The behavior for a dispatch with no handler is set by `unhandled()` — per branch with
`setRequest` / `setSend` / `setPublish`, and the recording level with `setSendLogLevel` /
`setPublishLogLevel`.

## 4. Metrics

Framework instruments go out through Micrometer. If you use Spring Boot Actuator, the
registry is already in the container, so **no extra wiring is needed.** To rename instruments
or attach common tags, add a `ZLinkMetricsCustomizer` bean.

```java
@Bean
ZLinkMetricsCustomizer zlinkMetrics(PlaySettings settings) {
    return registry -> registry.config()
        .commonTags("node", settings.instanceName());
}
```

Instrument names start with `zlink.`. The exact names, kinds, units, and labels are owned
by [Runtime Metrics And Aggregation Rules](../../../common/spec/25-runtime-metrics.ko.md).

> **The Java runtime currently emits only part of the contract.** Of the 47 the contract
> defines, only 14 are emitted, and the three request-related ones come out as
> `zlink.channel.request.*` rather than the contract name (`zlink.mesh_node.request.*`).
> Check the actual emitted names before building a dashboard.

## 5. Readiness And Liveness

Java has no separate health-check surface. **You judge it from runtime status.**

```java
@Component
public class ZLinkReadinessIndicator implements HealthIndicator {
    private final ZLinkFrameworkRuntime runtime;

    @Override
    public Health health() {
        return runtime.status().isReady()
            ? Health.up().build()
            : Health.outOfService().build();
    }
}
```

**Reflect a dependency that can drop out briefly, like the store connection, only in
readiness.** Put it in liveness and the orchestrator kills the process the moment the store
blips.

## 6. Common Problems

- **I subscribed to `observe(...)` but no value arrives** → a `Flow.Publisher` only flows
  once subscribed. Check that you called `subscribe(...)` and signaled demand with
  `Subscription.request(n)`.
- **Some state transitions are missing** → `observe(...)`'s capacity was exceeded and they
  got skipped. Raise the capacity and make the subscriber consume faster.
- **There's a deadlock inside the observer** → it runs on the runtime thread. Don't
  block-wait or call back into a framework surface inside it.
- **The flow record is empty** → the default level is `ERRORS_ONLY`, so normal flow isn't
  recorded. Raise it to `KEY_TRANSITIONS` or above.
- **Metrics aren't showing up** → check that Actuator and the registry are in the context.
  The framework only publishes instruments to the registry — it doesn't create the registry
  itself.
- **The store blipped briefly but the process restarted** → the store status is in liveness.
  Move it to readiness.

## 7. Related Documents

- The formal contract: [Java monitoring public interface](../../../common/spec/server/languages/java/interfaces/monitoring.en.md)
- Metrics and drain/readiness operations: [12. Operations](12-operations.en.md)
- Diagnostics option list: [16. Options](16-options.en.md) §4
- Instrument naming convention: [Runtime Metrics And Aggregation Rules](../../../common/spec/25-runtime-metrics.ko.md)
