---
title: "11. Monitoring — Status Observation And Diagnostics · Kotlin"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Location](10-location.en.md) | [Next: Operations — metrics · drain · readiness](12-operations.en.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — Status Observation And Diagnostics

> **The document that owns this chapter's contract** — the observation surfaces are the
> same as [Java 11. Monitoring](../../../java/guide/server/11-monitoring.en.md). This
> chapter notes only what changes in Kotlin.

Read the Java chapter first. The four status surfaces, message flow levels, Micrometer
integration, and readiness judgment all apply as-is. Kotlin only changes **the shape of
what you receive**, in three places.

## 1. Receiving The Status Stream As A `Flow`

Java gives you a `Flow.Publisher`. The `asFlow()` extension converts it to a coroutine
`Flow`.

```kotlin
@Service
class MeshWatcher(private val meshRuntime: ZLinkRouteMeshRuntime) {

    suspend fun watch() {
        // A slow collector that exceeds capacity skips intermediate values -- same as Java.
        meshRuntime.observe("game.room", 64).asFlow().collect { observed ->
            record(observed.status())
            // observed.loss() is how many this subscription has missed.
        }
    }
}
```

`asFlow()` is provided by `zlink-framework-kotlin`. Cancelling the subscription is cleaned
up together when you stop collecting the `Flow` — you don't handle `Subscription` directly.

Host status works the same way.

```kotlin
runtime.observe().asFlow().collect { observed ->
    val status = observed.status()
    logger.info("host lifecycle: {} {}", status.state(), status.relocationResult())
}
```

## 2. Receiving Paged Queries As A `Flow`

For a paged query like Location topology, an extension function stitches the pages
together.

```kotlin
// Java: repeats listTopology(filter, page) until the cursor is empty.
// Kotlin: topology(...) wraps that loop as a Flow.
query.topology(ZLinkLocationTopologyFilter("play"), pageSize = 100)
    .collect { entry -> render(entry) }
```

`pageSize` defaults to 100. **The page boundary still exists** — `Flow` only hides it; it
doesn't fetch everything at once.

## 3. Registering A Message Flow Observer As A Lambda

Java implements and passes a `ZLinkMessageFlowObserver`. Kotlin gives you a lambda via an
extension function.

```kotlin
options.configureDispatch().onMessageFlow { event ->
    // Runs on the runtime thread -- don't block or call back into a framework surface here.
    auditSink.append(event)
}
```

`onMessageFlow` wraps `setMessageFlowObserver`, so the behavior is the same. **It's not a
suspend lambda** — if you need to await a coroutine inside it, hand off to a separate scope.

There's also an extension that wraps registration in a block.

```kotlin
options.configureDispatch {
    messageFlow(ZLinkMessageFlowLogMode.ERRORS_ONLY)
    traceLabel(settings.instanceName)
}
```

## 4. Common Problems

- **`asFlow()` isn't visible** → check the `zlink-framework-kotlin` dependency and the
  `systems.zlink.framework.kotlin` import.
- **I stopped collecting the `Flow` but the subscription is still around** → the
  subscription ends when the collecting coroutine is cancelled. Check that you didn't just
  stop collecting while keeping the scope alive.
- **I can't call a suspend function inside `onMessageFlow`** → it's a plain lambda. If you
  need a coroutine, put the event on a channel and process it in a separate scope.
- **Other symptoms** → see [Java 11. Monitoring](../../../java/guide/server/11-monitoring.en.md) §6.

## 5. Related Documents

- The full observation surface: [Java 11. Monitoring](../../../java/guide/server/11-monitoring.en.md)
- What the Kotlin layer adds: [1. Overview](01-overview.en.md) §2
- The Kotlin-specific contract: [Kotlin monitoring public contract](../../../common/spec/server/languages/kotlin/interfaces/monitoring.en.md)
- Diagnostics option list: [Java 16. Options](../../../java/guide/server/16-options.en.md) §4
