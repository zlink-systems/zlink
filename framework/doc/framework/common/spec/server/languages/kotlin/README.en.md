# ZLink Framework Kotlin Public Contract

This directory owns the Kotlin-only public contract `zlink-framework-kotlin`
adds on top of the Java runtime. Java types and methods used as-is follow
the [Java Public Contract](../java/README.en.md) and aren't copied and
redefined here.

The Kotlin coroutine and DSL signatures are based on the
[Per-Feature Interfaces](interfaces/README.ko.md). A server extension
waiting on a Java API is also included in this directory's formal
contract. Kotlin source and contract tests must follow this contract.
The client Stream Connector's coroutine wrapper and `Flow` surface are
owned by a separate
[Java/Kotlin Stream Connector Contract](../../../stream-connector/languages/java/03-stream-connector.en.md).

Host relocation uses Java's mode/options/result types unchanged.
Planned maintenance only uses the same application version as source,
and rolling update only uses a target that exactly matches the
caller-specified higher application version. A Kotlin-only default mode
or target selection extension isn't provided.

A single ChannelName call, RouteMesh/ClientServer role builder, listener
network identity, handler context, and dedicated descriptor/runtime all
reuse the Java formal types, with only the Kotlin DSL projected
idiomatically.

Global ActorId/SpotId, exact ActorRef/SpotRef, the
[User Spot](../../../01-glossary.en.md#entry-user-instance-spot) manager's
explicit create/get-or-create, and the actor-free Instance Spot
lifecycle also reuse the Java formal types. The Location provider
implements Java's opaque key/value atomic batch, and the Relocation
provider implements the immutable blob contract based on a
Framework-issued reference, unchanged. Kotlin only adds `send` and
`request` extensions to the ID-only direct call, and doesn't declare a
suspend `requestToSpot` that conflicts with the Java member. The exact
extension and Store type reuse are fixed by the
[Per-Feature Interfaces](interfaces/README.ko.md).

The shared JVM runtime implements placement and the activation barrier
using the Java binding's public raw socket API. It doesn't use the Core
service driver, a private binding entrypoint, or a separate Kotlin
runtime. A Ready-owner call resolves current
[authority](../../../01-glossary.en.md#authority) using the global ID,
and doesn't use a process-local handle or separate address.

The official Redis location extension's Kotlin call boundary and Java
type reuse rule are fixed by
[Location And Maintenance](interfaces/location-maintenance.ko.md).

## Cancellation Argument

A framework `CancellationToken` or a separate cancellation argument for
the same purpose isn't put on a Kotlin application callback or call
interface. A `suspend` function follows the lifecycle of the coroutine
that called it, and this behavior isn't duplicated with a separate token
parameter. Timeout, host shutdown, and resource cleanup follow each
feature's contract.

`ZLinkStoreCancellation` and `ZLinkRelocationCancellation`, reused from
the Java provider/adapter ABI, aren't Kotlin lifecycle tokens — they're
a fence for that SPI operation. This type isn't projected onto a Kotlin
suspending lifecycle callback.
