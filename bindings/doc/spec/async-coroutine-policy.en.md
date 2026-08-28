---
title: "Bindings Routed Submit Contract And Async Completion Surface Policy"
---

<!-- bindings-nav:start -->
[Spec index](README.en.md) | [Previous: Overview](README.en.md) | [Next: C](c/README.en.md)
<!-- bindings-nav:end -->

# Bindings Routed Submit Contract And Async Completion Surface Policy

> **What this chapter defines** — for every language binding except C, it fixes
> the **names and return types** by which the four operation types expose their
> completion surfaces.

The four operation types and their completion surfaces covered here:

| operation | completion surface |
|---|---|
| **HWM-managed send** (PAIR send, DEALER/ROUTER routed send, `Received.send()`) | async terminal **and** sync(+flags) terminal (Go, by convention, sync only) |
| **publish** (PUB/XPUB) | synchronous `submit()` |
| **request** | three terminals — sync return / sync callback (+flags) / async, in every binding |
| **raw reply** (ROUTER/`Received`) | HWM-free synchronous one-shot |

The bindings library provides a per-language completion boundary over the core C
API. Execution-model wiring (OS thread·virtual thread·coroutine·event loop) and
handler-dispatcher wiring are the framework's job.

> **Terminology.** The synchronous/asynchronous, execution-model
> (thread·coroutine·event loop), and awaitable·terminal definitions this document
> uses follow [Async Execution Model and Completion Surface Terminology](async-execution-model.en.md).
> "Asynchronous" is the umbrella term that also covers thread-based asynchrony,
> and "coroutine" is used only when naming an actual coroutine language.

**The bindings library owns no thread or queue for admission waiting or retry.**
Admission waiting and retry are owned by Core. Completion works per operation
type as follows.

- **HWM-managed send / routed send** — Core owns admission waiting and retry.
  - C++ blocking `submit()` and Go `Submit(ctx)` wait inside Core.
  - Every other language's async terminal submits via `zlink_send_async` and then
    finishes its awaitable on the final completion delivered by
    `zlink_send_complete_handler`.
  - An operation Core has admitted completes exactly once, as one of
    `ZLINK_SEND_ADMITTED`, `ZLINK_SEND_TIMED_OUT`, or `ZLINK_SEND_TERMINAL`. The
    binding only links the operation id to the per-language completion object and
    does not retry.
  - Only when Core's pending-operation ceiling **rejects the initial submit**
    does the application decide whether to retry.
- **publish** — does not wait at HWM, so it completes with a synchronous
  `submit()`.
- **request** — Core itself drives completion at a point (reply handler callback,
  `ZLINK_REQUEST_TIMED_OUT`). The binding merely wires that terminal to a
  suspension·callback·completion channel and keeps no retry or admission queue of
  its own.
  - Languages whose completion call can run the user continuation inline can
    complete off the native callback thread using their existing completion
    dispatcher. This does not mean adding a request-specific executor or
    scheduler.
  - Idiomatic language suspension objects are part of the binding contract:
    Python `submit()` returns an `await`-able coroutine object, and Rust
    `submit()` returns a runtime-agnostic `Future`. These are not new operation
    entry points or a framework executor.

