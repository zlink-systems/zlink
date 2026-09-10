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
`Submit(context.Context)` terminal, submits with Core `DONTWAIT`, and then waits for the internal
completion.

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
