---
title: "Bindings Routed Submit Contract And Async Completion Surface Policy"
---

<!-- bindings-nav:start -->
[Spec index](README.en.md) | [Previous: Overview](README.en.md) | [Next: C](c/README.en.md)
<!-- bindings-nav:end -->

# Bindings Routed Submit Contract And Async Completion Surface Policy

> **Revision history (1st)** — Revised by owner decision on 2026-08-23. The
> canonical terminal for HWM-managed routed **send** reverts to a synchronous
> `submit()`, and the binding-owned admission machinery introduced to support
> the async-only rule (park queues, WRITABLE-callback retry, deadline timers,
> dispatcher threads) and the "HWM-managed routed is async-only" rule itself
> are abolished. The asynchronous completion surface for **request**
> (a suspension whose completion Core drives through the reply) is unchanged.
> For rationale and measurements, see
> `doc/plan/cpp-routed-async-contract-issue.ko.md` (§0 governing principle,
> §3.1 preflight findings, §3.2 final design).

> **Revision history (2nd)** — Revised by owner decision again on the same
> day, 2026-08-23. This revision normalizes the terminal surface: (1) under
> the rule then in force, it codifies that operations that can wait on HWM
> (PAIR send, DEALER/ROUTER routed send, request) are classified ASYNC purely
> because an HWM wait can occur; (2) it re-expands the C++ routed send surface
> the 1st revision had narrowed to "synchronous only" back into a two-terminal
> `submit()` (blocking) + `async()` (coroutine) surface; and (3) it restores
> C++ **request**'s three-terminal surface (`submit()` / `submit(callback)` /
> `async()`) to its pre-2026-08-15 shape. See [Classification
> principle](#classification-principle) and the sections below for details.
> C++ send's async completion depends on the Core send-completion
> notification design, which is in progress at
> `doc/plan/core-send-completion-design.ko.md` (a forward reference — once
> that notification surface is finalized, the relevant sections here will be
> updated to match its contract).

> **Revision history (3rd)** — Revised by owner decision on 2026-08-24. PUB/XPUB
> `publish` is reclassified as synchronous-only. Default PUB semantics are
> lossy: when a subscriber's queue reaches HWM, that subscriber's copy is
> dropped and the publisher proceeds immediately; the publisher never waits on
> HWM. With `ZLINK_PUB_OPT_NODROP` (an opt-in equivalent to ZMQ
> `XPUB_NODROP`), a full subscriber surfaces an immediate
> `BACKPRESSURED`/`EAGAIN` error from synchronous `submit()`, and retry policy
> belongs to the application. Core's `zlink_send_async` returns `ENOTSUP` for
> PUB/XPUB.

> **What this chapter defines** — the naming and return-type policy every
> language binding except C must follow for (1) the asynchronous completion
> surface of HWM-managed **send** (PAIR send, DEALER/ROUTER routed send),
> (2) the synchronous `submit()` completion surface of PUB/XPUB **publish**,
> (3) the Core-driven asynchronous completion of **request** (C++ is the
> exception, with three terminals: `submit()` / `submit(callback)` / `async()`),
> and (4) the synchronous one-shot completion of raw reply, which stays HWM-free.

This document defines the name and return type each language binding, except
C, must use for the asynchronous completion of HWM-managed PAIR **send** and
DEALER/ROUTER **routed send**, the synchronous `submit()` completion of PUB/XPUB
**publish**, the asynchronous completion of **request**, and the synchronous
completion of raw ROUTER/`Received` reply. The bindings library provides a
per-language completion boundary on top of the core C API. Coroutine execution,
virtual thread execution, event loop wiring, and handler dispatcher wiring are
the framework's responsibility.

**The bindings library owns no threads at all.** HWM-managed send and routed
send are classified ASYNC because an HWM wait can occur, but Core still drives
completion. C++'s blocking `submit()` waits inside Core and resumes on Core's
signal, `SNDTIMEO` bounds the wait, and `DONTWAIT` returns `EAGAIN` immediately.
Every other language's single `submit()` (Go's `Submit(ctx)`) wraps the Core
send but returns a language-idiomatic awaitable, and that awaitable's completion
is driven by the upcoming Core send-completion notification
(`doc/plan/core-send-completion-design.ko.md`, in progress). PUB/XPUB
`publish` never waits on HWM and completes through synchronous `submit()`.
Backpressure policy belongs to the application. Request
already has a point where Core itself drives completion (the reply handler
callback, `ZLINK_REQUEST_TIMED_OUT`), so a binding only wires that point to
complete the suspension, callback, or completion channel; it adds no retry
logic or thread of its own. Suspension resumption happens in the context
where Core delivered the completion. The bindings library owns no coroutine
executor or scheduler. It still provides a language-native suspension
object: a Python request builder's `submit()` returns an awaitable coroutine
object, and a Rust request builder's `submit()` returns a runtime-independent
`Future`. Neither is a new operation starting point or a framework executor.

