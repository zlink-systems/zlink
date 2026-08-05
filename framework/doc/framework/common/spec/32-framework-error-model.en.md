---
title: "Framework Error Model"
---

# Framework Error Model

[Spec index](README.ko.md) · [Previous: Failure handling and failover scope](31-failure-failover-policy.ko.md)

> **What this chapter defines** — the shared error delivered to an
> Application when `Send`, `Request`, or a lifecycle operation fails.

## 1. Scope

This document defines the shared error the Framework delivers to an
Application when `Send`, `Request`, or a lifecycle operation fails. An
error represents a failure category an Application must distinguish, not
an internal function or state-machine step.

The Framework does not provide a retry-or-not signal alongside an error.
An Application checks the operation's completion condition, idempotency,
and business state before deciding whether to start a new operation.

## 2. The Shared `ErrorKind`

All five languages project the following names and numbers using that
language's enum naming convention. The value `0` is also a valid error
value.

| Value | Kind | Meaning |
|---:|---|---|
| 0 | `NotFound` | The Actor, Spot, handler, route, or target doesn't exist. |
| 1 | `AlreadyExists` | The same identity or registration already exists. |
| 2 | `TypeMismatch` | The stable type differs from the requested Application type. |
| 3 | `NotConfigured` | The required role, handler, or Store isn't registered. |
| 4 | `Rejected` | A Framework admission, filter, or runtime policy without a typed result rejected the operation. |
| 5 | `Unavailable` | The target, route, Store, or worker is currently unavailable. |
| 6 | `CapacityExceeded` | Placement, a queue, or a bounded resource has no room. |
| 7 | `DeadlineExceeded` | The operation did not complete within its set deadline. |
| 8 | `ShuttingDown` | The runtime isn't accepting a new operation. |
| 9 | `ProtocolError` | The wire, payload, or reply contract could not be processed. |
| 10 | `InvalidOperation` | The operation can't run in the current object, session, or runtime state. |
| 11 | `DataLost` | The published Relocation payload is missing or failed validation. |
| 12 | `InternalFailure` | A Framework failure that can't be expressed by the categories above. |

Generation, owner fence, moving phase, worker-queue state, and a
Relocation processing step are internal causes. When an Application
doesn't need to choose a separate response, they are recorded in logs and
traces rather than exposed as a new public kind.

## 3. Errors Checkable Before The Call

A problem checkable right at the call site — an invalid argument, an
already-closed handle — is delivered as that language's standard
argument or invalid-operation error. A startup configuration error is
also delivered as a per-language configuration exception. Neither of
these is turned into a remote error reply.

A Framework failure discovered while waiting on outbound queue
acceptance, route resolution, or a remote reply is delivered as a
per-language Framework exception, or as a `result`'s `ErrorKind`.

## 4. `Send` Completion And Failure

`Send` completes with no return value once the source runtime's outbound
queue accepts the message. This point does not mean the target handler
has processed the message.

| Condition checked before completion | Result |
|---|---|
| The logical target or route doesn't exist | `NotFound` |
| The connection or current owner is currently unavailable | `Unavailable` |
| The outbound queue doesn't accept the message before the send timeout | `DeadlineExceeded` |
| The runtime has stopped new admission | `ShuttingDown` |

Even if target activation, admission, or handler execution fails after
`Send` completes, it does not change the result of the already-completed
call. The Framework records this failure in metrics, logs, and the
message-flow trace, and does not automatically resubmit the same message
to a different target.

## 5. `Request` Completion And Failure

`Request` completes normally when it receives a typed reply. When a
normal reply can't be produced, it completes exactly once with one of the
following `ErrorKind`s.

- `NotFound` if the target or handler doesn't exist.
- `Unavailable` if the route, connection, or current owner is
  unavailable.
- `DeadlineExceeded` if the reply isn't received within the deadline.
- `ProtocolError` if the wire, payload, or reply type can't be
  processed.
- `CapacityExceeded` if the source runtime can't secure the local bounded
  resource this operation needs. The target here is a resource the
  source owns — a slot to hold the reply, an operation-table entry, and
  so on. **A Spot/Actor queue within the same runtime also falls under
  this** — because the submitting side and the queue are in the same
  process, it's a resource the source owns.
- By contrast, **a failure because another node's queue is full is
  `Unavailable`.** A target's queue state is not expressed as
  `CapacityExceeded`. The line between the two kinds is "does this
  runtime own the failed resource," and a caller uses this distinction
  to judge what to retry.
- This distinction applies **only to a queue**. When a target node's
  placement capacity is insufficient, that's an admission decision, not
  a queue, so `CapacityExceeded` is correct
  ([Spot Actor](15-spot-actor.ko.md),
  [Spot address messaging](16-spot-address-messaging.ko.md)).
- **The Message Follow relay queue is an exception, and is
  `CapacityExceeded`.** This queue is physically on the previous owner
  node, but the runtime responsible for the relay owns it as its own
  resource, and its bound is a fixed value the contract sets (1024
  messages, 16 MiB). To the caller, this means not "the other node
  can't receive" but "the fixed capacity of the relocation path was
  exceeded," so the retry judgment differs
  ([Spot Actor](15-spot-actor.ko.md),
  [Location runtime](21-location-runtime.ko.md)). This kind **applies
  only to `Request`** — the result of an already-completed one-way call
  never changes because of a relay failure (§4), and exceeding the relay
  bound is recorded only in metrics/logs/traces.
- `ShuttingDown` if the runtime is shutting down.
- `InternalFailure` for a Framework execution failure that can't be
  expressed by the kinds above.

Cancellation is delivered as that language's cancelled awaitable.
`DeadlineExceeded` and cancellation mean the caller has stopped waiting
for the reply. They don't mean the remote handler never ran, and a
reply that arrives late does not produce a second result.

## 6. Typed Results And `Rejected`

For an operation whose contract has `Accepted` and `Rejected`, such as
Actor create or join, the Application callback's decision is returned as
a typed result. In this case, `Rejected` is not a Framework exception.

The shared `ErrorKind.Rejected` is used only when a filter, admission, or
runtime policy without a typed result rejects the operation. Not every
result an Application's business rule rejects is converted into a
Framework exception.

## 7. Retry Judgment

A public exception, error object, or typed failure does not carry a
retry hint such as `RetryAdvice`, `isRetriable`, or `retriable`. Even for
the same `ErrorKind`, the likelihood that the operation already ran and
the impact of a duplicate can differ.

To start a new operation, an Application directly checks the following.

1. Check the previous operation's completion condition and whether it
   could have run remotely.
2. Check whether the operation is idempotent, or whether an idempotency
   key prevents a duplicate effect.
3. Re-query business state if needed, then start the new operation.

A send-ready wait inside the Framework, a Store result re-check, and
target reselection before acceptance are not Application retries. Once
an operation has been accepted, or it has become unknowable whether it
was accepted, the Framework does not automatically resubmit the same
operation to a different logical target.

## 8. Per-Language Projection And Verification

The five server packages and the HTTP client package all use the same 13
kinds. A per-language interface document defines only the enum name and
the exception/result representation — it does not add a kind or a retry
boolean.

Contract tests and E2E verify the following.

- Each language's 13 kinds and numbers match.
- `Send` completes on source outbound queue acceptance, and a later
  remote failure doesn't change the result.
- A late reply after a `Request` timeout or cancellation doesn't produce
  a second result.
- A typed `Rejected` result is distinguished from an `ErrorKind.Rejected`
  exception.
- The public error surface carries no retry hint.
