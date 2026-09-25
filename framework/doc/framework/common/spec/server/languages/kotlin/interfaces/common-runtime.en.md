# Kotlin Common Runtime Public Interface

[Interface table of contents](README.en.md) · [Java Common Runtime](../../java/interfaces/common-runtime.en.md)

Kotlin uses Java's `ZLinkTopologyState`, `ZLinkFrameworkRuntimeState`,
`ZLinkFrameworkRelocationMode`, `ZLinkFrameworkRelocationOptions`,
relocation/termination outcome/reason/result, and
`ZLinkFrameworkRuntime` unchanged. It doesn't add the same enum, options,
result wrapper, or runtime facade. There's no separate drain facade or
partial termination member taking a MeshName — Kotlin uses the Java
host's `relocate()` and `shutdown()` unchanged.

Kotlin also uses Java's `ZLinkListenerKind` and `ZLinkListenerStatus`.
`ZLinkFrameworkRuntime.listenerStatus(kind, name)` returns the current
advertised endpoint after the selected listener has completed bind; it
doesn't add a Kotlin-specific wrapper.

The source owner of the call, handler, and coroutine lifecycle adapter
the Kotlin artifact directly declares is
`systems/zlink/framework/kotlin/contracts/`. Even if the source moves to
this boundary, the package declaration keeps
`systems.zlink.framework.kotlin`, so this document's public FQN doesn't
change. The Java contract isn't redeclared in Kotlin source.

The handler filter also uses Java's `ZLinkHandlerFilter`,
`ZLinkHandlerFilterContext`, `ZLinkHandlerFilterNext<T>`, and
`ZLinkHandlerDispatchKind` unchanged. A Kotlin-only filter context or
dispatch enum isn't added. An existing Kotlin filter implementation must
change `invoke`'s first argument from `ZLinkMessageContext` to
`ZLinkHandlerFilterContext`. While the host passes continuity preflight
and prepares a relocation unit, the Java enum's `RELOCATING(2)` is
observed, and once relocation finishes, it transitions to `RELOCATED(3)`.
Starting `shutdown()` transitions to `DRAINING(4)`.

## Kotlin Source Signature

```kotlin
public suspend fun <T> CompletionStage<T>.await(): T
```

This function preserves the success value and failure cause of Java's
`CompletionStage`. Coroutine cancellation only ends the waiting
continuation and doesn't cancel an already-started shared host
operation.

Kotlin uses the Java host relocation modes and result enum.
[Host relocation flow](../../../05-location-relocation/05-host-relocation-flow.en.md)
defines target selection, deadline, commit, and shutdown.
Cancellation of `CompletionStage.await()` affects only the waiting continuation.

```kotlin
val relocation = frameworkRuntime.relocate(
 ZLinkFrameworkRelocationOptions(
 ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
 12L, // only selects a Ready node that exactly matches this version.
 Duration.ofSeconds(30)
 )
).await()
val stopped = frameworkRuntime.shutdown().await()
```

There's no separate `relocateAsync`, `shutdownAsync`, `drain`, or
`awaitStopped` extension. The relocation result is
`ZLinkFrameworkRelocationResult`, and the host termination result is
`ZLinkFrameworkTerminationResult`, waited on with `CompletionStage.await()`.

## Generated JVM Signature

The JVM signature below is the generated form of the Kotlin source
contract.

```java
public final class systems.zlink.framework.kotlin.ZLinkCoroutineTurnAwaitKt {
 public static final <T> java.lang.Object await(java.util.concurrent.CompletionStage<T>, kotlin.coroutines.Continuation<? super T>);
}
```
