# 08. Observability diagnostics

[Reference index](README.en.md)

The diagnostics option values, host status fields, and the `ZLinkFrameworkErrorKind`
correspondence table are exactly the same as
[Java reference 08. Observability diagnostics](../../java/reference/08-observability-diagnostics.ko.md)
(Korean-only) — Kotlin uses Java's `ZLinkFrameworkErrorKind`/`ZLinkFrameworkException` directly
and adds no separate enum or exception hierarchy. The only thing Kotlin adds is a single
extension function wrapping message-flow observer registration in a receiver lambda. The exact
signature is owned by the
[Kotlin monitoring exact interface](../../common/spec/server/languages/kotlin/interfaces/monitoring.en.md)
(Korean-only).

---

## `onMessageFlow { }` (configuration time, DSL)

An extension function that wraps `ZLinkDispatchOptions.setMessageFlowObserver(...)` in a lambda.

```kotlin
options.configureDispatch {
    onMessageFlow { event: ZLinkMessageFlowEvent ->
        logger.info("flow: ${event.outcome()} ${event.packetName()}")
    }
}
```

**Options.** Takes `observer: (ZLinkMessageFlowEvent) -> Unit`. Returns `ZLinkDispatchOptions`,
so it can be chained with other modifiers inside the `configureDispatch { }` DSL
(topology-discovery category).

**Completion result.** Has exactly the same effect as registering a Java
`ZLinkMessageFlowObserver` — registers synchronously with no return value.

**When to use.** Use this extension function in the same situation as the
`configureDispatch().diagnostics()` entry in the Java reference's document 08, when you want to
use a lambda directly instead of making the observer a separate class.

---

## Host status/error kind

Host status (`ZLinkFrameworkRuntimeStatus`, `ZLinkInboundDispatchStatus`) and the
`ZLinkFrameworkErrorKind` correspondence table use the Java types directly — no Kotlin-only data
class or enum is added. See
[the Java reference's document 08](../../java/reference/08-observability-diagnostics.ko.md)
(Korean-only) for the field meaning and correspondence table of both entries.

---

See the
[Kotlin monitoring exact interface](../../common/spec/server/languages/kotlin/interfaces/monitoring.en.md)
and
[Java reference 08. Observability diagnostics](../../java/reference/08-observability-diagnostics.ko.md)
(Korean-only) for the full rationale.
