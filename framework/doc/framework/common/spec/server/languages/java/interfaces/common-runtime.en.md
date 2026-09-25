# Java Common Runtime Public Interface

[Interface table of contents](README.en.md) · [Host Relocation And Termination Contract](../../../05-location-relocation/05-host-relocation-flow.en.md)

This document fixes the public types expressing host execution state,
object relocation, termination request, and common async operations in
Java. The common document defines behavior — the declarations below show
the shape of the types and members used in Java.

The public contract is owned by the `systems.zlink.framework` Java
module. This module only exports the application packages recorded in
this per-language interface, plus `runtime.host`, to general consumers. The raw
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

public enum ZLinkListenerKind {
 ROUTE_MESH, CLIENT_SERVER, FANOUT, STREAM
}

public record ZLinkListenerStatus(
 ZLinkListenerKind kind,
 String name,
 String endpoint,
 Instant observedAt) {}

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
 public ZLinkListenerStatus listenerStatus(
 ZLinkListenerKind kind, String name);
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

`listenerStatus(kind, name)` is the Java projection of the
[common listener status query](../../../02-channel-transport/04-network-listener-identity.en.md#31-listener-state-the-publisher-checks).
`kind` selects RouteMesh, ClientServer, Fanout, or STREAM, and `name` is that
registration name. When the query ends with a configuration error, it fails with
`ZLinkFrameworkErrorKind.NOT_CONFIGURED`. The returned status's `observedAt` is
the observation time.

The canonical declaration of `ZLinkObservedStatus<T>` and
`ZLinkObservationLoss`, which `observe()` delivers, is owned by the
[Monitoring Public Interface](monitoring.en.md). This document only
fixes the fact that the host status stream uses the same envelope.

`relocate(options)` and `shutdown()` are host lifecycle APIs. Java delivers the shared
result through a per-call `CompletableFuture` view; cancellation affects only that waiter.
[Host relocation flow](../../../05-location-relocation/05-host-relocation-flow.en.md)
defines target, deadline, commit, and shutdown.
Java maps an invalid mode/version combination to `IllegalArgumentException`.

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

## Public Member `javap` Inventory

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
public final class systems.zlink.framework.monitoring.ZLinkListenerKind extends java.lang.Enum<systems.zlink.framework.monitoring.ZLinkListenerKind> {
 public static final systems.zlink.framework.monitoring.ZLinkListenerKind ROUTE_MESH;
 public static final systems.zlink.framework.monitoring.ZLinkListenerKind CLIENT_SERVER;
 public static final systems.zlink.framework.monitoring.ZLinkListenerKind FANOUT;
 public static final systems.zlink.framework.monitoring.ZLinkListenerKind STREAM;
 public static systems.zlink.framework.monitoring.ZLinkListenerKind[] values();
 public static systems.zlink.framework.monitoring.ZLinkListenerKind valueOf(java.lang.String);
}
public final class systems.zlink.framework.monitoring.ZLinkListenerStatus extends java.lang.Record {
 public systems.zlink.framework.monitoring.ZLinkListenerStatus(systems.zlink.framework.monitoring.ZLinkListenerKind, java.lang.String, java.lang.String, java.time.Instant);
 public systems.zlink.framework.monitoring.ZLinkListenerKind kind();
 public java.lang.String name();
 public java.lang.String endpoint();
 public java.time.Instant observedAt();
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
 public default <T> systems.zlink.framework.ZLinkEncodedPayload serialize(T, java.lang.Class<?>);
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
[Common Error Model](../../../00-foundation/07-framework-error-model.en.md). The
public exception doesn't provide whether it's retryable.

## Serializer And Error Public Signature

The first `decode(Class<T>)` on a received `ZLinkMessage` fixes either a value or a failure.
A later call with the same type returns the same value without invoking the serializer
again. A call with another type also does not deserialize again and ends with
`TYPE_MISMATCH` if that type cannot accept the first value. If the first call failed, that
failure is delivered again.

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
 public static systems.zlink.framework.messaging.ZLinkMessage of(java.lang.Object, java.lang.Class<?>);
 public static systems.zlink.framework.messaging.ZLinkMessage fromEncoded(systems.zlink.framework.ZLinkEncodedPayload, systems.zlink.framework.ZLinkMessageSerializer);
 public boolean isEmpty();
 public java.lang.Class<?> declaredType();
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
