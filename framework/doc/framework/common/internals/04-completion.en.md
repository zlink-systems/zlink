---
title: "4. Operation Completion Confirmation — Only One Finalizes"
---

# 4. Operation Completion Confirmation — Only One Finalizes

[Internal structure table of contents](README.en.md) · [Previous: 3. Application And Infrastructure Execution Separation](03-progress-isolation.en.md) · [Next: 5. Message Continuity During A Move](05-relocation-continuity.en.md)

> **What this chapter answers** — when a response, timeout,
> cancellation, shutdown, and disconnect all arrive at once, what
> finalizes the caller.
>
> **Contract ownership** — the error kind is owned by
> [the Framework Error Model](../spec/32-framework-error-model.en.md),
> and the ban on resending after acceptance by
> [Transport Liveness](../spec/29-transport-liveness.en.md). This
> chapter covers the **structure** that satisfies that contract and
> the failures that become visible during completion races.

For one call waiting on a response, a response, a timeout, a
cancellation, a shutdown, and a disconnect can all arrive **at the same
time.** This document covers the structure that lets only one of them
finalize the caller, and how not to lose a response along the way.

## 1. The Core Decision — Only The Path That Claims The Completion Slot First Wins

Each call has one completion slot, and several paths compete for it.
Only the path that claims it releases the caller's wait. The losing
paths do nothing.

```mermaid
flowchart LR
    R["response arrives"] --> S["completion slot<br/>(one)"]
    T["timeout"] --> S
    C["cancellation"] --> S
    D["shutdown"] --> S
    X["disconnect"] --> S
    S --> W["only the path that claimed it<br/>finalizes the caller"]
```

<a id="the-approach-the-implementations-converged-on"></a>
### Completion Authority Confirmation

**Decision — atomically taking an entry from the in-progress call table is the
completion contention point.** The response, timeout, cancellation, and shutdown paths
all try to take the same entry. Only the successful path gains completion authority; the
others observe that the call is already complete and stop.

This operation confirms completion authority and removes the in-progress call together.
It therefore needs neither a separate completion marker nor a second slot reservation.
Every completion path uses the same approach so that adding a path cannot introduce a
different contention rule.

<a id="2-dont-call-the-handler-from-where-completion-was-confirmed"></a>
## 2. Completion Callback Execution Turn

If an application callback runs inside the lock held while confirming
completion, the callback calling back into the runtime requires the
same lock, causing a deadlock. Timer cancellation and payload cleanup
are also done outside it.

Releasing the lock and immediately invoking the callback on the same
call stack is still insufficient. It lets application code re-enter the
runtime before the current transport response or timeout handling has
returned. The callback is placed on a process-shared completion dispatcher
and runs on a new execution turn after the current handling returns.

The order is — **confirm completion authority → release the lock → enqueue
the callback on the dispatcher → run the callback on a new execution turn.**

If the terminal winner takes the in-progress table entry and dispatcher admission then
fails, the application completion is lost. The runtime therefore reserves a completion
dispatcher slot when it accepts the operation. That reservation remains until the callback
returns. The combined number of in-progress operations and callbacks waiting or running on
the dispatcher cannot exceed 4,096, so the callback queue cannot grow without a bound.

If no slot can be reserved, the operation is rejected with `CapacityExceeded` before the
request is sent. Once an operation is accepted, completion enqueue has no reject or drop
path. The dispatcher uses a process-shared lane instead of creating a thread per callback,
and shutdown drains every accepted callback. An exception from one callback does not stop
later callbacks from running.

## 3. Separate Operation Identity From The Reply Route

A service-wire request preserves two different values. Both are Framework-internal values
and are not exposed to the application.

| Value | Form | Responsibility |
|---|---|---|
| `OperationId` | `{ high: u64, low: u64 }` | Terminal-deduplication identity for one operation. It stays unchanged through relocation and reply relay |
| `ReplyRouteId` | non-zero `u64` | Connects a terminal reply to a pending entry within the source lifecycle. It does not replace operation identity |

