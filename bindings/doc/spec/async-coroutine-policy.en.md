---
title: "Bindings Routed Submit Contract And Async Completion Surface Policy"
---

<!-- bindings-nav:start -->
[Spec index](README.en.md) | [Previous: Overview](README.en.md) | [Next: C](c/README.en.md)
<!-- bindings-nav:end -->

# Bindings Routed Submit Contract And Async Completion Surface Policy

> **Revision history** — Revised by owner decision on 2026-08-23. The
> canonical terminal for HWM-managed routed **send** reverts to a synchronous
> `submit()`, and the binding-owned admission machinery introduced to support
> the async-only rule (park queues, WRITABLE-callback retry, deadline timers,
> dispatcher threads) and the "HWM-managed routed is async-only" rule itself
> are abolished. The asynchronous completion surface for **request**
> (a suspension whose completion Core drives through the reply) is unchanged.
> For rationale and measurements, see
> `doc/plan/cpp-routed-async-contract-issue.ko.md` (§0 governing principle,
> §3.1 preflight findings, §3.2 final design).

> **What this chapter defines** — the naming and return-type policy every
> language binding except C must follow for (1) the synchronous `submit()`
> contract of HWM-managed routed **send**, (2) the Core-driven asynchronous
> completion of **request**, and (3) the synchronous one-shot completion of
> raw reply.

This document defines the name and return type each language binding, except
C, must use for the synchronous submit of HWM-managed DEALER/ROUTER **send**,
the asynchronous completion of **request**, and the synchronous completion of
raw ROUTER/`Received` reply. The bindings library provides a per-language
completion boundary on top of the core C API. Coroutine execution, virtual
thread execution, event loop wiring, and handler dispatcher wiring are the
framework's responsibility.

**The bindings library owns no threads at all.** Routed send is a synchronous
call that wraps the Core send directly; HWM wait, resume, and timeout
semantics are entirely Core-owned: blocking mode waits inside Core and resumes
on Core's signal, `SNDTIMEO` bounds the wait, and `DONTWAIT` returns `EAGAIN`
immediately. Backpressure policy belongs to the application. Request differs —
Core itself drives completion at a point it owns (the reply handler
callback), so a binding only wires that callback to complete the suspension;
it adds no retry logic or thread of its own. Suspension resumption happens in
the context where Core delivered the completion. The bindings library owns no
coroutine executor or scheduler. It still provides a language-native
suspension object: a Python request builder's `submit()` returns an awaitable
coroutine object, and a Rust request builder's `submit()` returns a
runtime-independent `Future`. Neither is a new operation starting point or a
framework executor.

