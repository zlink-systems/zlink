# Kotlin Common Runtime Public Interface

[Interface table of contents](README.en.md) · [Java Common Runtime](../../java/interfaces/common-runtime.en.md)

Kotlin uses Java's `ZLinkTopologyState`, `ZLinkFrameworkRuntimeState`,
`ZLinkFrameworkRelocationMode`, `ZLinkFrameworkRelocationOptions`,
relocation/termination outcome/reason/result, and
`ZLinkFrameworkRuntime` unchanged. It doesn't add the same enum, options,
result wrapper, or runtime facade. There's no separate drain facade or
partial termination member taking a MeshName — Kotlin uses the Java
host's `Relocate` and `Shutdown` unchanged.

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

A User Spot moves the Spot and the entire current member Actor set
together as one aggregate, with no fixed cap on the total participant
count. So its mere existence isn't a relocation blocker. The relocation
reason for selecting `disableRelocation()`, absence of target, and
state-capability mismatch is the same as Java. If there's even one
local manual RouteMesh peer, ClientServer client endpoint, fanout
subscriber endpoint, or manual fanout publisher, it's `Blocked` with the
Java enum's `MANUAL_TOPOLOGY_UNSUPPORTED(8)`. Automatic RouteMesh only
transitions to `RELOCATING(2)` after the source's Core peer table has
the same RID/lifecycle generation as the descriptor admitted and ready.
The manual topology restriction isn't applied to `shutdown()`.

`PLANNED_MAINTENANCE(0)` requires `targetApplicationVersion == null`
and only uses a target of the same version as source. `ROLLING_UPDATE(1)`
must specify a target version greater than source and only uses a
candidate that exactly matches that value. The framework selects a
target in the order: version, `SERVING` Object Server that isn't
source, stable type/factory/adapter capability, capacity and a
different maintenance wave, an `ADMITTED` Core peer matching RID/
lifecycle generation, placement weight. It doesn't fall back to a
different version. If there's no version/wave/capacity or exact-ready
target, it re-checks until the deadline and then it's
`BLOCKED/TARGET_UNAVAILABLE`. If factory/policy/adapter doesn't match,
`BLOCKED/STATE_INCOMPATIBLE`; a Store lookup failure is
`BLOCKED/STORE_UNAVAILABLE`.

A concurrent call with the same mode and effective target version joins
the shared operation the first call started and receives the same
terminal result. The first options' deadline fixes the shared operation
deadline. If mode or target version differs, it doesn't change the
existing operation and returns `BLOCKED/OPERATION_IN_PROGRESS(10)`.
Kotlin doesn't provide a default mode or separate target selection
extension that abbreviates this rule.

As with Java, a deadline before every target becomes `Prepared` and the
relocation commit is published ends with `Blocked/DeadlineExceeded`
after durable abort and source normalization. There's no rollback to
source after commit — the remaining stages are only processed while the
same target process is running. If the target process terminates, a
different runtime doesn't automatically take over the relocation, and
`Relocated` isn't returned. Calling `shutdown()` during `RELOCATING`
only finishes the currently running atomic unit, and the relocation
waiter receives `Blocked/ShutdownRequested`. A Kotlin enum or result
isn't added.

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

## Exact Generated JVM Signature

The JVM signature below is the generated form of the Kotlin source
contract.

```java
public final class systems.zlink.framework.kotlin.ZLinkCoroutineTurnAwaitKt {
  public static final <T> java.lang.Object await(java.util.concurrent.CompletionStage<T>, kotlin.coroutines.Continuation<? super T>);
}
```
