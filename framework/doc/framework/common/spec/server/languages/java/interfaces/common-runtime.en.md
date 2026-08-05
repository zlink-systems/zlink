# Java Common Runtime Public Interface

[Interface table of contents](README.en.md) · [Host Relocation And Termination Contract](../../../../28-graceful-drain-handoff.en.md)

This document fixes the public types expressing host execution state,
object relocation, termination request, and common async operations in
Java. The common document defines behavior — the declarations below show
the exact shape of the types and members used in Java.

The public contract is owned by the `systems.zlink.framework` Java
module. This module only exports the application packages recorded in
this exact interface, plus `runtime.host`, to general consumers. The raw
binding implementation is in a separate internal artifact and isn't
included in the application compile classpath. Even in the named module,
only the packages the Framework companion module needs are exported. So
raw binding types aren't included in the public API inventory.

```java
public enum ZLinkFrameworkRuntimeState {
    PREPARING(0), SERVING(1), RELOCATING(2), RELOCATED(3),
    DRAINING(4), STOPPED(5), ERROR(6);
    private final int wireValue;
    ZLinkFrameworkRuntimeState(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkRelocationOutcome {
    RELOCATED(0), BLOCKED(1);
    private final int wireValue;
    ZLinkFrameworkRelocationOutcome(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkRelocationMode {
    PLANNED_MAINTENANCE(0), ROLLING_UPDATE(1);
    private final int wireValue;
    ZLinkFrameworkRelocationMode(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkRelocationReason {
    NONE(0), TARGET_UNAVAILABLE(1), STORE_UNAVAILABLE(2),
    RELOCATION_DISABLED(3), STATE_INCOMPATIBLE(4),
    DEADLINE_EXCEEDED(5), RELOCATION_FAILED(6),
    RUNTIME_NOT_READY(7), MANUAL_TOPOLOGY_UNSUPPORTED(8),
    SHUTDOWN_REQUESTED(9), OPERATION_IN_PROGRESS(10);
    private final int wireValue;
    ZLinkFrameworkRelocationReason(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public record ZLinkFrameworkRelocationOptions(
    ZLinkFrameworkRelocationMode mode,
    Long targetApplicationVersion,
    Duration deadline) {}

public record ZLinkFrameworkRelocationResult(
    ZLinkFrameworkRelocationMode mode,
    long effectiveTargetApplicationVersion,
    ZLinkFrameworkRelocationOutcome outcome,
    ZLinkFrameworkRelocationReason reason) {}

public enum ZLinkFrameworkTerminationOutcome {
    STOPPED(0), FORCE_STOPPED(1);
    private final int wireValue;
    ZLinkFrameworkTerminationOutcome(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkTerminationReason {
    NONE(0), DEADLINE_EXCEEDED(1), TEARDOWN_FAILED(2);
    private final int wireValue;
    ZLinkFrameworkTerminationReason(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public record ZLinkFrameworkTerminationResult(
    ZLinkFrameworkTerminationOutcome outcome,
    ZLinkFrameworkTerminationReason reason) {}

public final class ZLinkFrameworkRuntime
    implements AutoCloseable, ZLinkMessageFlowControl {
    public ZLinkClient client();
    public void setMessageFlowMode(ZLinkMessageFlowLogMode mode);
    public ZLinkMessageFlowLogMode messageFlowMode();
    public ZLinkFanoutClient fanout();
    public ZLinkRouteClient route();
    public ZLinkRouteMeshRuntime routeMeshRuntime();
    public ZLinkClientServerRuntime clientServerRuntime();
    public ZLinkFanoutRuntime fanoutRuntime();
    public ZLinkSpotManager spotManager();
    public ZLinkSpotOutbound spotOutbound();
    public ZLinkSpotPublisherClient spotPublisherClient();
    public ZLinkLocationRuntimeQuery monitoringLocationRuntimeQuery();
    public ZLinkLocationReadiness locationReadiness();
    public boolean stopSpotRuntime();
    public ZLinkActorManager actorManager();
    public ZLinkActorClient actorClient();
    public ZLinkSessionActorsRuntime sessionActors(String streamNodeName, RoutingId sessionRid);

    public ZLinkFrameworkRuntimeStatus status();
    public Flow.Publisher<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> observe();
    public CompletionStage<ZLinkFrameworkRelocationResult> relocate(
        ZLinkFrameworkRelocationOptions options);
    public CompletionStage<ZLinkFrameworkTerminationResult> shutdown();
    public CompletionStage<ZLinkFrameworkTerminationResult> shutdown(Duration deadline);
    public void close();
}
```

