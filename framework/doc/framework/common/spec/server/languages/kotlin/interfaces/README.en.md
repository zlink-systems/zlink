# Kotlin Public Interface Formal Contract

[Kotlin contract table of contents](../README.en.md) · [Java Interface](../../java/interfaces/README.en.md)

The Kotlin package shares the JVM service runtime with Java. The
documents below fix, per feature, the scope where Java types are used
as-is and the Kotlin-only coroutine/DSL signatures.

- [Common Runtime](common-runtime.en.md)
- [Configuration And Host](configuration-host.en.md)
- [Channel Messaging](channel-messaging.en.md)
- [Spot](spots.en.md)
- [Actor](actors.en.md)
- [STREAM Session](stream-session.en.md)
- [Location And Maintenance](location-maintenance.en.md)
- [Monitoring](monitoring.en.md)

## Public API Structure

A Kotlin application directly uses Java's lifecycle, termination,
factory relocation builder, and Location types. The Kotlin package
provides coroutine handlers, suspending calls, reified registration, and
a configuration DSL, and doesn't duplicate a runtime facade or state
type of the same meaning.

The Actor/Spot state-preservation adapter is also formally Java's
`ZLinkActorRelocationAdapter` and `ZLinkSpotRelocationAdapter`. Kotlin
projects `byte[]` as `ByteArray` and uses `CompletionStage` completion
unchanged, without defining a separate state DTO, state contract ID,
suspending adapter, or reified state-preservation policy. The Entry
Spot's coroutine lifecycle class doesn't add an infrastructure
membership relocation callback. Only the `SpotWide`
application-signaled boundary's completion is provided as an
`onRelocationReadyCompletedSuspending(...)` default no-op bridge.

A Channel extension only takes a process-local ChannelName, and an
optional overload that takes MeshName and
[ChannelName](../../../../01-glossary.en.md#channelname) together isn't
added. Host `Relocate`/`Shutdown` uses Java's relocation mode/options/
result type unchanged, and doesn't provide a separate drain facade. The
Location Store's opaque key/value atomic batch and the Relocation
Store's immutable blob contract based on a Framework-issued reference
are also formally the Java public interface.

Each feature document distinguishes the Kotlin source signature from the
generated JVM signature the application actually links against. Default
argument, suspend continuation, extension receiver, and generic bound
must correspond without loss between the two representations. The first
`String` argument of an extension that directly specifies a node is
[MeshName](../../../../01-glossary.en.md#meshname), same as the Java
contract.

Public generation, revision, epoch, and sequence ordinal use the Java
contract's positive `Long` range unchanged. The valid range is
`1..Long.MAX_VALUE`, and at the maximum value it's treated as terminal
exhaustion, without wrap or value reuse. `0` is only used when the
relevant contract explicitly specifies it to represent a not-yet-determined
value.

## RouteMesh Object Runtime Baseline

The Kotlin exact interface uses the same global ActorId/SpotId,
immutable `ActorRef`/`SpotRef`, ID-only regular messaging, and
exact-ref mutation/session bind as Java. Actor and User Spot's
create/get-or-create are single-use fluent operations. The
[Spot](../../../../01-glossary.en.md#spot) manager is User-Spot-only and
doesn't provide an Instance Spot creation member. Cold activation of a
Missing [Instance Spot](../../../../01-glossary.en.md#entry-user-instance-spot)
only starts when `instanceSpot()` or `instanceSpot(stableType)` is
specified on the Spot-dedicated send/request call. Without the marker,
it's not-found, and cold activation using only the marker auto-selects
the type only when the selected Mesh has one distinct serving Instance
type. Existing [authority](../../../../01-glossary.en.md#authority) uses
the stored type regardless of the number of registered types. Mesh
object role is distinguished as None, Client, Server. Every server
factory configure callback selects exactly one relocation behavior. A
Kotlin extension doesn't abbreviate this contract or add a local
fallback.

The global ref's JSON fields are `actorId` or `spotId`,
`objectGeneration`, `meshName`, `nodeRid`. `objectGeneration` is a
decimal string, and an unknown field, duplicate field, missing required
field, or a value that's 0 or exceeds `Long.MAX_VALUE` is rejected.
String identity keeps the exact value and isn't normalized. The
following two shapes are allowed. `objectGeneration` is `"1"`..
`"9223372036854775807"` with no leading zero, and a JSON number token is
rejected.

```json
{"actorId":"actor-7","objectGeneration":"41","meshName":"game","nodeRid":"game-0123456789abcdef0123456789abcdef"}
```

```json
{"spotId":"room-7","objectGeneration":"42","meshName":"game","nodeRid":"game-0123456789abcdef0123456789abcdef"}
```