| Section | Covers |
|---|---|
| [Classification principle](#classification-principle) | How HWM-wait possibility classifies send/request as ASYNC and publish/raw reply as SYNC |
| [Common principles](#common-principles) | Shared rules for operation start-point names, builders, and submit-failure representation |
| [HWM-managed send completion contract](#hwm-managed-send-completion-contract) | The asynchronous completion surface for HWM-managed send, plus the synchronous PUB/XPUB publish contract |
| [Request completion surface and C++'s three terminals](#request-completion-surface-and-cs-three-terminals) | The per-language completion surface for HWM-managed request, and C++'s three terminals |
| [Per-language normative table for send/publish/request/raw reply](#per-language-normative-table-for-sendpublishrequestraw-reply) | Each of the four operation types' per-language terminal and return type |
| [Synchronous one-shot raw reply](#synchronous-one-shot-raw-reply) | The seven bindings' reply terminals and immediate failure contract |
| [Framework typed Session reply](#framework-typed-session-reply) | The separate awaitable Framework contract |
| [How frameworks attach coroutines](#how-frameworks-attach-coroutines) | How each language's framework turns the completion boundary into its own execution model |

## Classification principle

- **An operation that can wait on HWM is classified as ASYNC.** PAIR send,
  DEALER/ROUTER routed send, and request pass through a point where an HWM wait
  *can* occur, so they are classified ASYNC. The classification criterion is
  not "does an HWM wait happen often" — it is "can an HWM wait happen at all."
- **Raw reply is HWM-free and genuinely synchronous.** A raw
  ROUTER/`Received` reply never passes through the HWM path, so it is a
  genuinely synchronous completion lane.
- **PUB/XPUB `publish` is outside this ASYNC classification.** Default PUB
  semantics are lossy: when a subscriber queue reaches HWM, that subscriber's
  copy is dropped and the publisher proceeds immediately. No HWM wait exists,
  so synchronous `submit()` is publish's terminal. With
  `ZLINK_PUB_OPT_NODROP`, a full subscriber surfaces immediate
  `BACKPRESSURED`/`EAGAIN` from synchronous submit; retry policy belongs to the
  application.
- This classification applies identically to bindings and framework
  surfaces. A framework's typed Session reply, managed request, and
  HWM-managed send split ASYNC/SYNC by the same criterion — the framework does
  not get a relaxed rule of its own. Publish is synchronous under the lossy
  contract above.
- This principle is the basis for every section below: C++ exposing
  `submit()`/`async()` for send (or request's three terminals), other languages
  returning an awaitable for send, and publish and raw reply remaining
  synchronous all follow from this classification principle.

## Common principles

- The bindings public API keeps a single name for an operation's starting point. It does not grow that name with completion style or flag combinations, as in `requestAsync`, `request_callback`, `sendNoWait`, or `publishWithFlags`.
- `send`, `request`, `reply`, `publish`, Actor location operations, and Actor session attach operations all return an operation builder.
- Payload and timeout are expressed at the builder stage, not as arguments to the operation's starting point.
- The bindings library does not own a coroutine scheduler, a Kotlin `CoroutineScope`, a C++ executor, or a framework dispatcher. **The bindings library owns no threads, queues, or retry policy of its own either** — for send, publish, routed send, or request.
- It does not add separate public APIs to the bindings contract for a coroutine-only recv, a virtual-thread-only recv, or a framework-dispatcher-only submit.
- Once a builder has been submitted, it cannot be submitted again. Where the language offers an ownership type or typestate, this is blocked by the type system; otherwise it is blocked by a runtime state check.
- Submit failure and reply failure are delivered through the return type's failure representation or that language's idiomatic exception.
- **Naming-distinction rule.** C++ must distinguish, by name, functions
  callable directly from a plain thread from functions callable only from a
  coroutine: `submit()` is the plain-thread terminal (a call that blocks
  inside Core), `async()` is the coroutine terminal (a call that suspends).
  .NET follows its own Async-suffix convention with `Async()` for
  async-classified operations and `Submit()` for synchronous publish and raw
  reply. Every other language keeps a single `submit()` (Go: `Submit(ctx)`)
  terminal per operation: an async-classified operation returns an awaitable
  consumed idiomatically (await / join / block_on / channel recv), while
  synchronous publish and raw reply are consumed immediately.
- The C++ request builder pairs `submit()` (blocking), `submit(callback)`
  (completion-delivery only), and `async()` (coroutine) together — per the
  naming-distinction rule above, C++ alone needs these three terminals. Other
  languages keep a single terminal, because the awaitable that terminal
  returns already serves every consumption style, leaving no reason to add
  another terminal. (The former rule forbidding a callback or
  blocking-compatible terminal alongside the canonical suspension terminal
  was repealed in the 2nd revision on 2026-08-23.)

## HWM-managed send completion contract

PAIR **send** and DEALER/ROUTER **routed send** are classified ASYNC per the
[classification principle](#classification-principle) — an HWM wait can occur.
Each invokes the language's canonical terminal on the builder returned by the
same operation entrypoint. It adds no separate name such as `send_async` or
`sendCoroutine`.

- **C++**: exposes both `submit()` (blocking — returns `bool` or throws
  `submit_error_t`, waiting inside Core and resuming on Core's signal) and
  `async()` (coroutine-only — returns a move-only `async_result_t<T>`). This
  follows the naming-distinction rule, separating the plain-thread call from
  the coroutine call.
- **Every other language**: exposes a single `submit()` (Go: `Submit(ctx)`)
  terminal, returning that language's idiomatic awaitable (`CompletionStage`,
  `Promise`, a coroutine object, a `Future`, and so on). Users consume that
  awaitable idiomatically — await / join / block_on / channel recv.
- **Completion driver.** Both the `async_result_t<T>` returned by C++'s
  `async()` and the awaitable returned by every other language's `submit()`
  are completed by the Core send-completion notification. That notification
  mechanism is in progress; see `doc/plan/core-send-completion-design.ko.md`
  (a forward reference — once that document is finalized, this section's
  contract will be updated to match its surface). C++'s blocking `submit()`
  does not depend on this notification and keeps using Core's internal
  wait/resume exactly as before.
- The `send_ready` readiness-hint semantics are abolished. The completion
  notification is deliverable via a callback or receivable as an event —
  two channels, and the consumer chooses between them; this is two options,
  not a duplicated surface.
- A binding keeps no park queue, WRITABLE-callback retry, deadline timer, or
  dispatcher thread for this completion surface — completion is driven by
  the Core send-completion notification.
- Submit flags may be accepted as a language-idiomatic option argument or
  builder stage.
- Backpressure policy is owned by the application. The binding does not
  retry.

| Aspect | bindings completion surface |
|---|---|
| Classification | ASYNC (an HWM wait can occur) |
| C++ execution meaning | `submit()` blocks inside Core and resumes on Core's signal. `async()` returns an awaitable `co_await`ed from a coroutine |
| Other-language execution meaning | The single `submit()` (Go: `Submit(ctx)`) returns a language-idiomatic awaitable |
| Blocking mode (C++ `submit()`) | Waits inside Core and resumes on Core's signal (no binding-side wait) |
| `SNDTIMEO` | Used by Core as the wait bound |
| `DONTWAIT` | Core returns `EAGAIN` immediately (surfaced as the language's `BACKPRESSURED`) |
| Completion notification | The Core send-completion notification (in progress, `doc/plan/core-send-completion-design.ko.md`) drives the awaitable/callback completion. The consumer chooses between a callback or a receivable event |
| Submit flags | May be accepted as a language-idiomatic option argument or builder stage |
| Failure | The language's idiomatic exception or `Result`/`error` return |
| Backpressure policy | Owned by the application. The binding does not retry |

### Synchronous publish submission contract

PUB/XPUB `publish` is lossy by default. When a subscriber's queue reaches HWM,
that subscriber's copy is dropped and the publisher proceeds immediately; the
publisher never waits on HWM. Publish is therefore outside the ASYNC
classification and exposes only a synchronous `submit()` terminal.

With `ZLINK_PUB_OPT_NODROP` (an opt-in equivalent to ZMQ `XPUB_NODROP`), a
publish targeting a full subscriber surfaces immediate
`BACKPRESSURED`/`EAGAIN` from synchronous `submit()`. Retry policy belongs to
the application. Core's
`zlink_send_async` returns `ENOTSUP` for PUB/XPUB, so there is no async publish
surface. Auto-HWM budget, per-queue caps, and manually configured `sndhwm`
still apply fully as each subscriber's drop threshold and memory bound; they do
not make the publisher wait. The normative table below gives publish the same
synchronous submit shape and failure representation as raw reply.

- **C++ async failure mapping (normative).** When an `async()` completion
  reports `TIMED_OUT` or `TERMINAL`, it surfaces as
  `submit_error_t(submit_result_t::not_admitted, terminal_errno)` — the
  operation never reached admission, and `terminal_errno` preserves the
  cause.
- **Flags are for the synchronous `submit()` only.** Submit flags such as
  `DONTWAIT` are meaningless on an async terminal, so C++ `async()` rejects
  non-zero flags with `EINVAL`.

## Request completion surface and C++'s three terminals

DEALER/ROUTER **request** is also classified ASYNC per the [classification
principle](#classification-principle). It invokes a terminal on the builder
returned by the same operation entrypoint. It adds no separate name such as
`requestCoroutine`, `request_async`, or `submit_async`.

### C++ request's three terminals (pre-2026-08-15 surface restored)

The C++ request builder exposes three terminals. The 2nd revision repeals
the 2026-08-15 rule that narrowed it to a single canonical suspension
terminal, restoring the earlier surface:

1. **`submit()`** — blocking. Returns the reply
   (`std::vector<message_t>`) directly after a Core-owned wait. Timeout
   expiry is signaled as `ZLINK_REQUEST_TIMED_OUT`.
2. **`submit(callback)`** — returns immediately. Core's reply callback
   drives the app callback — this terminal only handles completion
   delivery; the binding adds no retry logic or scheduling of its own.
3. **`async()`** — coroutine-only. Returns a move-only `async_result_t<T>`.
   This is the framework-canonical terminal — frameworks are
   coroutine-mandatory, so they use only this terminal.

Other languages keep a single terminal (`submit()`; .NET: `Async(...)`) —
per the naming-distinction rule, only C++ needs these three terminals, since
in every other language the terminal's returned awaitable already serves
every consumption style (await / join / block_on / channel recv), leaving no
reason to add another terminal.

Core drives reply completion: a reply handler callback completes the
suspension, callback, or completion channel, and resumption happens in the
context where that completion occurred. Request timeout is already
Core-owned (`ZLINK_REQUEST_TIMED_OUT`). A binding keeps no retry queue or
dedicated thread for this completion surface.

| Aspect | bindings completion surface |
|---|---|
| Classification | ASYNC (an HWM wait can occur) |
| C++ terminal | `submit()` (blocking, returns the reply directly) / `submit(callback)` (completion delivery) / `async()` (coroutine, framework canonical) |
| Other-language terminal | A single `submit()` (or .NET `Async(...)`) — returns a language-idiomatic awaitable or completion channel |
| Reply delivery | The suspension's success value, the callback's argument, or channel completion |
| Submit flags | The managed request terminal does not accept them |
| Timeout | The builder's `timeout(...)` stage; expiry is signaled by Core as `ZLINK_REQUEST_TIMED_OUT` |
| Submit failure | A failed task/future/promise, error result, or exception (C++ `submit()` throws) |
| Reply failure | Failure of the same completion surface |
| Resumption context | The context where Core delivered the completion (the reply handler callback). Further execution-model wiring belongs to the framework |

## Per-language normative table for send/publish/request/raw reply

The table below covers the four operation types — HWM-managed **send** (PAIR
send and DEALER/ROUTER routed send), **publish**, **request**, and **raw reply**
— with each language's terminal and return/completion representation. Send and
request are classified ASYNC per the [classification principle](#classification-principle);
publish and raw reply are SYNC. A framework may wrap this surface to provide
another execution model, but it must not grow the bindings public API's names.

| Binding | HWM-managed send | Publish | Request | Raw reply |
|---|---|---|---|---|
| C | Not applicable — the C ABI does not apply builder policy and follows the functional contract in `core/include/zlink.h` | Not applicable | Not applicable | Not applicable |
| C++ | `submit()` (blocking) → `bool` for PAIR/STREAM, `void` for routed; all failures throw `submit_error_t`. `async()` → move-only `async_result_t<T>` | `submit()` → synchronous `bool`; throws `submit_error_t` on failure | `submit()` (blocking) → the reply, throws on failure. `submit(callback)` → returns immediately, completion delivered via callback. `async()` → `async_result_t<T>` (framework canonical) | `submit()` → `void`; throws `submit_error_t` on failure |
| .NET | `Async()` → `Task`/`ValueTask`/`Task<T>`/`ValueTask<T>` | `Submit()` → synchronous `void`; throws `ZlinkSubmitException` on failure | `Async(...)` → same as above | `Submit()` → synchronous `void`; throws `ZlinkSubmitException` on failure |
| Java, Kotlin | `submit()` → `CompletionStage<T>` | `submit()` → synchronous `void`; throws `ZlinkSubmitException` on failure | `submit()` → `CompletionStage<T>` (same as above) | `submit()` → synchronous `void`; throws `ZlinkSubmitException` on failure |
| Node | `submit()` → `Promise<T>` (or `Promise<void>`) | `submit()` → synchronous `void`; throws `SubmitError` on failure | `submit()` → `Promise<T>` (same as above) | `submit()` → synchronous `void`; throws `SubmitError` on failure |
| Python | `submit()` → awaitable coroutine object | `submit()` → synchronous `None`; raises `SubmitError` on failure | `submit()` → same as above | `submit()` → synchronous `None`; raises `SubmitError` on failure |
| Go | `Submit(ctx)` → `error` (`nil` on success) | `Submit(ctx)` → synchronous `error` (`nil` on success) | `Submit(ctx)` → completion channel | `Submit(ctx)` → synchronous `error` (`nil` on success) |
| Rust | `submit()` → `Future<Output = Result<(), SubmitError>>` | `submit()` → synchronous `Result<(), SubmitError>` | `submit()` → runtime-independent `Future<Output = Result<Vec<message_t>, ZlinkError>>` | `submit()` → synchronous `Result<(), SubmitError>` |

Kotlin code that uses the Java binding directly follows the Java column's
contract as-is. The Kotlin Framework surface (`.reply(...).await()`, and so
on) is covered separately in [Framework typed Session
reply](#framework-typed-session-reply).

## Synchronous one-shot raw reply

A raw ROUTER/`Received` reply builder is neither HWM-managed send nor publish nor
request. As defined in the [classification principle](#classification-principle),
raw reply is a completion lane that never passes through the HWM path, which is
why it is genuinely synchronous. Its terminal submits a terminal reply or
error reply to the HWM-free completion lane with one native call and finishes
synchronously. HWM backpressure is not a reply result.
`NOT_CONNECTED`, `TERMINATED`, `INVALID_ARGUMENT`, or another non-HWM submit
failure is delivered immediately as the language's `SubmitError`.

| Binding | Raw reply terminal | Success return | Failure representation |
|---|---|---|---|
| C++ | `reply_submit_operation_t::submit()` | `void` | Throws `submit_error_t` |
| .NET | `ReplySubmitOperation.Submit()` | `void` | Throws `ZlinkSubmitException` |
| Java | `ReplySubmitOperation.submit()` | `void` | Throws `ZlinkSubmitException` |
| Node | `ReplySubmitOperation.submit()` | `void` | Throws `SubmitError` |
| Go | `ReplySubmitOp.Submit(ctx)` | `nil` | Returns `*SubmitError` as `error` |
| Python | `ReplyOp.submit()` | `None` | Raises `SubmitError` |
| Rust | `ReplyOp<Ready>::submit()` | `Ok(())` | Returns `Err(SubmitError)` |

Kotlin code that uses the Java binding directly also uses Java's synchronous
`submit(): void` reply contract. That API is distinct from the Kotlin Framework
`reply(...).await()` below.

## Framework typed Session reply

A Framework typed Session reply is not the raw binding reply with an async
terminal. The Framework runtime owns typed serialization and a one-shot reply
token for the request. Its terminal atomically claims that token and waits for
source-local admission. A second reply using the same token completes
exceptionally without attempting transport.

| Framework language | Typed Session reply terminal | Completion representation |
|---|---|---|
| C++ | `.reply_packet(...).submit()` | A Framework task that can be `co_await`ed |
| .NET | `.Reply(...).Async(ct)` | `ValueTask` |
| Java | `.reply(...).submit()` | `CompletionStage<Void>` |
| Kotlin | `.reply(...).await()` | Suspending `Unit` |
| Node | `.reply(...).submit(signal?)` | `Promise<void>` |

Therefore, even where C++, Java, and Node use the same `submit` name, return
type and owning layer distinguish the meaning. A raw binding reply is a
synchronous one-shot; only a Framework typed Session reply is awaitable for
source-local admission.

## How frameworks attach coroutines

A framework converts the completion boundary bindings provides into its
own execution model. **Frameworks are coroutine-mandatory** — every framework
language supports only a coroutine, or coroutine-equivalent async, execution
model, and provides no separate surface for a blocking execution model.
**Bindings provides no executor, but a framework does not need to build one
for HWM-managed send** — the send awaitable (C++'s `async()`, or the awaitable
every other language's `submit()` returns) is already completed by the Core
send-completion notification, so the framework simply `await`s / `co_await`s
that awaitable directly from its own coroutine. PUB/XPUB publish is not an
awaitable; the framework calls its synchronous `submit()` directly. Request
already returns a language-native awaitable (or C++'s `async()` result), so it
needs no such wrapping either.

### Java framework

- The Java framework wires the `CompletionStage` it gets from a managed
  request's `submit()` into the handler executor, virtual threads, and the
  timeout policy.
- The `CompletionStage` returned by HWM-managed send's `submit()` is wired the
  same way, exactly like managed request — the Core send-completion notification
  drives completion, so no separate executor wrapping is needed.
- For PUB/XPUB publish, the framework calls synchronous `submit()` directly
  and handles its immediate success or failure; it does not connect publish to
  a `CompletionStage` or coroutine suspension.
- Even when using virtual threads, it does not add a virtual-thread-only recv or submit API to bindings.

### Kotlin framework

- The Kotlin wrapper does not call Java's `await()`.
- The Kotlin wrapper gets the `CompletionStage` from a Java managed request
  builder's `submit()`, then connects it to coroutine suspension via
  `kotlinx-coroutines-jdk8`'s `await()`.
- HWM-managed send is connected to coroutine suspension the same way, via the
  `CompletionStage` from Java `submit()`'s `await()` — it does not need to be
  wrapped in a separate dispatcher such as `Dispatchers.IO`, since the Core
  send-completion notification already drives asynchronous completion; it is
  not a blocking call.
- For PUB/XPUB publish, the framework calls synchronous `submit()` directly
  and handles its immediate success or failure; it does not connect publish to
  coroutine suspension.
- Kotlin user code and Kotlin samples do not call Java's `submit()` directly — they use Kotlin wrappers such as `connect().await()`, `request(...).await<T>()`, and `waitFor<T>(...).await()`.
- The Kotlin framework owns the coroutine scope, dispatcher, and cancellation handling. The bindings library does not create a scope or dispatcher.

### C++ framework

- C++ bindings provides move-only `async_result_t<T>` through a managed
  request's `async()`, and user and framework coroutines `co_await` it
  directly.
- HWM-managed send works the same way: the `async_result_t<T>` returned by
  `async()` is `co_await`ed directly by the framework coroutine — the Core
  send-completion notification drives completion, so the framework does not
  need to wrap blocking `submit()` in a separate executor. Because frameworks
  are coroutine-mandatory, they use `async()` for send, not blocking `submit()`.
- For PUB/XPUB publish, the framework calls synchronous `submit()` directly;
  it does not use `async()` or `co_await`.
- A standalone coroutine may resume in the context where its completion
  occurred (such as a Core reply handler callback, or the Core
  send-completion notification callback). This does not permit the
  binding to create its own threads, dispatchers, or schedulers for completion
  or resumption — execution resources belong to the framework. The Framework
  `task_t` promise uses an optional continuation-scheduler hook to hand off
  only the current serial turn and ambient context.
- The bindings public API does not add a coroutine-only `request_async` or `request_coroutine`, a framework executor argument, or a framework dispatcher argument.

### Other language frameworks

- The .NET framework `await`s the `Task`/`ValueTask` returned by a managed
  request as-is. The `Task`/`ValueTask` returned by HWM-managed send's
  `Async()` is `await`ed the same way, directly — it does not need to be
  wrapped in an executor such as `Task.Run(...)`. For PUB/XPUB publish, it
  calls `Submit()` directly and handles the returned success or failure.
- The Node framework `await`s the `Promise` returned by a managed request
  according to its event loop policy. The `Promise` returned by HWM-managed
  send's `submit()` is `await`ed the same way, directly — it does not need to
  be wrapped with a worker-offload policy. For PUB/XPUB publish, it calls
  synchronous `submit()` directly.
- The Python framework awaits a managed request's `submit()` coroutine object
  directly. HWM-managed send's `submit()` coroutine object is awaited the same
  way, directly — it does not need to be wrapped in an execution path such as
  `loop.run_in_executor(...)`. For PUB/XPUB publish, it calls synchronous
  `submit()` directly.
- The Rust framework polls a managed request's runtime-independent `submit()`
  Future on its own executor. HWM-managed send's `submit()` Future is polled
  the same way — it does not need to be wrapped with something like
  `spawn_blocking`. For PUB/XPUB publish, it calls synchronous `submit()`
  directly.

This way, bindings keeps its responsibility as a C API wrapper, while each
framework can independently provide coroutine support that fits its own
execution model. Bindings owns no thread in either case.
