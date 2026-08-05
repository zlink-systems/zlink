# Kotlin Monitoring Public Interface

[Interface table of contents](README.en.md) · [Java Monitoring](../../java/interfaces/monitoring.en.md)

Kotlin uses Java's RouteMesh, ClientServer, automatic fanout, and host
runtime status types. The `Flow` projection only connects the Java
publisher to coroutine cancellation — it doesn't define a separate
status or event value. Each item is a complete status after a change,
not an event holding only some fields.

The host's inbound dispatch state also uses Java's
`ZLinkInboundDispatchStatus` unchanged. A Kotlin-only data class or
`Flow` aggregation isn't added. Every byte value is a non-negative Java
`long`, and `pendingPayloadBytes == queuedPayloadBytes +
activePayloadBytes` holds.

Endpoint, lifecycle generation, and descriptor source are kept only for
the framework to judge a stale descriptor and connection. Admission/
claim/reservation, pending work, and connection intent also aren't added
to the Kotlin projection.

A RouteMesh peer uses Java's `ZLinkPeerState` unchanged.
`NOT_CONNECTED` is a state where a connection is needed but there's no
ready connection, and `NOT_REQUIRED` is a normal state where neither
Object Client has RouteMesh Channel Server membership so a connection
isn't needed. The same applies when only Channel Client membership is
registered. If either side has Channel Server membership, including
weight `0`, absence of connection is marked `NOT_CONNECTED`. The two
states aren't merged into a Kotlin-only boolean or string.
`NOT_REQUIRED` is excluded from ready peer count and liveness/health
failure aggregation.

The topology runtime uses Java `ZLinkFrameworkRuntime`'s
`routeMeshRuntime()`, `clientServerRuntime()`, and `fanoutRuntime()`
unchanged. A Kotlin wrapper accessor isn't added, and the topology bean
injected in Spring has the same reference identity as the return value
of that Java accessor. As with the Java monitoring contract, MeshNode
status has no Logical Multicast statistics, publish target count, or
per-target admission/failure fields. A Kotlin-only projection doesn't
add these.

The ClientServer target and fanout publisher use Java's `ZLinkPeerState`
and `ZLinkTopologyReason` unchanged. A Kotlin-only connection status
enum isn't created. The local role of a
[snapshot](../../../../01-glossary.en.md#snapshot) that registered
Client and Server together on the same ChannelName is represented by
Java's `ZLinkClientServerRole.CLIENT_AND_SERVER`. This is only an
aggregate projection of two separate role registrations, not a builder
role or registration key. A Kotlin-only enum or conversion value isn't
created.

Fanout ready semantics also use the Java contract unchanged. The
publisher-dedicated SUB socket's native-ready alone doesn't become
[ready](../../../../01-glossary.en.md#ready) — the first valid
application record or liveness beacon must also be received on the same
socket. The 15-second inbound timeout changes that publisher's peer
state to `NOT_CONNECTED`.

An error occurring in an internal runtime callback or observer is
recorded by the framework as a structured log. An error sink and raw
event DTO the Kotlin application implements or registers aren't the
public contract.

[RouteMesh](../../../../01-glossary.en.md#routemesh) placement status
only provides whether new objects can be accepted and the current
process's active Actor/Spot count. Node-wide placement weight,
per-stable-type capacity, pending activation, and reservation failure
are internal placement judgment values, so they aren't made public.
`isAvailable` is only `true` when the host is `SERVING` and Object
Server, placement weight is positive, and both Actor/Spot capacity and
activation concurrency have room. Activation's current value and limit
also aren't added to the Kotlin projection.

## Framework Error Values

Kotlin uses Java's `ZLinkFrameworkErrorKind` unchanged. The enum name
and number are part of the public exception classification and fix the
following values.

```text
NOT_FOUND = 0
ALREADY_EXISTS = 1
TYPE_MISMATCH = 2
NOT_CONFIGURED = 3
REJECTED = 4
UNAVAILABLE = 5
CAPACITY_EXCEEDED = 6
DEADLINE_EXCEEDED = 7
SHUTTING_DOWN = 8
PROTOCOL_ERROR = 9
INVALID_OPERATION = 10
DATA_LOST = 11
INTERNAL_FAILURE = 12
```

A remote framework error is delivered as `ZLinkFrameworkException`.
Public argument validation uses the JVM standard
`IllegalArgumentException`, and a startup configuration conflict uses
`ZLinkConfigurationException`. The public exception doesn't provide
whether it's retryable.

## Kotlin Source Signature

```kotlin
fun ZLinkDispatchOptions.onMessageFlow(
    observer: (ZLinkMessageFlowEvent) -> Unit,
): ZLinkDispatchOptions
```

When reading the Java `Publisher` status stream as a Kotlin `Flow`, the
common `asFlow()` bridge owned by
[Location And Maintenance](location-maintenance.en.md) is used. This
bridge's cancellation only releases that subscriber registration. It
doesn't cancel the shared runtime, monitoring publisher, or an
already-started host operation. The `onMessageFlow` generated JVM
member is included in
[Configuration And Host](configuration-host.en.md)'s multifile class
inventory.