| Section | Covers |
|---|---|
| [Common principles](#common-principles) | Shared rules for operation start-point names, builders, and submit-failure representation |
| [Routed send synchronous submit contract](#routed-send-synchronous-submit-contract) | The synchronous submit completion surface for HWM-managed routed send |
| [Request asynchronous completion](#request-asynchronous-completion) | The language-native suspension surface for HWM-managed request |
| [Routed send and request names and return types](#routed-send-and-request-names-and-return-types) | Each routed send/request builder's terminal method and return type |
| [Synchronous one-shot raw reply](#synchronous-one-shot-raw-reply) | The seven bindings' reply terminals and immediate failure contract |
| [Framework typed Session reply](#framework-typed-session-reply) | The separate awaitable Framework contract |
| [How frameworks attach coroutines](#how-frameworks-attach-coroutines) | How each language's framework turns the completion boundary into its own execution model |

## Common principles

- The bindings public API keeps a single name for an operation's starting point. It does not grow that name with completion style or flag combinations, as in `requestAsync`, `request_callback`, `sendNoWait`, or `publishWithFlags`.
- `send`, `request`, `reply`, `publish`, Actor location operations, and Actor session attach operations all return an operation builder.
- Payload and timeout are expressed at the builder stage, not as arguments to the operation's starting point.
- The bindings library does not own a coroutine scheduler, a Kotlin `CoroutineScope`, a C++ executor, or a framework dispatcher. **The bindings library owns no threads, queues, or retry policy of its own either** — for routed send or for request.
- It does not add separate public APIs to the bindings contract for a coroutine-only recv, a virtual-thread-only recv, or a framework-dispatcher-only submit.
- Once a builder has been submitted, it cannot be submitted again. Where the language offers an ownership type or typestate, this is blocked by the type system; otherwise it is blocked by a runtime state check.
- Submit failure and reply failure are delivered through the return type's failure representation or that language's idiomatic exception.
- A request builder does not pair a callback, or another blocking-compatible terminal, with the canonical suspension terminal. This rule does not apply to the routed send builder — its canonical terminal is itself synchronous (see below).

## Routed send synchronous submit contract

A DEALER/ROUTER **send** (a one-way routed transmission, not a request) invokes
the language's canonical **synchronous** terminal on the builder returned by the
same operation entrypoint. It adds no separate name such as `send_async` or
`sendCoroutine`. This terminal wraps the Core send directly, and a binding
keeps no park queue, WRITABLE-callback retry, deadline timer, or dispatcher
thread for admission.

| Aspect | bindings completion surface |
|---|---|
| Execution meaning | A synchronous call that wraps the Core send directly; it returns or throws immediately |
| Blocking mode | Waits inside Core and resumes on Core's signal (no binding-side wait) |
| `SNDTIMEO` | Used by Core as the wait bound |
| `DONTWAIT` | Core returns `EAGAIN` immediately (surfaced as the language's `BACKPRESSURED`) |
| Submit flags | May be accepted as a language-idiomatic option argument or builder stage |
| Failure | The language's idiomatic exception or `Result`/`error` return — the same shape as raw reply's contract today |
| Backpressure policy | Owned by the application. The binding does not retry |

## Request asynchronous completion

A DEALER/ROUTER **request** invokes the language's canonical suspension
terminal on the builder returned by the same operation entrypoint. It adds no
separate name such as `requestCoroutine`, `request_async`, or `submit_async`.
Core drives reply completion: a reply handler callback completes the
suspension, and resumption happens in the context where that completion
occurred. Request timeout is already Core-owned (`ZLINK_REQUEST_TIMED_OUT`). A
binding keeps no retry queue or dedicated thread for this completion surface.

| Aspect | bindings completion surface |
|---|---|
| Execution meaning | Returns a language-native suspension object or completion channel |
| Reply delivery | The suspension's success value or channel completion |
| Submit flags | The managed request terminal does not accept them |
| Timeout | The builder's `timeout(...)` stage; expiry is signaled by Core as `ZLINK_REQUEST_TIMED_OUT` |
| Submit failure | A failed task/future/promise, error result, or exception |
| Reply failure | Failure of the same suspension result |
| Resumption context | The context where Core delivered the completion (the reply handler callback). Further execution-model wiring belongs to the framework |

## Routed send and request names and return types

The table below is the bindings public-surface baseline for HWM-managed
DEALER/ROUTER **routed send** and **request**. It does not apply to raw reply
or one-shot PAIR, PUB, and STREAM submit. Note that routed send takes the same
synchronous shape as raw reply, while request keeps the language-native
suspension surface. A framework may wrap this surface to provide coroutines or
another execution model, but it must not grow the bindings public API's names.

| Binding | Routed send terminal | Send return/failure | Request terminal | Request return type or completion representation |
|---|---|---|---|---|
| C | Not applicable | Not applicable | Not applicable | The C ABI does not apply builder policy — it follows the functional contract in `core/include/zlink.h`. |
| C++ | `submit()` | `void`; throws `submit_error_t` on failure | `async()` | Move-only `async_result_t<T>`. `co_await op.async()` is canonical. No `get`/`wait`/callback terminal is provided; drop and `cancel()` request cancellation. |
| Java | `submit()` | `void`; throws `ZlinkSubmitException` on failure | `submit()` | `CompletionStage<T>`. No blocking `await()` or callback compatibility terminal is paired with it. |
| .NET | `Submit()` | `void`; throws `ZlinkSubmitException` on failure | `Async(...)` | `Task<T>`, `ValueTask<T>`, `Task`, or `ValueTask`. Only the terminal method follows .NET naming convention. |
| Node | `submit()` | `void`; throws `SubmitError` on failure | `submit()` | `Promise<T>` or `Promise<void>`. Users suspend with `await op.submit()`. |
| Python | `submit()` | `None`; raises `SubmitError` on failure | `submit()` | Awaitable coroutine object. It does not block the event loop. |
| Kotlin | `submit()` | Same as Java (`void`, exception) | `submit()` | Java `CompletionStage<T>`. Canonical Kotlin usage is `submit().await()`. |
| Go | `Submit(ctx)` | `error` (`nil` on success) | `Submit(ctx)` | Completion channel. `context.Context` is passed at execution time; no callback or `SubmitAsync` terminal is added. |
| Rust | `submit()` | `Result<(), SubmitError>` | `submit()` | Returns a runtime-independent `Future<Output = Result<Vec<Message>, ZlinkError>>`. Canonical usage is `submit().await?`; dropping the future requests cancellation. |

## Synchronous one-shot raw reply

A raw ROUTER/`Received` reply builder is not an HWM-managed routed send/request.
Its terminal submits a terminal reply or error reply to the HWM-free completion
lane with one native call and finishes synchronously. HWM backpressure is not a
reply result. `NOT_CONNECTED`, `TERMINATED`, `INVALID_ARGUMENT`, or another
non-HWM submit failure is delivered immediately as the language's
`SubmitError`.

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
`reply(...).await()` below. Routed send's synchronous `submit()` follows the
same shape, though it is a distinct builder for a distinct operation (a
one-way send).

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
own execution model. The conversion code is owned by the framework, or by
the framework's language wrapper. **Bindings provides no executor** — a
framework that wants an awaitable routed send wraps the synchronous
`submit()` in its own executor (a thread pool, event-loop offload, and so
on). Request already returns a language-native suspension, so it needs no
such wrapping.

### Java framework

- The Java framework wires the `CompletionStage` it gets from a managed
  request's `submit()` into the handler executor, virtual threads, and the
  timeout policy.
- For an awaitable routed send, it wraps the synchronous `submit()` in its own
  executor (for example, calling it from a virtual thread).
- Even when using virtual threads, it does not add a virtual-thread-only recv or submit API to bindings.

### Kotlin framework

- The Kotlin wrapper does not call Java's `await()`.
- The Kotlin wrapper gets the `CompletionStage` from a Java managed request
  builder's `submit()`, then connects it to coroutine suspension via
  `kotlinx-coroutines-jdk8`'s `await()`.
- For routed send, the Kotlin wrapper wraps the synchronous `submit()` as a
  suspend function, calling it from its own dispatcher (such as
  `Dispatchers.IO`).
- Kotlin user code and Kotlin samples do not call Java's `submit()` directly — they use Kotlin wrappers such as `connect().await()`, `request(...).await<T>()`, and `waitFor<T>(...).await()`.
- The Kotlin framework owns the coroutine scope, dispatcher, and cancellation handling. The bindings library does not create a scope or dispatcher.

### C++ framework

- C++ bindings provides move-only `async_result_t<T>` through a managed
  request's `async()`, and user and framework coroutines `co_await` it
  directly.
- A standalone coroutine may resume in the context where its completion
  occurred (such as a Core reply handler callback). This does not permit the
  binding to create its own threads, dispatchers, or schedulers for completion
  or resumption — execution resources belong to the framework. The Framework
  `task_t` promise uses an optional continuation-scheduler hook to hand off
  only the current serial turn and ambient context.
- Routed send is a synchronous `submit()`. A C++ framework that wants an
  awaitable routed send calls `submit()` on its own executor (such as a thread
  pool) and wraps the result in a `task_t` — bindings does not create that
  executor.
- The bindings public API does not add a coroutine-only `request_async` or `request_coroutine`, a framework executor argument, or a framework dispatcher argument.

### Other language frameworks

- The .NET framework `await`s the `Task`/`ValueTask` returned by a managed
  request as-is. When routed send needs to be awaitable, it wraps the
  synchronous `Submit()` in a framework-owned executor such as `Task.Run(...)`.
- The Node framework `await`s the `Promise` returned by a managed request
  according to its event loop policy. Routed send is a synchronous `submit()`,
  so a Node framework that wants non-blocking execution wraps it with its own
  worker-offload policy.
- The Python framework awaits a managed request's `submit()` coroutine object
  directly. For an awaitable routed send, it wraps the synchronous `submit()`
  in a framework-owned execution path such as `loop.run_in_executor(...)`.
- The Rust framework polls a managed request's runtime-independent `submit()`
  Future on its own executor. Routed send is a synchronous `submit()`, so when
  asynchronous execution is needed, the framework wraps it with something like
  `spawn_blocking`.

This way, bindings keeps its responsibility as a C API wrapper, while each
framework can independently provide coroutine support that fits its own
execution model. Bindings owns no thread in either case.
