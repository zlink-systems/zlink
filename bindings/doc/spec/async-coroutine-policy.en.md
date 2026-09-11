---
title: "Bindings Send and Async Completion Surface Policy"
---

<!-- bindings-nav:start -->
[Spec index](README.en.md) | [Previous: Overview](README.en.md) | [Next: C](c/README.en.md)
<!-- bindings-nav:end -->

# Bindings Send and Async Completion Surface Policy

> This document defines how first-party bindings other than C start and finish send, request, publish,
> and reply operations. A binding pulls native completions internally and converts them into
> language-specific awaitables or blocking results. The [C binding specification](c/README.en.md)
> owns the raw completion contract for C.

## 1. Operations and completion boundaries

Send and request can wait for local send queue admission. A high-level binding uses Core `NONE` for a
blocking terminal and Core `DONTWAIT` for an awaitable terminal. Go exposes one public
`Submit(context.Context)` terminal, submits with Core `DONTWAIT`, and returns a result object
immediately; the internal completion is awaited by the object's `Admitted(ctx)`/`Reply(ctx)`.

| Operation | Public completion boundary |
|---|---|
| Send | The admission outcome in [Submit result projection](README.en.md#submit-result-projection). |
| Request | Both blocking and awaitable terminals wait for the reply, timeout, or terminal request error. |
| Publish | Uses lossy/NODROP flags and has a synchronous submit result. |
| Reply | Finishes with synchronous `NONE` admission subject to socket `SNDTIMEO`. |

Per-send-operation timeouts and high-level send/request flags are not part of the public contract. The
request reply timeout remains on the builder. Publish in Go and Python uses a separate `PublishOp`
with publish flags and synchronous submit semantics.

## 2. Builders and operation start

A socket captures its target when it creates an operation. Routed and non-routed sends use one send
operation family per language. `Received.send()/Send()` returns a send builder that captures the source
target. `Received.reply()/Reply()` returns a reply builder that captures the source routing ID and
`ReplyToken`. Requesting a reply builder from a DATA envelope without reply information fails with the
language's invalid-state error.

| Binding | PAIR | DEALER | ROUTER | STREAM |
|---|---|---|---|---|
| C++ | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| .NET | `Send()` | `Send()`, `Request()` | `Send(rid)`, `Request(rid)`, `Reply(rid, token)` | `Send(rid)` |
| Java/Kotlin | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Node | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Python | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |
| Go | `Send()` | `Send()`, `Request()` | `SendTo(rid)`, `Request(rid)`, `Reply(rid, token)` | `SendTo(rid)` |
| Rust | `send()` | `send()`, `request()` | `send(rid)`, `request(rid)`, `reply(rid, token)` | `send(rid)` |

A builder collects the payload one part at a time and can be submitted only once. High-level bindings
preserve their existing language-specific message ownership. A binding that restores an lvalue or
managed message after submit failure restores it from staging; an rvalue or move input is consumed.
This behavior is not a retransmission queue.

## 3. Awaitable completion

Submit results follow the [common result projection](README.en.md#submit-result-projection).
The [async execution model](async-execution-model.en.md#5-joining-submit-results-and-completions)
owns completion joins, context lifetime, and exactly-once cleanup;
[caller wait cancellation](async-execution-model.en.md#6-caller-wait-cancellation)
owns language wait cancellation and typed request errors.

## 4. PollCompletion and pull events

The [async execution model](async-execution-model.en.md#4-pollers-and-completion-drain)
defines raw readiness versus high-level progress, drain through `NO_DATA`, and owner transfer to a public poller.

## 5. ReplyToken and reply

Only a ROUTER REQUEST receive creates a valid `ReplyToken`. A token carries both the responder socket
instance and an opaque value; equality and hashing compare both. It provides no public constructor,
parse operation, raw numeric conversion, ordering, serialization, or close operation. The same raw value
from different responder sockets is not equal.

If a language cannot prevent construction of a default or zero value, that value is invalid. When the
token owner differs from the receiver socket, an explicit ROUTER reply fails with the language's
invalid-argument error before the native call. In C++ and Rust, the token also holds a shared owner tag
created by the ROUTER wrapper, so a reused wrapper address cannot make tokens from different sockets
equal.

Reply provides exactly one flag-free synchronous terminal in every high-level binding. The C++, Go,
Node, Python, and Rust reply builders also accept no flags.

## 6. Per-language terminal interfaces

The following declarations summarize the complete signatures in each language README. Each README owns
its language-specific overloads, visibility, and ownership.

Async terminals return a **submission result object** captured at submit time. `SendSubmission` carries
`result` (a submit-time snapshot, `OK`|`BACKPRESSURED`) and the admission stage; `RequestSubmission` adds
the reply stage. When `result` is `OK` the admission is already complete (SEND ends there; REQUEST's reply
completes from the completion queue); when `BACKPRESSURED` the binding retains the input and completes
admission via WRITABLE resubmission. Other submit failures (`NOT_CONNECTED`, `NOT_FOUND`, `NOT_ADMITTED`,
`INVALID_ARGUMENT`, `TERMINATED`, `OUT_OF_MEMORY`, `INTERNAL_ERROR`, …) are raised as exceptions/errors, not
through the result object (the caller does not watch two places). The object and field names
(`Submission`, `result`, `admitted`, `reply`) are shared across the seven languages. The structure and join
rules belong to [the common result projection](README.en.md#submit-result-projection) and
[async execution model §5](async-execution-model.en.md#5-joining-submit-results-and-completions). Synchronous
terminals (`submit_sync()`, .NET/C++ `Submit()`/`submit()`) are unchanged.

<a id="submission-stage-isolation"></a>

**The state of stages returned by result objects is isolated between distinct submissions.** If a caller completes, fails, cancels, or overwrites the completion state of one submission's `admitted` or `reply`, or a view obtained through a public conversion method of that stage, the action must not propagate to another submission's completion state or observed result through a shared returned representation or shared state behind it. Consuming a stage or detaching a wait must not change another submission's waiter state or ability to consume its result through shared state. This rule covers previously returned results, in-flight submissions, and later submissions on both the same socket and other sockets. Normal resource reclamation and the resulting progress of other submissions follow the existing admission and lifecycle contracts.

The actions covered here are completion-state operations offered by the returned type and ordinary awaiting, consumption, dropping, destruction, and wait cancellation. Arbitrary property or prototype replacement, private-state access, and payload mutation are outside this clause. This clause does not require new cancellation or forced-completion APIs. Socket/context shutdown, a cancellation source explicitly connected to multiple waits, and completion dependencies explicitly established by the application follow their respective existing contracts.

Isolation does not mean cancellation of the Core operation or reversal of admission that has already occurred. Actual admission and reply completion, including the relationship between the two stages of one REQUEST, follow [async execution model §5](async-execution-model.en.md#5-joining-submit-results-and-completions). Caller-wait cancellation and native-state cleanup follow [§6 of that document](async-execution-model.en.md#6-caller-wait-cancellation). A value forcibly assigned to a view by the caller is not evidence of actual admission or a reply; this clause defines no new propagation rule between stages for that action.

An instance may be shared to represent already-completed admission, provided the actions above cannot affect another submission through that shared representation. This condition covers waiter and single-consumption state as well as the completed value. Per-submission objects, completion representations that cannot be changed, and views with isolated mutations can all satisfy the condition. Shared ownership of internal state within the same operation is allowed.

| Binding | Safe example for already-successful admission | Condition |
|---|---|---|
| Java/Kotlin | Shared `CompletableFuture.completedStage(null)`, or a new `CompletableFuture.completedFuture(null)` per submission | Mutating a `toCompletableFuture()` view of the shared minimal stage does not affect another submission. Directly sharing a mutable `completedFuture(null)` instance across submissions allows operations such as `obtrudeException()` to change other results. Calling only `cancel()` after successful completion does not detect that defect. |
| Node.js | The result of `Promise.resolve()`, without exposing its resolver | A Promise's completion state cannot change once settled. The returned Promise has no caller-facing settle/cancel API. |
| .NET | `Task.CompletedTask`, or a completed Task specific to the submission | The result object exposes the Task, not the source that completes it. |
| Python | An `asyncio.Future` created for the submission on its event loop and completed successfully | Do not share another submission's pending or cancelled Future, or a coroutine object that cannot be reused. |
| Go | `Admitted(ctx)` that returns `nil` immediately while keeping completion state private | The result object exposes neither a completion channel nor a completion-state setter. |
| Rust | `Box::pin(std::future::ready(Ok(())))` created per submission | Do not share mutable Future consumption state across submissions. Move ownership alone does not establish the absence of internal sharing. |
| C++ | A move-only `async_result_t<void>` with completion and consumption state specific to the submission | Internal `shared_ptr` use is allowed. Consuming or destroying one result must not change another submission's completion or consumption state. |

**Verification requirement.** Pin the following with regression tests using public result objects and the language's completion types.

- Cover `OK` admission, `BACKPRESSURED` admission, and REQUEST `reply` separately. Exercise completion, failure, cancellation, and overwrite operations where the type offers them and the state permits them. Distinguish successful state changes from rejected or ineffective attempts. A test of an already-successful mutable future must not rely only on a no-op `cancel()`.
- After modifying, consuming, or releasing one submission's result, verify that both another result already returned on the same socket and a later submission receive their own completion outcomes and REQUEST replies. Also check results from another socket to cover completion representations shared across sockets. Use independent cancellation sources for independent waits.
- If forced completion or cancellation is unavailable, verify that the public type exposes no such authority, and use the awaiting, single consumption, dropping, destruction, or wait cancellation that the type does support to check isolation from other submissions. A comment recording the missing operation does not replace completion verification. Do not require a single-consumer stage to support consumption twice.
- When a pending wait is cancelled or detached, verify that late completion neither completes the cancelled wait again nor prevents other submissions from completing. Verification of actual admission/reply ordering and native cleanup follows [the async execution model's verification requirements](async-execution-model.en.md#7-implementation-and-contract-test-verification-requirements).

| Binding | Send terminal | Request terminal | Reply terminal |
|---|---|---|---|
| C++ | `void submit() &&`, `send_submission_t async() &&` | `vector<message_t> submit() &&`, `request_submission_t async() &&` | `void submit() &&` |
| .NET | `void Submit()`, `SendSubmission Async(CancellationToken)` | `IReadOnlyList<Message> Submit()`, `RequestSubmission Async(CancellationToken)` | `void Submit()` |
| Java/Kotlin | `SendSubmission submit()`, `void submit_sync()` | `RequestSubmission submit()`, `List<Message> submit_sync()` | `void submit()` |
| Node | `SendSubmission submit()`, `void submit_sync()` | `RequestSubmission submit()`, `Message[] submit_sync()` | `void submit()` |
| Python | `SendSubmission submit()`, `None submit_sync()` | `RequestSubmission submit()`, `list[Message] submit_sync()` | `None submit()` |
| Go | `Submit(context.Context) (SendSubmission, error)` | `Submit(context.Context) (RequestSubmission, error)` | `Submit(context.Context) error` |
| Rust | `Result<SendSubmission, SubmitError> submit()`, `Result<(), SubmitError> submit_sync()` | `Result<RequestSubmission, ZlinkError> submit()`, `Result<Vec<Message>, ZlinkError> submit_sync()` | `Result<(), SubmitError> submit()` |

The Kotlin suspend surface maps to extensions that `await()` `admitted()`/`reply()`. In Go the waiting is
done by the object's `Result()`/`Admitted(ctx)`/`Reply(ctx)` methods (the `Submit(ctx)` in the Go row only
submits and returns immediately).

Go and Python publish use the following separate operation families.

```text
Go:     PublishOp -> PublishSubmitOp.Flags(SendFlags).Submit(context.Context) (bool, error)
Python: PublishOp.flags(flags).submit() -> None
```

The other high-level bindings use separate publish operation types and publish terminals.

## 7. Implementation and contract-test verification requirements

Verify the following using only public builders, terminals, poller events, and language results. Each
item maps to one contract test.

**Operation surface**

- Each socket's send, request, and reply factory has the name listed in section 2, returns one send
  operation family, and preserves the target in the builder.
- Send and request terminals expose only the signatures in section 6. They do not expose send/request
  flags, a send timeout, or a request callback terminal.
- Async terminals return a result object. `result` is a submit-time `OK`|`BACKPRESSURED` snapshot; when `OK`
  the `admitted` stage is already complete, and when `BACKPRESSURED` `admitted` completes after WRITABLE
  resubmission. A REQUEST's `reply` completes only after `admitted` succeeds and fails with the same cause
  when `admitted` fails (exactly once). Contract tests confirm that submit failures other than
  `OK`|`BACKPRESSURED` surface as exceptions/errors and that `admitted` and `reply` each complete exactly once.
- Publish in Go and Python provides publish flags and synchronous submit results on a separate `PublishOp`.
- The reply terminal has no flags and returns the result of synchronous `NONE` admission.

**Completion and cancellation**

Submit-race, cancellation, and request-error observations follow the
[common execution model's verification requirements](async-execution-model.en.md#7-implementation-and-contract-test-verification-requirements).

**Poller and token**

Poller observations follow the [common execution model's verification requirements](async-execution-model.en.md#7-implementation-and-contract-test-verification-requirements).

- Tokens created by ROUTER REQUEST receive are equal for the same socket and value and unequal across
  sockets; invalid tokens and tokens owned by another socket are rejected before native reply.

<!-- bindings-nav:start -->
[Spec index](README.en.md) | [Previous: Overview](README.en.md) | [Next: C](c/README.en.md)
<!-- bindings-nav:end -->
