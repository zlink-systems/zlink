# Java Public Interface Formal Contract

[Java contract table of contents](../README.en.md)

This directory fixes the exact public signature of the Java server
package, organized per feature. Common behavior is owned by the
[server common spec](../../../../README.en.md).

- [Common Runtime](common-runtime.en.md)
- [Configuration And Host](configuration-host.en.md)
- [Channel Messaging](channel-messaging.en.md)
- [Spot](spots.en.md)
- [Actor](actors.en.md)
- [STREAM Session](stream-session.en.md)
- [Location And Maintenance](location-maintenance.en.md)
- [Monitoring](monitoring.en.md)

Java and Kotlin share one JVM service runtime. A Kotlin coroutine
wrapper isn't put in the Java contract, and the Kotlin contract
separately specifies whether it reuses a Java type or provides a
Kotlin-only extension.

## Public API Structure

A Java application configures host and topology in
`ZLinkFrameworkOptions`, and processes messages through the Channel/
Spot/Actor/STREAM client and handler contracts. `ZLinkFrameworkRuntime`'s
mode-specified `Relocate` and `Shutdown` each own object relocation and
host termination, and a partial termination operation taking a MeshName
isn't provided. The Location provider provides read, version-conditional
atomic batch, and bounded snapshot scan on the opaque record the
framework creates. The Relocation provider stores an immutable blob at a
reference the framework issued in advance.

`ZLinkTopologyState` represents the availability of a registered
topology, and `ZLinkFrameworkRuntimeState` represents the whole host's
state. A Channel call only takes a process-local ChannelName. The first
argument of `sendToNode(String, RoutingId, Object)`, which directly
specifies a node, is
[MeshName](../../../../01-glossary.en.md#meshname).

ActorId and User/Instance SpotId are global logical IDs. A regular
message only takes the ID and resolves current
[authority](../../../../01-glossary.en.md#authority), and exact mutation
and session bind take an `ActorRef` or `SpotRef`. A
[MeshNode](../../../../01-glossary.en.md#meshnode)'s object role is
closed to `None`, `Client`, `Server`, and Client/Server require a
Location Store.

The exact type, constructor, method, record component, enum value, and
generic bound are owned by the per-feature documents above. Internal
Core/bindings types and `runtime.internal` types aren't exposed in the
application public signature. The Spring starter only provides the
public bean's type, singleton lifetime, and identity as contract. The
concrete type of auto-configuration classes, bean factory methods, and
lifecycle adapters isn't included in the exact public interface.

The valid range of public generation, revision, epoch, and sequence
ordinal is a positive `long`, i.e. `1..Long.MAX_VALUE`. On reaching the
maximum value, the framework treats it as terminal exhaustion, without
wrap or value reuse. `0` is only used when the relevant contract
explicitly specifies it to represent a not-yet-determined value.
