# Kotlin Configuration And Host Public Interface

[Interface table of contents](README.en.md) · [Java Configuration](../../java/interfaces/configuration-host.en.md) ·
[MeshNode Common Contract](../../../03-spot-actor/03-mesh-node.en.md)

A Kotlin application directly uses the Java builder. The Kotlin DSL is
only provided when a receiver and reified type genuinely reduce
duplication, and doesn't create a role, factory default, or allocation
provider that isn't in the Java contract. So ClientServer's Client-only
connect and the intent merging per Server RID/lifecycle generation, and
fanout's Subscriber-only connect and the ban on mixing automatic/manual
subscriber, apply the same contract as
[Java Configuration](../../java/interfaces/configuration-host.en.md)
unchanged. On the same ClientServer ChannelName, the Java builder's
`client()` and `server()` can each be registered once, without adding a
separate Kotlin DSL or public API. The two roles share one topology
through separate registrations under the `(ChannelName, Role)` key, and
a duplicate of the same role is a startup error. A local Server is also
selected under the same readiness/weight/drain conditions as a remote
Server, without local priority or calling a handler directly.

Automatic RouteMesh compares RID in canonical byte order, and only the
MeshNode with the smaller RID connects to the counterpart endpoint. A
manual topology can connect from one or both sides depending on
application endpoint configuration. If bidirectional connection or
automatic discovery contention/a stale snapshot creates a duplicate
candidate, handshake and admission check the same RID and
[lifecycle generation](../../../00-foundation/02-glossary.en.md#lifecycle-generation)
and keep only one in ready state.

A peer connection isn't needed only when both MeshNodes are Object
Client and neither has RouteMesh Channel Server membership. The same
applies when only Channel Client membership is registered. If either
side has Channel Server membership, including weight `0`, a connection
is needed. ClientServer and classic fanout are separate physical
topologies, so they aren't included in this judgment.

A [MeshNode](../../../00-foundation/02-glossary.en.md#meshnode)'s object role is
one of `None`, `Client`, `Server`. Not calling `objects()` means
`None`; `client()` provides an outbound manager and resolve; `server()`
provides Client capability plus [factory](../../../00-foundation/02-glossary.en.md#factory)/
Entry registration together. Client and Server need a Location Store.
None has no object manager or factory. Selecting a duplicate role on
one node is a startup configuration error. A RouteMesh Channel Server
can also be registered on an Object Client, but an application Node
direct handler can't be registered. Specifying an Object Client RID as
a Node direct target ends as not-found without switching to a different
RID.

`ZLinkFrameworkOptions.addLocationStore(...)` and
`addRelocationStore(...)` use the Java public member unchanged. If even
one factory selected `recreateOnRelocation()` or `preserveStateWith(...)`,
or even one Instance Spot factory exists, exactly one Relocation Store
must be registered. A missing or duplicate registration is a
configuration error before socket bind. A Relocation Store isn't
required for a same-node configuration with no
[Instance Spot](../../../00-foundation/02-glossary.en.md#entry-user-instance-spot)
factory that only selected `disableRelocation()`. A Kotlin DSL or
Redis-specific registration helper bundling both capabilities isn't
provided. The application state/queue/timer handoff payload of a
cross-node Actor/[Spot](../../../00-foundation/02-glossary.en.md#spot) move isn't
stored in the Relocation Store. The source keeps the payload in memory
and transfers it as chunks directly over the source–target ordered mesh
connection, and source memory is the restore origin. The Relocation
Store keeps owning the Instance Spot cold activation record and the
terminal record of a pending request completed after relocation, so the
registration requirement above is kept. A
same-node Actor join doesn't create a relocation payload, and a
cross-node move on a factory that selected `disableRelocation()` is
rejected before capture.

The following Java builder members are called directly from Kotlin with
the same JVM signature, without a property conversion.

```java
public enum systems.zlink.framework.configuration.ZLinkCoreHwmProfile {
 COMPACT,
 LOW_LATENCY,
 BALANCED,
 THROUGHPUT
}
public enum systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile {
 COMPACT,
 LOW_LATENCY,
 BALANCED,
 THROUGHPUT
}
public interface systems.zlink.framework.configuration.ZLinkDispatchOptions {
 public abstract systems.zlink.framework.configuration.ZLinkUnhandledDispatchOptions unhandled();
 public abstract systems.zlink.framework.configuration.ZLinkDiagnosticsOptions diagnostics();
 public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode);
 public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions traceSampleRate(double);
 public abstract systems.zlink.framework.configuration.ZLinkDispatchOptions includeMessageSizes(boolean);
}
public interface systems.zlink.framework.configuration.ZLinkInboundDispatchOptions {
 public abstract java.util.OptionalLong coreHwmMemoryLimitBytes();
 public abstract void setCoreHwmMemoryLimitBytes(long);
 public abstract java.util.OptionalLong coreHwmBudgetBytes();
 public abstract void setCoreHwmBudgetBytes(long);
 public abstract systems.zlink.framework.configuration.ZLinkCoreHwmProfile coreHwmProfile();
 public abstract void setCoreHwmProfile(systems.zlink.framework.configuration.ZLinkCoreHwmProfile);
 public abstract systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile applicationJobQueueProfile();
 public abstract void setApplicationJobQueueProfile(systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile);
 public abstract java.util.OptionalLong maxQueuedApplicationJobs();
 public abstract void setMaxQueuedApplicationJobs(long);
 public abstract int applicationJobQueuePauseThresholdPercent();
 public abstract void setApplicationJobQueuePauseThresholdPercent(int);
 public abstract int applicationJobQueueResumeThresholdPercent();
 public abstract void setApplicationJobQueueResumeThresholdPercent(int);
}
public interface systems.zlink.framework.locations.ZLinkLocationOptions {
 public abstract java.time.Duration ownerLeaseRenewInterval();
 public abstract void setOwnerLeaseRenewInterval(java.time.Duration);
 public abstract java.time.Duration ownerLeaseTtl();
 public abstract void setOwnerLeaseTtl(java.time.Duration);
 public abstract java.time.Duration pollingInterval();
 public abstract void setPollingInterval(java.time.Duration);
 public abstract java.time.Duration storeFailureGrace();
 public abstract void setStoreFailureGrace(java.time.Duration);
 public abstract java.time.Duration ownerLeaseFencingMargin();
 public abstract void setOwnerLeaseFencingMargin(java.time.Duration);
 public abstract java.time.Duration ownerLeaseRenewTimeout();
 public abstract void setOwnerLeaseRenewTimeout(java.time.Duration);
 public abstract java.time.Duration routeCacheMaxAge();
 public abstract void setRouteCacheMaxAge(java.time.Duration);
 public abstract java.time.Duration messageFollowDuration();
 public abstract void setMessageFollowDuration(java.time.Duration);
 public abstract java.time.Duration sessionRelocationSealTimeout();
 public abstract void setSessionRelocationSealTimeout(java.time.Duration);
 public abstract long relocationPayloadChunkLimitBytes();
 public abstract void setRelocationPayloadChunkLimitBytes(long);
 public abstract long relocationInFlightPayloadBudgetBytes();
 public abstract void setRelocationInFlightPayloadBudgetBytes(long);
 public abstract long relocationNodeInFlightPayloadBudgetBytes();
 public abstract void setRelocationNodeInFlightPayloadBudgetBytes(long);
 public abstract java.time.Duration relocationCutoverWaitTimeout();
 public abstract void setRelocationCutoverWaitTimeout(java.time.Duration);
}
public interface systems.zlink.framework.configuration.ZLinkMeshNodeBuilder {
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setRoutingIdPrefix(java.lang.String);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setPlacementWeight(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActorCapacity(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setSpotCapacity(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshNodeBuilder setActivationConcurrency(int);
 public abstract systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder objects();
}
public interface systems.zlink.framework.configuration.ZLinkMeshObjectRoleBuilder {
 public abstract systems.zlink.framework.configuration.ZLinkMeshObjectClientBuilder client();
 public abstract systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder server();
}
public interface systems.zlink.framework.configuration.ZLinkStreamNodeBuilder {
 public abstract systems.zlink.framework.configuration.ZLinkStreamSocketConfig configureSocket();
 public abstract systems.zlink.framework.configuration.ZLinkStreamNodeBuilder enableActorDispatch();
}
public interface systems.zlink.framework.configuration.ZLinkStreamSocketConfig {
 public abstract long maxMessageSize();
 public abstract void setMaxMessageSize(long);
}
```

Core HWM and application-job-queue configuration is obtained directly
from `options.configureInboundDispatch()`, which returns
`ZLinkInboundDispatchOptions`; `configureDispatch { ... }` remains the
diagnostics DSL and does not forward HWM or job-queue settings.

`sessionRelocationSealTimeout()` is the same startup-only positive `Duration` as Java,
defaulting to three seconds. A non-millisecond-representable, zero, negative, or infinite
value is a configuration error before socket bind.

`relocationPayloadChunkLimitBytes()`, `relocationInFlightPayloadBudgetBytes()`,
`relocationNodeInFlightPayloadBudgetBytes()`, and `relocationCutoverWaitTimeout()` also use
the Java public contract unchanged. The chunk limit is the maximum size in bytes of one
encoded chunk of a relocation payload, defaulting to 256 KiB; setting it above the frame
limit the transport negotiated is a startup configuration error before socket bind. The
in-flight budget caps the sum of relocation chunk bytes concurrently in flight per peer
connection, defaulting to 16 MiB, with `0` meaning not applied. The node in-flight
budget applies the same rule to the node-wide sum and defaults to `0`, meaning not
applied. The cutover wait timeout is both the target's wait for cutover and the time
the source keeps its boundary batch copy for retransmission, defaulting to one second.
All four values are startup-only, and negative values are a configuration error before
socket bind.

Kotlin uses Java's `ZLinkStreamNodeBuilder.configureSocket()` and
`ZLinkStreamSocketConfig.setMaxMessageSize(...)` unchanged. The default is
`64 KiB`, and the setting applies only to complete client-to-server messages
received by a StreamNode through Core STREAM. The size is header bytes plus
payload bytes, excluding the 6-byte prefix. `0` maps to Core `-1`, so
Framework adds no limit; a negative value is a startup configuration error.
A message over the limit is never partly delivered to the handler. The
server records `EMSGSIZE` and a diagnostic trace, then closes the connection.
The raw client observes the close without a separate wire error code. The
limit doesn't apply to server-to-client outbound messages, and the setting
isn't added to ClientServer or RouteMesh SS.

The Kotlin binding forwards a positive finite Java `Runtime.maxMemory()` value to Core as
its runtime memory hint. Core and application-job-queue profiles use the Java public
contract's independent enums and calculations, both defaulting to `BALANCED`. Pressure thresholds
default to pause `80` and resume `60`; pause is an integer in `1..100`, resume is an integer in
`0..99`, and resume must be less than pause. Manual job cap, startup CPU snapshot, and pre-bind
bounds, ordering, and overflow validation also match the Java public contract.

## Kotlin Source Signature

```kotlin
fun ZLinkFrameworkOptions.useCoroutineHandlers(dispatcher: CoroutineDispatcher)
fun ZLinkFrameworkOptions.useCoroutineHandlers(
 scope: CoroutineScope,
 dispatcher: CoroutineDispatcher,
)

inline fun ZLinkFrameworkOptions.configureDispatch(
 block: ZLinkDispatchOptions.() -> Unit,
): ZLinkDispatchOptions

fun ZLinkFrameworkOptions.configureStreamCompression(
 configure: ZLinkStreamCompressionBuilder.() -> Unit,
): ZLinkFrameworkOptions

inline fun <reified TActor, reified TFactory>
 ZLinkMeshObjectServerBuilder.actorFactory(
 actorType: String,
 noinline configure: ZLinkActorFactoryBuilder<TActor>.() -> Unit,
 ): ZLinkMeshObjectServerBuilder
 where TActor : ZLinkActor,
 TFactory : ZLinkActorFactory
```

The factory configure callback has no default. The Actor factory
builder has no setting other than relocation behavior selection. Node
placement [weight](../../../00-foundation/02-glossary.en.md#weight) is 0..10000,
defaulting to 100. An out-of-range value is a configuration error in
both startup config and runtime change. It's independent of Channel
weight, and the same value is used for runtime update and the
descriptor [snapshot](../../../00-foundation/02-glossary.en.md#snapshot).
RouteMesh Channel Server and ClientServer Server weight also use the
same range and default. Weighted selection computes the sum of
candidate weight using at least a 64-bit integer.

The automatic RID of a MeshNode and store-backed fanout publisher has
the format `prefix-<lowercase-canonical-uuid-v4>`. UUID v4 is
represented as a lowercase canonical string in `8-4-4-4-12` digit
groups. Prefix is ASCII `[A-Za-z0-9._-]` 1..64 characters, and the full
RID is at most 255 UTF-8 bytes. On conflict with an active owner, it
fails immediately with `RoutingIdConflict` instead of retrying with a
new UUID. A fixed RID is allowed in an automatic discovery topology and on a MeshNode that has an
object role. Implementation and test scenarios sometimes need to name a specific peer, and an
auto-assigned UUID cannot be named. When a node using a fixed RID restarts and collides with the
previous active owner claim, the same rule as an automatic RID applies — it does not retry with a
new value but fails immediately with a conflict, and the restart succeeds once the previous owner
lease expires. There's no slot
count, allocation group, or public allocation provider.

The Object Server's Entry Spot ID has the same-prefix format
`<prefix>-entry-<lowercase-canonical-uuid-v4>`, using a UUID v4
generated separately from the MeshNode. Java's
`ZLinkMeshNodeDescriptor.entrySpotId()` provides the mapping for
the same lifecycle. If the global Spot ID conflicts with an active
owner, startup fails immediately with `SpotIdConflict` instead of
retrying with a new UUID. If a caller-specified User/Instance Spot ID
matches the reserved format, it's rejected as a startup configuration
error before the Store and factory.

Every factory configures the Java builder with a Kotlin receiver
callback. The callback calls exactly one of `disableRelocation()`,
`recreateOnRelocation()`, `preserveStateWith(...)`. Omitting it or
calling more than one is a startup configuration error before socket
bind. A Kotlin-only policy value or suspending adapter isn't added. There
is no separate registration API.

The framework runs the receiver callback synchronously exactly once
inside the registration call. Calling the retained builder again after
the callback returns is a configuration error. If the callback throws,
that factory isn't registered and the same exception is propagated to
the caller. If `stableTypeLimit(...)` is omitted, it shares the node
limit, and an explicit value must be 1..`Int.MAX_VALUE`. 0 and a
negative value are a configuration error during callback execution.

An Object Server with even one factory that selected
`recreateOnRelocation()` or `preserveStateWith(...)`, or even one
registered Instance Spot factory, registers exactly one Relocation Store
with the Java root's `addRelocationStore(...)`. Only a same-node
configuration with no Instance Spot factory where every factory
selected `disableRelocation()` can omit this.

## Generated JVM Signature

```java
public final class systems.zlink.framework.kotlin.ZLinkCoroutineHandlerOptionsKt {
 public static final void useCoroutineHandlers(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlinx.coroutines.CoroutineDispatcher);
 public static final void useCoroutineHandlers(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlinx.coroutines.CoroutineScope, kotlinx.coroutines.CoroutineDispatcher);
}
public final class systems.zlink.framework.kotlin.ZLinkDispatchOptionsExtensionsKt {
 public static final systems.zlink.framework.configuration.ZLinkDispatchOptions configureDispatch(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkDispatchOptions, kotlin.Unit>);
}
public final class systems.zlink.framework.kotlin.ZLinkFrameworkExtensionsKt {
 public static final <TActor extends systems.zlink.framework.actors.ZLinkActor, TFactory extends systems.zlink.framework.actors.ZLinkActorFactory> systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder actorFactory(systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder, java.lang.String, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkActorFactoryBuilder<TActor>, kotlin.Unit>);
 public static final systems.zlink.framework.configuration.ZLinkFrameworkOptions configureStreamCompression(systems.zlink.framework.configuration.ZLinkFrameworkOptions, kotlin.jvm.functions.Function1<? super systems.zlink.framework.configuration.ZLinkStreamCompressionBuilder, kotlin.Unit>);
}
```
