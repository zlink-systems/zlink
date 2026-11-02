# ZLink Framework Java Public Contract

<!-- framework-adapter-nav:start -->
[Language interface table of contents](../README.en.md) | [Spec table of contents](../../README.en.md)
<!-- framework-adapter-nav:end -->
This directory owns the **formal public contract** the Java framework
must provide. The implementation and regression tests must follow this
contract.

Where Kotlin uses the Java contract as-is, this document is followed;
the Kotlin-only `suspend` and `Flow` surface is fixed separately by the
[Kotlin Public Contract](../kotlin/README.en.md).

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
| [Per-Feature Interfaces](interfaces/README.en.md) | signature for runtime, configuration, Channel, Spot, Actor, STREAM, Location/maintenance, and monitoring |
| [Stream Connector](../../../stream-connector/languages/java/03-stream-connector.en.md) | The client connector's public surface |

**The meaning and behavioral rules of a feature are owned by the
[common spec](../../README.en.md).** This directory only fixes the
**per-language public API** that meaning takes in this language.

## Cancellation Representation

A generic Framework token imitating .NET's `CancellationToken` isn't
added to Java lifecycle callbacks and host operations. `CompletionStage`
waiter cancellation doesn't interrupt an already-started shared
operation, and [Spot](../../00-foundation/02-glossary.en.md#spot) closing is
bounded by having the framework end the stage-completion wait at the
context's absolute deadline.

`ZLinkRelocationCancellation`, `ZLinkStoreCancellation`, and
`ZLinkWorkerCancellation` aren't generic lifecycle tokens. Each is an
SPI-only type that only expresses blocking a stale relocation attempt's
adapter completion, operation cancellation for provider I/O, and
stopping CPU/I/O worker execution, respectively. These types aren't
reused for handler, Spot lifecycle, host termination, or the regular
message API.

## Messaging Hot Path Transition Count Measurement

[Messaging hot path §3](../../01-execution/08-messaging-hot-path.en.md) limits Framework-created execution-resource transitions to one from binding completion notification to the first instruction of the caller continuation. The unit test counts thread-identity changes between the binding callback and continuation, and uses the completion-dispatcher turn marker to verify that Framework delivery ran on its worker. Task posting and wakeups are not execution-resource transitions.

| Segment | Limit | Measurement point | Test identifier |
|---|---:|---|---|
| Binding completion notification → caller continuation's first instruction | At most 1 | Test-only identity comparison [ZLinkOutsideLaneResumptionTest.java:48](../../../../../../../languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/execution/ZLinkOutsideLaneResumptionTest.java#L48); [ZLinkServiceCompletionDispatcher.java:53](../../../../../../../languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java#L53) | `unit:ZLinkOutsideLaneResumptionTest.messagingHotPathCompletionTransitionCountIsOne` (expected 1) |

---
<!-- framework-adapter-nav:bottom:start -->
[Language interface table of contents](../README.en.md) | [Spec table of contents](../../README.en.md)
<!-- framework-adapter-nav:bottom:end -->