An operation that expects a terminal result cannot have an `OperationId` with both words
zero. Registries and durable completion records preserve both words. Using only the `low`
word as a key can make two different operations appear to be the same entry. A
`ReplyRouteId` is also unique among requests pending in one source-owner lifecycle, but it
does not by itself decide terminal deduplication after relocation.

The sending runtime first creates `OperationId` and, when a reply route is required,
`ReplyRouteId`. It then registers a pending completion entry keyed by the full `OperationId`,
reserves a dispatcher slot, and fixes the reply route addressed by `ReplyRouteId`. Only then
does it submit to transport. The wire request preserves the two values in separate fields;
neither value is used as an alias for the other.

```mermaid
sequenceDiagram
    participant S as Source runtime
    participant P as Completion and reply-route registry
    participant T as Transport
    S->>S: create OperationId and ReplyRouteId
    S->>P: register full OperationId, reply route, and dispatcher slot
    S->>T: submit the registered request
    T-->>P: terminal reply arrives
    P->>P: atomically take the exact entry
    P-->>S: deliver completion in a new execution turn
```

With this order, even an immediate in-process response cannot be processed before
registration. No separate early-response map, or race-handling that cross-checks such a map
with the pending table, is needed.

## 4. Don't Resend After Acceptance

Once transport has accepted a message, **whether the target has
executed it is unknowable.** Resending to a different target in this
state can cause double execution.

**Decision — the runtime never automatically resends after
acceptance.** This holds even if the connection drops
([Transport Liveness 「5. Ready And Failure Determination」](../spec/29-transport-liveness.en.md#5-ready-and-failure-determination)).
The application can start a new call, and at that point the risk of
duplicate execution is judged by the application.

This rule requires distinguishing "failure after sending" from
"failure before sending."

| Failure timing | May it be resent |
|---|---|
| Before transport accepts | Yes. It's certain the target never received it |
| After transport accepts | **No.** Whether it executed is unknowable |

## 5. The Completion Point Of A Call That Doesn't Wait For A Response

A call that doesn't wait for a response completes normally **at the
moment this process's send path accepts the message.** Whether the
remote queue received it or the handler executed it can't be known
from this result
([Framework API 「12. Spot, Actor, And STREAM Owner」](../spec/06-framework-api.en.md#12-spot-actor-and-stream-owner)).

"Local acceptance" and "transport acceptance" are not separate events.
In this product the send path is the socket's send queue, so both terms
refer to the same completion boundary. Documentation and code comments
use the single term send acceptance.

## 6. Don't Classify Failure By String

The completion path must distinguish cancellation, timeout, and
shutdown. This distinction decides the result the caller receives.

Judging cancellation by **running a regex against the error message
string** makes classification change silently when the language or
library changes the wording. Conversely, a business error whose message
contains "cancel" is misclassified as cancellation and swallowed.

**Decision — classify failure by type or a dedicated value.** The
message string is for humans to read, not a branch condition.

## 7. Result To Confirm

- Even if a response, timeout, cancellation, and shutdown occur at the
  same time, the caller completes exactly once.
- A late-arriving response doesn't finalize the caller again.
- The completion callback runs on a new execution turn outside both the
  confirmation lock and the current transport call stack.
- A dispatcher slot is reserved before operation acceptance, and an
  accepted completion enqueue is neither rejected nor dropped. The
  combined number of in-progress operations and waiting/running
  callbacks doesn't exceed 4,096.
- The dispatcher is a process-shared lane that doesn't create a thread
  per callback, and shutdown drains every accepted callback.
- The completion table keys by both `u64` words of the wire `OperationId`, while
  `ReplyRouteId` remains a separate reply-route identity.
- The full operation identity, reply route, and dispatcher slot are registered before the
  request is submitted.
- Even if the connection drops after transport accepted, the runtime
  doesn't resend to a different target.
- There's one completion-confirmation approach within the runtime.
- Cancellation/timeout/shutdown classification doesn't depend on the
  error message string.

---

[Internal structure table of contents](README.en.md) · [Previous: 3. Application And Infrastructure Execution Separation](03-progress-isolation.en.md) · [Next: 5. Message Continuity During A Move](05-relocation-continuity.en.md)