| Section | What it covers |
|---|---|
| [Classification principle](#classification-principle) | the criterion that classifies send/request as ASYNC and publish/raw reply as SYNC, based on whether HWM waiting is possible |
| [Common principles](#common-principles) | shared rules for operation-entrypoint names, builders, and how submit failure is expressed |
| [HWM-managed send completion contract](#hwm-managed-send-completion-contract) | the async completion of HWM-managed send (including routed send) and the synchronous PUB/XPUB publish submission contract |
| [Request completion surface and three terminals](#request-completion-surface-and-three-terminals) | the three terminals of HWM-managed request (sync return/sync callback/async) and their per-language surfaces |
| [Per-language normative table for send/publish/request/raw reply](#per-language-normative-table-for-sendpublishrequestraw-reply) | per-language terminal and return type for each of the four operation types |
| [Raw reply synchronous one-shot](#raw-reply-synchronous-one-shot) | the reply terminal and immediate-failure contract across the seven bindings |
| [The framework's consumption rule (pointer)](#the-frameworks-consumption-rule-pointer) | how the framework uses these surfaces is owned by the framework spec |

## Classification principle

- **Any operation where HWM waiting can occur is classified as an ASYNC
  function.** PAIR send, DEALER/ROUTER routed send, and request pass through a
  point where HWM waiting *may* occur, so they are classified ASYNC. The criterion
  is not "does HWM waiting occur often in practice" but "is HWM waiting possible."
  This operation classification does not force the terminal return type in every
  language. Go's `Submit(ctx) error` is a synchronous terminal that waits inside
  Core.
- **Raw reply is HWM-free and genuinely synchronous.** A raw ROUTER/`Received`
  reply never traverses the HWM path, so it is a purely synchronous completion
  lane.
- **PUB/XPUB `publish` is not part of this ASYNC classification.** Default PUB
  semantics are lossy drop, so even when a subscriber queue reaches HWM the copy
  is dropped and the publisher proceeds immediately. There is therefore no HWM
  waiting, and publish's synchronous `submit()` is the terminal. With
  `ZLINK_PUB_OPT_NODROP` a full subscriber surfaces `BACKPRESSURED`/`EAGAIN`
  immediately from the synchronous submit, and the retry policy is owned by the
  application.
- The basis of this classification (the possibility of HWM waiting) holds
  regardless of the surface layer. How the framework surfaces follow this
  classification is owned by the framework spec
  (see [the framework's consumption rule](#the-frameworks-consumption-rule-pointer)).
  Publish is separately synchronous per the lossy contract above.
- This principle is the basis for every section below: C++ exposing both
  `submit()`/`async()` on send (or the three terminals on request), other
  languages exposing sync/async terminals on send and request, and publish and
  raw reply staying purely synchronous all derive from this classification
  principle.

## Common principles

- The bindings public API keeps a single name for each operation entry point. It
  does not proliferate entry-point names by completion mode or flag combination,
  such as `requestAsync`, `request_callback`, `sendNoWait`, `publishWithFlags`.
- `send`, `request`, `reply`, `publish`, Actor placement operations, and Actor
  session-attach operations return an operation builder.
- Payload and timeout are expressed in the builder stage, not as operation
  entry-point arguments.
- A single-part payload uses the same builder. The caller adds `Message(...)`
  once and then calls the terminal. Direct single-part shortcut APIs like
  `Send(Message)`, `Send(RoutingId, Message)`, `Publish(topic, Message)` are not
  added to the public contract. An implementation may pick an internal one-part
  fast path, but it must not become a separate API the caller chooses.
- The bindings library owns no framework coroutine scheduler, Kotlin
  `CoroutineScope`, or C++ framework executor. **The bindings library also owns no
  admission queue or retry policy** — the same for send, publish, routed send, or
  request. Delivering the terminal from an existing completion dispatcher, so that
  a language future/promise's inline continuation does not re-enter the Core
  callback, is allowed; but no per-operation executor·queue·timer is added.
- No separate public API such as coroutine-only recv, virtual-thread-only recv, or
  a framework-dispatcher-only submit is added to the bindings contract.
- A builder cannot be submitted twice. If the language offers an ownership type or
  typestate, block it by type; otherwise block it with a runtime state check.
- Submit failure and reply failure are delivered via the return type's failure
  representation or an idiomatic language exception.
- **Name-distinction principle — reflecting each language's characteristics is the
  default policy.** Each language's terminal shape follows that language's idiom.
  C++ must distinguish, by name, a function callable directly on a plain thread
  from a function callable only from a coroutine — `submit()` is for the plain
  thread (a call that blocks inside Core), and `async()` is for the coroutine (a
  suspending call). .NET puts an `Async()` terminal following its own Async-suffix
  convention on async-classified operations, and a `Submit()` terminal on
  synchronous publish and raw reply. Every other language keeps a single
  `submit()` (Go: `Submit(ctx)`) terminal per operation. (Exception: HWM-managed
  **send and request** have both sync and async terminals to expose the admission-
  flag contract — this is not name proliferation but a split between the flag-
  bearing SYNC path and the flagless ASYNC path. See the [normative table](#per-language-normative-table-for-sendpublishrequestraw-reply).)

  **Unified sync-terminal naming** — in languages whose async terminal is
  `submit()` (Java, Node, Python, and Rust), the sync terminal is consistently
  named **`submit_sync`**. Java must use a distinct name because it cannot overload
  on return type alone, and the others follow it for symmetry. **`submit_sync`
  names the sync/async axis** ([async-execution-model](async-execution-model.en.md));
  whether it actually blocks is selected by the flag (`NONE`/`DONTWAIT`). This is
  why the name is `submit_sync`, not `submit_blocking`: with `DONTWAIT` it is
  non-blocking. C++ (`async()`/`submit()`), .NET (`Async()`/`Submit()`), and Go
  (`Submit(ctx)`) retain their own conventions because their async terminal is not
  `submit()`.
  The awaitable returned by an async-classified operation's terminal is consumed
  the idiomatic way of each language (await / join / block_on / channel recv),
  while synchronous publish and raw-reply terminals are consumed the moment they
  are called. By the same principle Go's send `Submit(ctx)` is a synchronous form
  returning `error`. A blocking call inside a goroutine is Go's idiomatic way to
  wait, and `context.Context` checks for pre-submit cancellation and deadline.
- **The request builder exposes three terminals in every binding** — sync return
  (blocking and returns the reply directly), sync callback (immediate admission
  result, with the reply delivered by callback), and async (awaitable). Because a
  request passes through HWM admission just like send, the sync callback surface
  is needed to express C's consecutive `DONTWAIT` submission model as a public
  binding surface (0.14.0). Names follow the rule above: C++
  `submit()`/`submit(callback)`/`async()`, .NET
  `Submit(SendFlags)`/`Submit(SendFlags, callback)`/`Async()`, Java, Node, Python,
  and Rust `submit_sync(flags)`/`submit_sync(flags, callback)`/`submit()`. Go uses
  `Submit(ctx)` (+Flags) and a completion channel for fire-and-collect, with no
  separate callback method. See the [normative table](#per-language-normative-table-for-sendpublishrequestraw-reply).

## HWM-managed send completion contract

This section defines the **completion mode and common contract** of the send
family (PAIR **send**, DEALER/ROUTER **routed send**, `Received.send()`). The
per-language terminal names and return types are owned by the
[normative table](#per-language-normative-table-for-sendpublishrequestraw-reply)
and are not repeated here.

Because the send family can incur HWM waiting, most bindings expose **two
terminals** (sync/async). **Go, by convention, keeps only the sync terminal**
(see the normative table); this is not a contract violation but a stated
per-language exception.

- **sync terminal**: takes a send flag. With no flag or `NONE` it waits blocking
  inside Core; with `DONTWAIT` it returns backpressure
  (`BACKPRESSURED`/`EAGAIN`) immediately.
- **async terminal**: takes no flag. It finishes its awaitable on the
  `zlink_send_async` completion notification. (Per-language composition follows
  the normative table — e.g. C++ has `submit()` (sync)/`async()`, while Go keeps
  only sync `Submit(ctx)`.)

This is not name proliferation — it splits the flag-bearing SYNC path from the
flagless ASYNC path, and does not create separate names like `send_async` or
`sendCoroutine`.

### Completion notification (common)

- The async terminal first installs a `zlink_send_complete_handler`. Immediate
  admission returns operation id `0` and the binding completes the awaitable
  immediately. When it waits at HWM it links a nonzero operation id to the
  awaitable, and `zlink_send_complete_event_t` delivers exactly once as one of
  `ZLINK_SEND_ADMITTED`·`ZLINK_SEND_TIMED_OUT`·`ZLINK_SEND_TERMINAL`.
- Completions for the same target preserve submission order, and one socket's
  completion callbacks never run concurrently with each other. Registering
  `ZLINK_POLLCOMPLETION` only moves the dispatch site of the same callback and
  event to the `zlink_poller_wait` calling thread; it does not create a separate
  completion path.
- The binding only links the operation id to a per-language completion object and
  does not retry. HWM retry of an accepted operation is owned by Core. **Only when
  the initial submit is rejected at the pending-operation ceiling** does the
  application decide whether to retry.
- There is no public send-ready handler. `ZLINK_POLLOUT` is a readiness value for
  retrying a synchronous nonblocking send, not a completion of an accepted async
  operation.
- For this completion surface the binding keeps no park queue, WRITABLE-callback
  retry, or deadline timer. The default C++ coroutine, whose continuation may call
  the next submit inside the callback, resumes off the callback via the
  continuation dispatcher on the pending slow path.

### Implementation description (native path)

- The async terminal submits via native `zlink_send_async` — this function is
  always nonblocking and has no flags field.
- The sync terminal submits via native `zlink_send_part(_rid)` + send flag — with
  no flag it waits blocking inside Core, and with `DONTWAIT` it returns `EAGAIN`
  (per-language `BACKPRESSURED`) immediately.
- Therefore a call that needs `DONTWAIT` uses the sync terminal, not the async
  terminal.
- (If the native path changes, align this section's wording to the code.)

### Contract summary (common)

| Item | Contract |
|---|---|
| classification | HWM waiting possible — two terminals, sync·async |
| `SNDTIMEO` | Core uses it as the upper bound for sync blocking waits |
| `DONTWAIT` | on the sync terminal Core returns `EAGAIN` (per-language `BACKPRESSURED`) immediately. The async terminal has no flag (`zlink_send_async` has no flags field) |
| completion notification | `zlink_send_complete_handler` delivers `zlink_send_complete_event_t` exactly once per accepted operation. `ZLINK_POLLCOMPLETION` only changes the dispatch owner |
| failure | idiomatic language exception or `Result`/`error` return |
| backpressure policy | Core retries an accepted operation. Only the initial submit rejection is application policy |

### Publish synchronous submission contract

PUB/XPUB `publish` does not wait at HWM, so it is not in the ASYNC classification
and provides **only a synchronous `submit()`**. The rules are:

- **Lossy by default.** When a subscriber's queue reaches HWM, the copy for that
  subscriber is dropped and the publisher proceeds immediately. The publisher
  never waits at HWM.
- **`ZLINK_PUB_OPT_NODROP`** (opt-in, equivalent to ZMQ `XPUB_NODROP`): a publish
  to a full subscriber becomes an immediate `BACKPRESSURED`/`EAGAIN` error from
  the synchronous `submit()`. Whether and how to retry is owned by the
  application.
- **No async surface.** Core's `zlink_send_async` returns `ENOTSUP` for PUB/XPUB.
- **Meaning of HWM settings.** Auto-HWM budget, per-queue caps, and manual
  `sndhwm` all apply as per-subscriber drop thresholds and memory bounds, and do
  not mean publisher waiting.
- **Terminal shape.** Each language's publish terminal and failure representation
  follow the same synchronous-submit shape as the raw-reply column of the
  normative table.

- **C++ async failure mapping (normative).** If `async()`'s completion result is
  `TIMED_OUT` or `TERMINAL`, deliver it as `submit_error_t(submit_result_t::not_admitted,
  terminal_errno)` — since it is a failure that never reached admission,
  `not_admitted` is correct, and `terminal_errno` preserves the cause.
- **Flags are for synchronous `submit()` only.** Submit flags such as `DONTWAIT`
  are meaningless on an async terminal, so C++ `async()` rejects non-zero flags
  with `EINVAL`.

## Request completion surface and three terminals

DEALER/ROUTER **request** is also classified ASYNC per the
[classification principle](#classification-principle). The terminal is called on
the builder returned by the same operation entry point. Separate names like
`requestCoroutine`, `request_async`, or `submit_async` are not created.

**Request submission passes through HWM admission just like send.** The request
message itself travels over the send path, so submitting a request can encounter
an admission wait (HWM). Therefore the request sync terminals have the **same
admission-flag contract** as send: `NONE` waits for admission, while `DONTWAIT`
immediately returns `BACKPRESSURED`/`EAGAIN`. Reply completion is driven
separately by Core (`ZLINK_REQUEST_TIMED_OUT`).

### Three terminals (every binding)

The request builder exposes three completion surfaces. Names follow the
[naming rule](#common-principles): languages whose async terminal is `submit()`
(Java, Node, Python, and Rust) call the sync terminal `submit_sync`; C++ uses
`submit()`/`submit(callback)` and `async()`, .NET uses `Submit(...)` and
`Async(...)`, and Go uses `Submit(ctx)`.

1. **sync return** — blocking. It takes an admission flag and, at the end of a
   Core-owned wait, **returns the reply directly**. With `NONE` it waits for
   admission and then for the reply. Timeout expiry is signaled via
   `ZLINK_REQUEST_TIMED_OUT`. Names: Java, Node, Python, and Rust
   `submit_sync(flags)`; C++ `submit()`; .NET `Submit(SendFlags)`; Go
   `Submit(ctx)` (+Flags).
2. **sync callback** — synchronously returns the **admission result** as soon as
   the request is submitted (`DONTWAIT` immediately returns `BACKPRESSURED`), and
   delivers the reply later by **callback**. This is the key surface for
   submitting consecutive reqrep operations without serializing them. The binding
   performs no retry or scheduling. Names: Java, Node, Python, and Rust
   `submit_sync(flags, callback)`; C++ `submit(callback)`; .NET
   `Submit(SendFlags, callback)`. Idiomatic Go uses the completion channel returned
   by `Submit(ctx)` for fire-and-collect and has no separate callback method.
3. **async** — for coroutines/awaitables. It returns the language awaitable (C++
   `async_result_t<T>`, .NET `Task`, Java `CompletionStage`, Node `Promise`, Python
   coroutine object, Rust `Future`, or Go completion channel). It is the framework
   It takes no flag.

The sync-return and sync-callback terminals **take an admission flag**; the async
terminal does not, just as with send. The sync callback (or Go channel) expresses
request admission backpressure, and admission backpressure (HWM), rather than a
hard-coded limit, determines the outstanding reqrep depth.

Reply completion is driven by Core. The reply handler callback takes the terminal
and payload exactly once. If a language future/promise can run the user
continuation inline on the completion-calling thread, the binding hands terminal
delivery to its existing completion dispatcher and completes off the native
callback thread. Request timeout is already Core-owned
(`ZLINK_REQUEST_TIMED_OUT`). For this completion surface the binding adds no
admission·retry queue, per-operation executor, or timer.

| Aspect | bindings completion surface |
|---|---|
| classification | ASYNC (HWM waiting possible) |
| terminal (every binding) | **sync return** (blocking, returns reply directly) / **sync callback** (immediate admission result, reply via callback) / **async** (awaitable) |
| per-language names | C++ `submit()`/`submit(callback)`/`async()` · .NET `Submit(SendFlags)`/`Submit(SendFlags, cb)`/`Async()` · Java/Node/Python/Rust `submit_sync(flags)`/`submit_sync(flags, cb)`/`submit()` · Go `Submit(ctx)` (+Flags) + channel |
| reply delivery | sync return value, callback argument, suspension success value, or channel completion |
| submit flags | **sync return and sync callback take an admission flag** (`NONE` waits for admission / `DONTWAIT` returns immediate backpressure, as with send). The async terminal takes none |
| timeout | the builder's `timeout(...)` stage. Expiry is notified by Core via `ZLINK_REQUEST_TIMED_OUT` |
| submit failure | failed task/future/promise, an error result, or an exception (C++ `submit()` throws) |
| reply failure | a failure of the same completion surface |
| resume context | the language binding's completion context. Inline-continuation languages may complete off the native reply callback from the existing completion dispatcher, and the subsequent execution-model wiring is the framework's job |

## Per-language normative table for send/publish/request/raw reply

The table below sets out, for each of the four operation types — HWM-managed
**send** (including PAIR send and DEALER/ROUTER routed send), **publish**,
**request**, and **raw reply** — the per-language terminal and its
return/completion representation. HWM-managed send can incur HWM waiting, so it
has **two surfaces, an async terminal and a sync(+flags) terminal** — async
handles the wait asynchronously, and sync decides whether to wait via a send flag
(blocking by default, `DONTWAIT` for immediate backpressure). Request is ASYNC
per the [classification principle](#classification-principle), and publish and raw
reply are SYNC. A framework may wrap these surfaces to provide a different
execution model, but must not proliferate bindings public API names. **Adding a
sync terminal to HWM-managed send is not name proliferation but the exposure, on
each language surface, of the already-existing flag contract (SYNC nonblocking
send).**

The send cell of the table holds **only what differs per language — the terminal
name and return type**. The rules common to all languages are as follows and are
not repeated in the cells.

- **sync terminal**: takes a send flag. With no flag or `NONE`, blocking by
  default (HWM waiting); with `DONTWAIT`, immediate backpressure
  (`BACKPRESSURED`/`EAGAIN`).
- **async terminal**: takes no flag. HWM waiting and retry are owned by Core.
- Failure is delivered idiomatically (an exception or a `Result`/`error` return).

The SendOp returned by `Received.send()` is also an HWM-managed send, so it
follows this table's send contract as-is. `reply()` is not part of the send family
— it does not hit HWM and is sent on a separate completion lane, so it has only
the single synchronous terminal of the raw-reply column and no send flag.

| Binding | HWM-managed send | Publish | Request | Raw reply |
|---|---|---|---|---|
| C | not applicable — the C ABI applies no builder policy and follows the functional contract in `core/include/zlink.h` | not applicable | not applicable | not applicable |
| C++ | sync `submit()`+`flags(int)` → PAIR/STREAM `bool`, routed `void`. async `async()` → `async_result_t<T>` | `submit()` → synchronous `bool`, throws `submit_error_t` on failure | `submit()` (blocking) → reply, throws on failure. `submit(callback)` → returns immediately, completion via callback. `async()` → `async_result_t<T>` | `submit()` → `void`, throws `submit_error_t` on failure |
| .NET | sync `Submit(SendFlags)` → `void`. async `Async()` → `Task`/`ValueTask`(`<T>`) | `Submit()` → synchronous `void`, throws `ZlinkSubmitException` on failure | sync return `Submit(SendFlags)` → reply. sync callback `Submit(SendFlags, callback)` → immediate admission. async `Async(ct)` → `Task`. Throws `ZlinkSubmitException` on failure | `Submit()` → synchronous `void`, throws `ZlinkSubmitException` on failure |
| Java, Kotlin | sync `submit_sync(SendFlags)` → `void`. async `submit()` → `CompletionStage<T>` | `submit()` → synchronous `void`, throws `ZlinkSubmitException` on failure | sync return `submit_sync(SendFlags)` → reply. sync callback `submit_sync(SendFlags, callback)` → immediate admission. async `submit()` → `CompletionStage<T>`. Throws `ZlinkSubmitException` on failure | `submit()` → synchronous `void`, throws `ZlinkSubmitException` on failure |
| Node | sync `submit_sync(SendFlags)` → `void`. async `submit()` → `Promise<T>` (or `Promise<void>`) | `submit()` → synchronous `void`, throws `SubmitError` on failure | sync return `submit_sync(SendFlags)` → reply. sync callback `submit_sync(SendFlags, callback)` → immediate admission. async `submit()` → `Promise<T>`. Throws `SubmitError` on failure | `submit()` → synchronous `void`, throws `SubmitError` on failure |
| Python | sync `submit_sync(*, flags)` → `None`. async `submit()` → an `await`-able coroutine object | `submit()` → synchronous `None`, raises `SubmitError` on failure | sync return `submit_sync(*, flags)` → reply. sync callback `submit_sync(*, flags, callback)` → immediate admission. async `submit()` → coroutine object. Raises `SubmitError` on failure | `submit()` → synchronous `None`, raises `SubmitError` on failure |
| Go | sync `Submit(ctx)`+builder `Flags` → `error` (`nil` on success). no async terminal (Go convention) | `Submit(ctx)` → synchronous `error` (`nil` on success) | `Flags(...).Submit(ctx)` → admission result (`error`) + reply via completion channel. The channel supports fire-and-collect (no separate callback) | `Submit(ctx)` → synchronous `error` (`nil` on success) |
| Rust | sync `submit_sync(SendFlags)` → `Result<(), SubmitError>`. async `submit()` → `Future<Output = Result<(), SubmitError>>` | `submit()` → synchronous `Result<(), SubmitError>` | sync return `submit_sync(SendFlags)` → reply. sync callback `submit_sync(SendFlags, callback)` → immediate admission. async `submit()` → runtime-agnostic `Future<Output = Result<Vec<message_t>, ZlinkError>>` | `submit()` → synchronous `Result<(), SubmitError>` |

When Kotlin uses the Java binding directly it follows the Java column's contract
as-is. The Kotlin Framework surface (`.reply(...).await()`, etc.) is owned by
the framework spec (see
[the framework's consumption rule](#the-frameworks-consumption-rule-pointer)).

## Raw reply synchronous one-shot

A raw ROUTER/`Received` reply builder is not an HWM-managed send, publish, or
request. As defined in the [classification principle](#classification-principle),
a raw reply never traverses the HWM path — it is a completion lane, and so it is
genuinely synchronous. The terminal submits the terminal reply or error reply to
the HWM-free completion lane with a single native call and finishes synchronously.
HWM backpressure is not a reply outcome. `NOT_CONNECTED`, `TERMINATED`,
`INVALID_ARGUMENT`, or any other non-HWM submit failure is delivered immediately
as a per-language `SubmitError`.

| Binding | Raw reply terminal | success return | failure representation |
|---|---|---|---|
| C++ | `reply_submit_operation_t::submit()` | `void` | throws `submit_error_t` |
| .NET | `ReplySubmitOperation.Submit()` | `void` | throws `ZlinkSubmitException` |
| Java | `ReplySubmitOperation.submit()` | `void` | throws `ZlinkSubmitException` |
| Node | `ReplySubmitOperation.submit()` | `void` | throws `SubmitError` |
| Go | `ReplySubmitOp.Submit(ctx)` | `nil` | returns `*SubmitError` as `error` |
| Python | `ReplyOp.submit()` | `None` | raises `SubmitError` |
| Rust | `ReplyOp<Ready>::submit()` | `Ok(())` | returns `Err(SubmitError)` |

When Kotlin uses the Java binding directly it also uses Java's synchronous
`submit(): void` reply contract as-is. The Kotlin Framework
`reply(...).await()` is a distinct API owned by the framework spec.

## The framework's consumption rule (pointer)

Which of this document's binding surfaces (the send, publish, request, and
raw reply terminals) the framework consumes, and under what rules — the
async-terminal-only principle, the sanctioned sync-terminal exceptions
(immediate backpressure observation; implementing a public synchronous
contract), and the framework typed Session reply surface — is owned by the
framework spec,
[Submit And Completion §15 "Consuming Binding Send Terminals"](../../../framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.en.md#15-consuming-binding-send-terminals-implementation).
This document defines only what the bindings provide.
