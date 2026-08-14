---
title: "Bindings Async Completion Surface Policy"
---

<!-- bindings-nav:start -->
[Spec index](README.en.md) | [Previous: Overview](README.en.md) | [Next: C](c/README.en.md)
<!-- bindings-nav:end -->

# Bindings Async Completion Surface Policy

> **What this chapter defines** — the naming and return-type policy every
> language binding except C must follow for asynchronous completion of
> HWM-managed routed send/request and synchronous one-shot completion of raw
> reply.

This document defines the name and return type each language binding, except
C, must use for asynchronous completion of HWM-managed routed send/request and
synchronous completion of raw ROUTER/`Received` reply. The bindings library
provides a per-language completion boundary on top of the core C API. Coroutine
execution, virtual thread execution, event loop wiring, and handler dispatcher
wiring are the framework's responsibility.

The bindings library owns no coroutine executor or scheduler. It still
provides a language-native suspension object: Python builder `submit()`
returns an awaitable coroutine object, and Rust builder `submit()` returns a
runtime-independent `Future`. Neither is a new operation starting point or a
framework executor.

| Section | Covers |
|---|---|
| [Common principles](#common-principles) | Shared rules for operation start-point names, builders, and submit-failure representation |
| [Managed routed completion](#managed-routed-completion) | The language-native suspension surface for HWM-managed send/request |
| [Managed routed names and return types](#managed-routed-names-and-return-types) | Each managed builder's terminal method and return type |
| [Synchronous one-shot raw reply](#synchronous-one-shot-raw-reply) | The seven bindings' reply terminals and immediate failure contract |
| [Framework typed Session reply](#framework-typed-session-reply) | The separate awaitable Framework contract |
| [How frameworks attach coroutines](#how-frameworks-attach-coroutines) | How each language's framework turns the completion boundary into its own execution model |

## Common principles

- The bindings public API keeps a single name for an operation's starting point. It does not grow that name with completion style or flag combinations, as in `requestAsync`, `request_callback`, `sendNoWait`, or `publishWithFlags`.
- `send`, `request`, `reply`, `publish`, Actor location operations, and Actor session attach operations all return an operation builder.
- Payload and timeout are expressed at the builder stage, not as arguments to the operation's starting point.
- The bindings library does not own a coroutine scheduler, a Kotlin `CoroutineScope`, a C++ executor, or a framework dispatcher.
- It does not add separate public APIs to the bindings contract for a coroutine-only recv, a virtual-thread-only recv, or a framework-dispatcher-only submit.
- Once a builder has been submitted, it cannot be submitted again. Where the language offers an ownership type or typestate, this is blocked by the type system; otherwise it is blocked by a runtime state check.
- Submit failure and reply failure are delivered through the return type's failure representation or that language's idiomatic exception.

## Managed routed completion

DEALER/ROUTER routed send and request invoke the language's canonical terminal
on the builder returned by the same operation entrypoint. They add no separate
name such as `requestCoroutine`, `request_async`, or `submit_async`. An
HWM-managed routed builder does not also expose callback or blocking
compatibility terminals. Core C callbacks, handler registration, and one-shot
immediate submit outside the routed builder are not removed by this rule.

| Aspect | bindings completion surface |
|---|---|
| Execution meaning | Returns a language-native suspension object or completion channel |
| Reply delivery | The suspension's success value or channel completion |
| Submit flags | A managed routed terminal does not accept them |
| Timeout | The builder's `timeout(...)` stage |
| Submit failure | A failed task/future/promise, error result, or exception |
| Reply failure | Failure of the same suspension result |

## Managed routed names and return types

The table below is the bindings public-surface baseline only for HWM-managed
DEALER/ROUTER routed send/request. It does not apply to raw reply or one-shot
PAIR, PUB, and STREAM submit. A framework may wrap this surface to provide
coroutines or another execution model, but it must not grow the bindings public
API's names.

| Binding | Builder terminal method | Return type or completion representation | Notes |
|---|---|---|---|
| C | Not applicable | Not applicable | The C ABI does not apply builder policy — it follows the functional contract in `core/include/zlink.h`. |
| C++ | `async()` | Move-only `async_result_t<T>` | `co_await op.async()` is canonical. No `get`/`wait`/callback terminal is provided; drop and `cancel()` request admission cancellation. |
| Java | `submit()` | `CompletionStage<T>` | No blocking `await()` or callback compatibility terminal is paired with it. |
| .NET | `Async(...)` | `Task<T>`, `ValueTask<T>`, `Task`, or `ValueTask` | Only the terminal method follows .NET naming convention. |
| Node | `submit()` | `Promise<T>` or `Promise<void>` | Users suspend with `await op.submit()`. |
| Python | `submit()` | Awaitable coroutine object | It does not block the event loop. |
| Kotlin | `submit()` | Java `CompletionStage<T>` | Canonical Kotlin usage is `submit().await()`. |
| Go | `Submit(ctx)` | Completion channel | `context.Context` is passed at execution time; no callback or `SubmitAsync` terminal is added. |
| Rust | `submit()` | Send returns `Future<Output = Result<(), SubmitError>>`; request returns `Future<Output = Result<Vec<Message>, ZlinkError>>` | The futures are runtime-independent. Canonical usage is `submit().await?`; dropping the Future requests cancellation. |

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
own execution model. The conversion code is owned by the framework, or by
the framework's language wrapper.

### Java framework

- The Java framework wires the `CompletionStage` it gets from managed routed
  send/request `submit()` into the handler executor, virtual threads, and the
  timeout policy.
- Even when using virtual threads, it does not add a virtual-thread-only recv or submit API to bindings.

### Kotlin framework

- The Kotlin wrapper does not call Java's `await()`.
- The Kotlin wrapper gets the `CompletionStage` from a Java managed builder's
  `submit()`, then connects it to coroutine suspension via
  `kotlinx-coroutines-jdk8`'s `await()`.
- Kotlin user code and Kotlin samples do not call Java's `submit()` directly — they use Kotlin wrappers such as `connect().await()`, `request(...).await<T>()`, and `waitFor<T>(...).await()`.
- The Kotlin framework owns the coroutine scope, dispatcher, and cancellation handling. The bindings library does not create a scope or dispatcher.

### C++ framework

- C++ bindings provides move-only `async_result_t<T>` through managed routed
  send/request `async()`, and user and framework coroutines `co_await` it
  directly.
- A standalone coroutine may resume on the binding completion thread. The Framework `task_t` promise uses an optional continuation-scheduler hook to hand off only the current serial turn and ambient context. Admission retry remains binding-owned.
- The bindings public API does not add a coroutine-only `request_async` or `request_coroutine`, a framework executor argument, or a framework dispatcher argument.

### Other language frameworks

- The .NET framework `await`s the `Task`/`ValueTask` returned by managed routed
  send/request as-is.
- The Node framework `await`s the `Promise` returned by managed routed
  send/request according to its event loop policy.
- The Python framework awaits the managed routed send/request `submit()`
  coroutine object directly.
- The Rust framework polls the managed routed send/request runtime-independent
  `submit()` Future on its own executor.

This way, bindings keeps its responsibility as a C API wrapper, while each
framework can independently provide coroutine support that fits its own
execution model.
