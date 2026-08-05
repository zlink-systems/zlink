# ZLink Framework Java Public Contract

This directory owns the **formal public contract** the Java framework
must provide. The implementation and regression tests must follow this
contract.

Where Kotlin uses the Java contract as-is, this document is followed;
the Kotlin-only `suspend` and `Flow` surface is fixed separately by the
[Kotlin Public Contract](../kotlin/README.ko.md).

A Channel call only uses a process-local ChannelName. `Relocate`, which
specifies mode and target application version, is the authority for
object relocation, and `Shutdown` for host termination. Planned
maintenance only moves to the same version as source, and rolling
update only to a higher version the caller specifies. The Location
provider provides an atomic storage primitive for opaque records, and
the Relocation provider stores an immutable blob at a Framework-issued
reference.

| Document | Scope |
|---|---|
| [Per-Feature Interfaces](interfaces/README.ko.md) | Exact signature for runtime, configuration, Channel, Spot, Actor, STREAM, Location/maintenance, and monitoring |
| [Stream Connector](../../../stream-connector/languages/java/03-stream-connector.en.md) | The client connector's public surface |

**The meaning and behavioral rules of a feature are owned by the
[common spec](../../../README.en.md).** This directory only fixes the
**exact public API** that meaning takes in this language.

## Cancellation Representation

A generic Framework token imitating .NET's `CancellationToken` isn't
added to Java lifecycle callbacks and host operations. `CompletionStage`
waiter cancellation doesn't interrupt an already-started shared
operation, and [Spot](../../../01-glossary.en.md#spot) closing is
bounded by having the framework end the stage-completion wait at the
context's absolute deadline.

`ZLinkRelocationCancellation`, `ZLinkStoreCancellation`, and
`ZLinkWorkerCancellation` aren't generic lifecycle tokens. Each is an
SPI-only type that only expresses blocking a stale relocation attempt's
adapter completion, operation cancellation for provider I/O, and
stopping CPU/I/O worker execution, respectively. These types aren't
reused for handler, Spot lifecycle, host termination, or the regular
message API.
