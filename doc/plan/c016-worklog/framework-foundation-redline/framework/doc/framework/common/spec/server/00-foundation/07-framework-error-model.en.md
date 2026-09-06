---
title: "Framework Error Model"
---

# Framework Error Model

[Foundation topic index](README.en.md) · [Spec index](../README.en.md) · [Previous: 06. Framework API](06-framework-api.en.md) · [Next: 08. Layering Boundaries And Identifiers](08-layering.en.md)

> Defines the shared `ErrorKind` that the Framework delivers to an Application when `Send`,
> `Request`, or a lifecycle operation fails, the completion/failure boundaries of Send and
> Request, and the rules for deciding whether to retry.

## 1. Scope

This document defines the shared error the Framework delivers to an Application when `Send`,
`Request`, or a lifecycle operation fails. An error represents a failure category an
Application must distinguish, not an internal function or state-machine step.

The Framework does not provide an indication of whether to retry alongside an error. An
Application checks the operation's completion condition, idempotency, and business state
before deciding whether to start a new operation.

## 2. The Shared `ErrorKind`

All five languages project the following names and numbers using that language's enum naming
convention. The value `0` is also a valid error value.

| Value | Kind | Meaning |
|---:|---|---|
| 0 | `NotFound` | The Actor, [Spot](02-glossary.en.md#spot) — a logical instance with an address and state — handler, route, or target doesn't exist. |
| 1 | `AlreadyExists` | The same identity or registration already exists. |
| 2 | `TypeMismatch` | The stable type differs from the requested Application type. |
| 3 | `NotConfigured` | The required role, handler, or Store isn't registered. |
| 4 | `Rejected` | A Framework admission, filter, or runtime policy rejected the operation without producing a typed result. |
| 5 | `Unavailable` | The target, route, Store, or worker is currently unavailable. |
| 6 | `CapacityExceeded` | Placement, a queue, or a bounded resource has no room. |
| 7 | [`DeadlineExceeded`](02-glossary.en.md#deadlineexceeded) | The operation did not complete within its set deadline. |
| 8 | `ShuttingDown` | The runtime isn't accepting a new operation. |
| 9 | `ProtocolError` | The wire, payload, or reply contract could not be processed. |
| 10 | `InvalidOperation` | The operation can't run in the current object, session, or runtime state. |
| 11 | `DataLost` | The published Relocation payload is missing or failed validation. |
| 12 | `InternalFailure` | A Framework failure that can't be expressed by the categories above. |

Generation, owner fence, moving phase, worker-queue state, and a Relocation processing step
are internal causes. When an Application doesn't need to choose a separate response, they are
recorded in logs and traces rather than exposed as a new public kind.

The five server packages and the HTTP client package share these 13 kinds. A per-language
interface document defines only the enum name and the exception/result representation — it
does not add a kind or a retry boolean.

## 3. Errors Checkable Before the Call

A problem that can be checked immediately at the call site — such as an invalid argument or an
already-closed handle — is delivered as that language's standard argument or
invalid-operation error. A startup configuration error is also delivered as a per-language
configuration exception. Neither of these is turned into a remote error reply.

A Framework failure discovered while waiting on outbound queue acceptance, route resolution,
or a remote reply is delivered as a per-language Framework exception or as a `result`'s
`ErrorKind`.

## 4. `Send` Completion and Failure

`Send` completes with no return value once the source runtime's outbound queue accepts the
message. This point does not mean the target handler has processed the message.

| Condition checked before completion | Result |
|---|---|
| The logical target or route doesn't exist (including [TargetNotFound](02-glossary.en.md#target-not-found)) | `NotFound` |
| The connection or current owner is currently unavailable (including [RouteNotConnected](02-glossary.en.md#route-not-connected)) | `Unavailable` |
| The outbound queue doesn't accept the message before the send timeout | `DeadlineExceeded` |
| The runtime has stopped new admission | `ShuttingDown` |

Even if target activation, admission, or handler execution fails after `Send` completes, that
failure does not change the result of the already-completed call. The Framework records it
in metrics, logs, and the message-flow trace, and does not automatically resubmit the same
message to a different target.

## 5. `Request` Completion and Failure

`Request` completes normally when it receives a typed reply. When a normal reply can't be
produced, it completes exactly once with one of the following `ErrorKind`s.

- `NotFound` if the target or handler doesn't exist.
- `Unavailable` if the route, connection, or current owner is unavailable.
- `DeadlineExceeded` if the reply isn't received within the deadline.
- `ProtocolError` if the wire, payload, or reply type can't be processed.
- `ShuttingDown` if the runtime is shutting down.
- `InternalFailure` for a Framework execution failure that can't be expressed by the kinds
  above.

<a id="bounded-queue-failure"></a>
`CapacityExceeded` and `Unavailable` both indicate a resource shortage, but they point to
different resources.

- **`CapacityExceeded` means the source couldn't secure a local bounded resource it owns.**
  That resource is a slot to hold the reply, an operation-table entry, or a
  Spot/Actor application or control queue within the same runtime — because the submitting side and the queue are in
  the same process, the source owns it.
- **`Unavailable` means the failure came from another node's queue being full.** A target's
  queue state is not expressed as `CapacityExceeded`. The line between the two kinds is
  whether this runtime owns the failed resource, and a caller uses this distinction to judge
  what to retry.
- **This distinction applies only to a queue.** When a target node's placement capacity is
  insufficient, that's an admission decision, not a queue, so `CapacityExceeded` is correct
  ([Spot Actor](../03-spot-actor/05-spot-actor-membership.en.md), [Spot address messaging](../03-spot-actor/06-spot-address-messaging.en.md)).
- **The [Message Follow](02-glossary.en.md#message-follow) relay queue — the mechanism that
  forwards a message that arrives at the previous owner node, on behalf of the new owner,
  after a relocation — and the relocation ingress hold have no record-count or
  byte bound defined by relocation itself.**
  - The amount retained in this queue or hold does
    not by itself produce `CapacityExceeded`.
  - The negotiated limit for one message and the
    limits set by transport, the deadline, and cancellation still apply.
  - Once retained work is
    admitted to an ordinary application execution lane, that lane's reservation applies, but
    this reservation is not used as a storage bound for the relay queue or hold.
  - If one of
    those limits causes a failure, the error follows the resource-owner rules above, based on
    which runtime owns the failed resource
    ([Spot Actor](../03-spot-actor/05-spot-actor-membership.en.md), [Location runtime](../05-location-relocation/01-location-runtime.en.md)).

Cancellation is delivered as that language's cancelled awaitable. `DeadlineExceeded` and
cancellation mean the caller has stopped waiting for the reply. They don't mean the remote
handler never ran, and a reply that arrives late does not produce a second result.

## 6. Typed Results and `Rejected`

For an operation whose contract has `Accepted` and `Rejected`, such as Actor create or join,
the Application callback's decision is returned as a typed result. In this case, `Rejected`
is not a Framework exception.

The shared `ErrorKind.Rejected` is used only when a filter, admission, or runtime policy
without a typed result rejects the operation. Not every result an Application's business
rule rejects is converted into a Framework exception.

## 7. Retry Judgment

A public exception, error object, or typed failure does not carry a retry hint such as
`RetryAdvice`, `isRetriable`, or `retriable`. Even for the same `ErrorKind`, the likelihood
that the operation already ran and the impact of a duplicate can differ.

To start a new operation, an Application directly checks the following.

1. Check the previous operation's completion condition and whether it could have run
   remotely.
2. Check whether the operation is idempotent, or whether an idempotency key prevents a
   duplicate effect.
3. Re-query business state if needed, then start the new operation.

Core-owned HWM retry within one binding operation is not an Application retry. The Framework
has no send-ready waiter and does not automatically resubmit the same operation to a
different logical target.

## 8. Application Job Queue Saturation

For the [Application job queue](02-glossary.en.md#application-job-queue) — the shared
supply-permit queue that a Framework host instance holds until an application callback
actually starts — a manual queue value outside `1..2,147,483,647`, or a calculation overflow,
is a configuration error before socket bind.

A runtime shared-cap shortage is a cancellable wait, not a public error, typed reject, or
drop reason.

Structural-limit failures of per-execution-object FIFOs are classified by resource ownership
in [§5](#bounded-queue-failure), separately from shared-cap waiting.
[Execution contract §7](../01-execution/02-handler-turn-and-execution-gate.en.md#execution-lanes) owns FIFO scope.

## 9. Verification Requirements

Verification uses only the public surface — each language's `ErrorKind` enum and its numeric
values, the return value/exception of `Send`/`Request`, and typed `Rejected` results. Each
item below leads to one contract test.

**`ErrorKind` values and count**

- Each language's 13 `ErrorKind`s and numbers match.

**`Send` completion boundary**

- `Send` completes on source outbound queue acceptance, and a later remote failure doesn't
  change the result.

**`Request` completion boundary**

- `Request` doesn't produce a second result from a reply that arrives late after a timeout or
  cancellation.

**Bounded queue errors**

- A Request that cannot secure a slot in the source runtime's bounded application/control queue receives `CapacityExceeded`.
- A Request rejected because another node's bounded application/control queue is full receives `Unavailable`.
- Insufficient target placement capacity is observed as `CapacityExceeded`, distinct from remote queue saturation.

**Typed `Rejected` distinction**

- A typed `Rejected` result is distinguished from an `ErrorKind.Rejected` exception.

**Absence of a retry hint**

- The public error surface (exception, error object, typed failure) carries no retry hint.

---

[Foundation topic index](README.en.md) · [Spec index](../README.en.md) · [Previous: 06. Framework API](06-framework-api.en.md) · [Next: 08. Layering Boundaries And Identifiers](08-layering.en.md)