The canonical declaration of `ZLinkObservedStatus<T>` and
`ZLinkObservationLoss`, which `observe()` delivers, is owned by the
[Monitoring Public Interface](monitoring.en.md). This document only
fixes the fact that the host status stream uses the same envelope.

`relocate(options)` closes new application admission and placement, and
moves current objects to a compatible target. On success it becomes
`RELOCATED` state, and the host process and infrastructure connections
are kept. A User Spot moves the [Spot](../../../../01-glossary.en.md#spot)
and the entire current member Actor set together as one aggregate. There's
no fixed cap on the total participant count. If even one aggregate
participant selected `disableRelocation()`, it ends with
`Blocked/RelocationDisabled`; if target/capacity/reservation can't be
secured, `Blocked/TargetUnavailable`; if application version/type/
state-preservation adapter capability doesn't match,
`Blocked/StateIncompatible`. This preflight failure doesn't change
admission. The mere existence of a
[User Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
doesn't block relocation. If there's even one local manual RouteMesh
peer, ClientServer client endpoint, fanout subscriber endpoint, or
manual fanout publisher, it ends with
`Blocked/ManualTopologyUnsupported`. Automatic RouteMesh only
transitions to `RELOCATING` after the source's Core peer table has the
same RID/lifecycle generation as the descriptor admitted and ready.
`shutdown()` doesn't start a new relocation. Neither operation performs
a hidden remote `GetOrCreate`, and waiter cancellation doesn't cancel an
already-started shared operation. Each call returns a dedicated
`CompletableFuture` view following the shared operation's result.
`toCompletableFuture().cancel(...)` only releases that waiter — the host
operation keeps proceeding, and other waiters receive the same terminal
result. A separate public cancellation token or a host-operation-cancel
member isn't added. Calling `shutdown()` during `RELOCATING` only
confirms the currently running atomic relocation unit to a terminal
state and doesn't start the rest of relocation. The relocation waiter
receives `Blocked/ShutdownRequested`, and the host continues bounded
cleanup.

### Relocation Mode And Target Selection

The caller always specifies mode. `targetApplicationVersion` and
`deadline` are nullable components. If `deadline == null`, the
framework's default host relocation deadline is used.

- `PLANNED_MAINTENANCE` is used for a node check or reboot that keeps the
  same application version. `targetApplicationVersion` must be `null`,
  and the result's `effectiveTargetApplicationVersion` is the source
  host's application version.
- `ROLLING_UPDATE` is used when replacing with a new application
  version. `targetApplicationVersion` must be specified and must be
  greater than the source version. The framework only uses a node whose
  application version exactly matches this value as a target candidate,
  and doesn't substitute an intermediate version or a different, higher
  version.

If the mode and target version combination doesn't satisfy the above
conditions, the framework rejects the call with
`IllegalArgumentException` without changing admission or placement
state. The candidate selection order for a valid call is as follows.

1. `PLANNED_MAINTENANCE` keeps only a node matching the source version;
   `ROLLING_UPDATE` keeps only a node exactly matching the specified
   target version.
2. Keeps only an Object Server that isn't source and is `SERVING` on
   the same Mesh.
3. Confirms stable type, the relocation behavior selected on the
   factory, and adapter capability match.
4. Confirms population capacity and reservation availability, and
   excludes the same maintenance wave as source.
5. Keeps only a node whose Core peer with the matching RID and
   lifecycle generation on the same descriptor snapshot is `ADMITTED`.
6. Applies node-wide placement weight to the remaining candidates.

Since the version condition is applied first, even if capability or
capacity is sufficient, it doesn't fall back to a node of a different
version. If there's no version/wave/capacity or exact-ready target, it
re-checks until the deadline and then it's `Blocked/TargetUnavailable`.
If stable type, factory, relocation policy, or adapter doesn't match,
it's `Blocked/StateIncompatible`. A Store lookup failure is
`Blocked/StoreUnavailable`.

While the same shared relocation is running, a call with the same mode
and effective target version joins the existing operation and receives
the same terminal result. The first call's deadline fixes the shared
operation deadline, and a later joining call's deadline doesn't extend or
shorten the operation. A call whose mode or target version differs from
the running operation doesn't change the current operation or queue —
it returns `Blocked/OperationInProgress`. This result records the
rejected call's requested mode, and the source version for planned
maintenance or the requested target version for rolling update, as
`effectiveTargetApplicationVersion`.

If the deadline ends first before every target becomes `Prepared` and
the relocation commit is published, the relocation reference and
reservation are cleaned up in durable-abort order, source authority and
admission are restored, and `Blocked/DeadlineExceeded` is returned.
There's no rollback to source after commit — the remaining stages are
only processed while the same target process is running. If the target
process terminates, a different runtime doesn't automatically take over
the relocation, and `RELOCATED` isn't returned.

`ZLinkFrameworkRuntime` owns one monitoring view each for RouteMesh,
ClientServer, and automatic fanout. The three accessors return the same
object for the runtime's lifetime and don't create a new adapter per
call. The topology runtime bean the Spring starter provides also has the
same reference identity as the object these accessors return.

The Spring starter provides the `ZLinkFrameworkRuntime` bean. An
operational maintenance endpoint calls `shutdown()` when
`relocate(options)`'s result is `RELOCATED`. To terminate without
relocation, only `shutdown()` is called. There's no separate drain
facade or partial operation taking a MeshName.

The application doesn't start `ZLinkFrameworkRuntime` directly. Runtime
creation and start are owned by the Spring starter. So a `start(...)`
factory isn't included in the public contract. Core's internal bootstrap
is qualified-exported only to the starter module. Testkit keeps the same
package-access helper only in test source.

## Exact Public Member `javap` Inventory

The declarations below fix the Java public types and members in the
binary signature format `javap` prints.

```java
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState PREPARING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState SERVING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState RELOCATING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState RELOCATED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState DRAINING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState STOPPED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState ERROR;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome RELOCATED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome BLOCKED;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode PLANNED_MAINTENANCE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode ROLLING_UPDATE;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason NONE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason TARGET_UNAVAILABLE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason STORE_UNAVAILABLE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason RELOCATION_DISABLED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason STATE_INCOMPATIBLE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason DEADLINE_EXCEEDED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason RELOCATION_FAILED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason RUNTIME_NOT_READY;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason MANUAL_TOPOLOGY_UNSUPPORTED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason SHUTDOWN_REQUESTED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason OPERATION_IN_PROGRESS;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions extends java.lang.Record {
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions(systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode, java.lang.Long, java.time.Duration);
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode mode();
  public java.lang.Long targetApplicationVersion();
  public java.time.Duration deadline();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult extends java.lang.Record {
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult(systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode, long, systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome, systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason);
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode mode();
  public long effectiveTargetApplicationVersion();
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome outcome();
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason reason();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome STOPPED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome FORCE_STOPPED;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason NONE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason DEADLINE_EXCEEDED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason TEARDOWN_FAILED;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult extends java.lang.Record {
  public systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult(systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome, systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason);
  public systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome outcome();
  public systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason reason();
}
public interface systems.zlink.framework.ZLinkMessageContext {
  public abstract java.util.Optional<java.lang.String> meshName();
  public abstract java.util.Optional<java.lang.String> channelName();
  public abstract java.lang.String packetName();
  public abstract java.util.Optional<java.lang.String> contentType();
  public abstract java.util.Map<java.lang.String, java.lang.String> metadata();
  public abstract java.util.Optional<java.lang.String> correlationId();
}
public final class systems.zlink.framework.ZLinkHandlerDispatchKind extends java.lang.Enum<systems.zlink.framework.ZLinkHandlerDispatchKind> {
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind NODE_DIRECT_SEND;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind NODE_DIRECT_REQUEST;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind CHANNEL_SEND;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind CHANNEL_REQUEST;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind CLASSIC_FANOUT;
}
public interface systems.zlink.framework.ZLinkHandlerFilterContext extends systems.zlink.framework.ZLinkMessageContext {
  public abstract systems.zlink.framework.ZLinkHandlerDispatchKind dispatchKind();
}
public interface systems.zlink.framework.ZLinkHandlerFilter {
  public abstract <T> java.util.concurrent.CompletionStage<T> invoke(systems.zlink.framework.ZLinkHandlerFilterContext, systems.zlink.framework.ZLinkHandlerFilterNext<T>);
}
public interface systems.zlink.framework.ZLinkMessageSerializer {
  public abstract <T> systems.zlink.framework.ZLinkEncodedPayload serialize(T);
  public abstract <T> T deserialize(systems.zlink.framework.ZLinkEncodedPayload, java.lang.Class<T>);
  public default void prepare(java.lang.Class<?>);
}
public interface systems.zlink.framework.ZLinkHandlerFilterNext<T> {
  public abstract java.util.concurrent.CompletionStage<T> invoke();
}
public final class systems.zlink.framework.errors.ZLinkFrameworkErrorKind extends java.lang.Enum<systems.zlink.framework.errors.ZLinkFrameworkErrorKind> {
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind NOT_FOUND;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind ALREADY_EXISTS;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind TYPE_MISMATCH;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind NOT_CONFIGURED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind REJECTED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind UNAVAILABLE;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind CAPACITY_EXCEEDED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind DEADLINE_EXCEEDED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind SHUTTING_DOWN;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind PROTOCOL_ERROR;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind INVALID_OPERATION;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind DATA_LOST;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind INTERNAL_FAILURE;
  public static systems.zlink.framework.errors.ZLinkFrameworkErrorKind[] values();
  public static systems.zlink.framework.errors.ZLinkFrameworkErrorKind valueOf(java.lang.String);
  public int value();
  public static systems.zlink.framework.errors.ZLinkFrameworkErrorKind fromValue(int);
}
public class systems.zlink.framework.errors.ZLinkFrameworkException extends java.lang.RuntimeException {
  public systems.zlink.framework.errors.ZLinkFrameworkException(systems.zlink.framework.errors.ZLinkFrameworkErrorKind, java.lang.String);
  public systems.zlink.framework.errors.ZLinkFrameworkException(systems.zlink.framework.errors.ZLinkFrameworkErrorKind, java.lang.String, java.lang.Throwable);
  public systems.zlink.framework.errors.ZLinkFrameworkErrorKind kind();
}
```

`ZLinkFrameworkErrorKind.value()` returns the common number `0..12`
regardless of declaration order. `fromValue(int)` also uses the same
mapping as the
[Common Error Model](../../../../32-framework-error-model.en.md). The
public exception doesn't provide whether it's retryable.

## Serializer And Error Public Signature

```java
public final class systems.zlink.framework.ZLinkEncodedPayload {
  public static systems.zlink.framework.ZLinkEncodedPayload from(byte[]);
  public byte[] bytes();
}
public final class systems.zlink.framework.errors.ZLinkConfigurationException extends systems.zlink.framework.errors.ZLinkFrameworkException {
  public systems.zlink.framework.errors.ZLinkConfigurationException(java.lang.String);
  public systems.zlink.framework.errors.ZLinkConfigurationException(java.lang.String, java.lang.Throwable);
}
public final class systems.zlink.framework.messaging.ZLinkMessage {
  public static systems.zlink.framework.messaging.ZLinkMessage empty();
  public static systems.zlink.framework.messaging.ZLinkMessage of(java.lang.Object);
  public static systems.zlink.framework.messaging.ZLinkMessage fromEncoded(systems.zlink.framework.ZLinkEncodedPayload, systems.zlink.framework.ZLinkMessageSerializer);
  public boolean isEmpty();
  public <T> T decode(java.lang.Class<T>);
  public systems.zlink.framework.ZLinkEncodedPayload toEncodedPayload(systems.zlink.framework.ZLinkMessageSerializer);
}
public final class systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec implements systems.zlink.framework.configuration.ZLinkCodecExtension,systems.zlink.stream.connector.ZLinkStreamTypedCodec {
  public static systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec defaultCodec();
  public static systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec forPayloadTypes(java.util.function.Predicate<java.lang.Class<?>>);
  public <T> systems.zlink.stream.connector.ZLinkStreamEncodedPayload encode(java.lang.String, T);
  public <T> T decode(systems.zlink.stream.connector.ZLinkStreamEncodedPayload, java.lang.Class<T>);
  public void register(systems.zlink.framework.configuration.ZLinkCodecRegistrar);
}
public final class systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec implements systems.zlink.framework.configuration.ZLinkCodecExtension,systems.zlink.stream.connector.ZLinkStreamTypedCodec {
  public static systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec defaultCodec();
  public <T> systems.zlink.stream.connector.ZLinkStreamEncodedPayload encode(java.lang.String, T);
  public <T> T decode(systems.zlink.stream.connector.ZLinkStreamEncodedPayload, java.lang.Class<T>);
  public void register(systems.zlink.framework.configuration.ZLinkCodecRegistrar);
}
```
