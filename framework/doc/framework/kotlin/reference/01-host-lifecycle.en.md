# 01. Host lifecycle

[Reference index](README.en.md)

Host registration (`@EnableZLinkFramework` + `ZLinkFrameworkConfigurer` bean) and
`ZLinkFrameworkRuntime` (`relocate`/`shutdown`/`status`/`observe`) use the Java types as-is — the
exact signature and options table follow
[Java reference 01. Host lifecycle](../../java/reference/01-host-lifecycle.ko.md) (Korean-only)
directly. The only thing Kotlin adds is a single bridge connecting `CompletionStage<T>` to
coroutines. The exact signature is owned by the
[Kotlin common runtime exact interface](../../common/spec/server/languages/kotlin/interfaces/common-runtime.en.md)
(Korean-only).

---

## `CompletionStage<T>.await()`

Waits, inside a suspend function, on the `CompletionStage<T>` that Java's `relocate(...)`/
`shutdown(...)` returns.

```kotlin
val relocation = frameworkRuntime.relocate(
    ZLinkFrameworkRelocationOptions(
        ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
        12L,
        Duration.ofSeconds(30),
    )
).await()

if (relocation.outcome() == ZLinkFrameworkRelocationOutcome.RELOCATED) {
    frameworkRuntime.shutdown().await()
}
```

**Options.** This function has no modifiers — call the extension function `await()` directly.

**Completion result.** Preserves the Java `CompletionStage`'s success value and failure cause
as-is. Coroutine cancellation only ends the waiting continuation — it does not cancel the shared
host operation (relocate/shutdown) itself once started. The completion kinds and conditions of
`relocate`/`shutdown` are identical to the Java reference.

**When to use.** Always use this extension function to wait on a host lifecycle operation from
Kotlin coroutine code. There is no separate dedicated suspend wrapper such as `relocateAsync`,
`shutdownAsync`, or `awaitStopped` — you simply chain `await()` after the Java return value.

---

## `status()` / `observe().asFlow()` (read/observe)

Querying the host's current status calls Java's `status()` directly (a synchronous call that
needs no coroutine bridge). Streaming status changes uses the common `asFlow()` bridge that
connects Java's `Flow.Publisher` to Kotlin's `Flow`.

```kotlin
val status = frameworkRuntime.status()

frameworkRuntime.observe().asFlow().collect { observed ->
    // check observed.status(), observed.loss()
}
```

**Options.** This entry point has no modifiers.

**Completion result.** Cancelling `asFlow()` only releases that subscriber registration — it does
not cancel the shared runtime, the monitoring publisher, or a host operation already started. The
meaning of `ZLinkObservedStatus`'s `status()`/`loss()` is identical to the Java reference.

**When to use.** Same as the `status`/`observe` entry in the Java reference — just add the `Flow`
conversion to use it naturally from coroutine code.

---

See the
[Kotlin common runtime exact interface](../../common/spec/server/languages/kotlin/interfaces/common-runtime.en.md)
and
[Java reference 01. Host lifecycle](../../java/reference/01-host-lifecycle.ko.md) (Korean-only)
for the full rationale.
