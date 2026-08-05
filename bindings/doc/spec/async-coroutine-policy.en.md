---
title: "Bindings Async Completion Surface Policy"
---

<!-- bindings-nav:start -->
[Spec index](README.md) | [Previous: Overview](README.md) | [Next: C](c/README.md)
<!-- bindings-nav:end -->

# Bindings Async Completion Surface Policy

> **What this chapter defines** — the naming and return-type policy every
> language binding except C must follow when it exposes async completion.

This document defines the name and return type each language binding,
except C, must use to expose async completion. The bindings library
provides a per-language completion boundary on top of the core C API.
Coroutine execution, virtual thread execution, event loop wiring, and
handler dispatcher wiring are the framework's responsibility.

The bindings library does not provide a way to directly run or resume a
coroutine. It therefore does not put coroutine-only public APIs — such as
C++ `co_await` support, Rust's `async fn submit_async`, or a Python method
that returns a coroutine — into the bindings contract.

| Section | Covers |
|---|---|
| [Common principles](#common-principles) | Shared rules for operation start-point names, builders, and submit-failure representation |
| [Request completion styles](#request-completion-styles) | A surface comparison of returning a completion object versus callback-based submit |
| [Per-language names and return types](#per-language-names-and-return-types) | Each language's builder terminal method and return type |
| [How frameworks attach coroutines](#how-frameworks-attach-coroutines) | How each language's framework turns the completion boundary into its own execution model |

## Common principles

- The bindings public API keeps a single name for an operation's starting point. It does not grow that name with completion style or flag combinations, as in `requestAsync`, `request_callback`, `sendNoWait`, or `publishWithFlags`.
- `send`, `request`, `reply`, `publish`, Actor location operations, and Actor session attach operations all return an operation builder.
- Payload, timeout, callback, and completion style are expressed at the builder stage, not as arguments to the operation's starting point.
- The bindings library does not own a coroutine scheduler, a Kotlin `CoroutineScope`, a C++ executor, or a framework dispatcher.
- It does not add separate public APIs to the bindings contract for a coroutine-only recv, a virtual-thread-only recv, or a framework-dispatcher-only submit.
- Once a builder has been submitted, it cannot be submitted again. Where the language offers an ownership type or typestate, this is blocked by the type system; otherwise it is blocked by a runtime state check.
- Submit failure and reply failure are delivered through the return type's failure representation, a callback result, or that language's idiomatic exception.

## Request completion styles

`request` chooses its completion style on the `RequestOp` builder returned
by the same `request` entrypoint. It does not create a separate
coroutine-execution method for coroutines, such as `requestCoroutine`,
`submitAsync`, or `submit_async`.

| Aspect | bindings completion surface | callback-based submit |
|---|---|---|
| Execution meaning | Returns the language's default completion object, or waits for completion on the current thread | Delivers completion via a callback |
| Reply delivery | The completion object's success value, or a blocking return value | Callback argument |
| Submit flags | Not accepted | May accept flags such as `DONTWAIT` when needed |
| Timeout | The builder's `timeout(...)` stage | The builder's `timeout(...)` stage |
| Submit failure | A failed future, rejected promise, error result, or exception | The language's submit-failure representation |
| Reply failure | A failed future, rejected promise, error result, or exception | Callback result |

## Per-language names and return types

The table below is the bindings library's public-surface baseline. A
framework may wrap this surface to provide coroutines or another execution
model, but it must not grow the bindings public API's names.

| Binding | Builder terminal method | Return type or completion representation | Callback surface | Notes |
|---|---|---|---|---|
| C | Not applicable | Not applicable | A C callback function plus flags | The C ABI does not apply builder policy — it follows the functional contract in `core/include/zlink.h`. |
| C++ | `async()` or `submit(callback)` | `async()` returns `async_result_t<T>`. `async_result_t<T>` supports `wait()`, `wait_for(...)`, `wait_until(...)`, and `get()`. | `submit(callback)` | The bindings library targets C++20. `async_result_t<T>` does not support `co_await`. |
| Java | `submit()` / `await()` / `submit(callback)` | `submit()` returns `CompletionStage<T>`; `await()` waits for completion on the current thread and returns `T` or `void`. | `submit(callback)` | `await()` is not a new network semantic — it is a blocking adapter that waits for `submit()`'s result. |
| .NET | `Async(...)` | `Task<T>`, `ValueTask<T>`, `Task`, or `ValueTask` | A separate submit stage on builders that need a callback | Only the terminal method's name follows .NET convention as `Async`. The starting-point name is not grown, as in `RequestAsync`. |
| Node | `submit(...)` | `Promise<T>` or `Promise<void>` | A separate submit stage on builders that need a callback | A JavaScript user can wait with `await op.submit()`, but bindings does not create a separate coroutine scheduler. |
| Python | `submit(callback)` | Callback completion | `submit(callback)` | bindings does not provide `submit_async()` or a method that returns a coroutine object. |
| Go | `Submit(ctx)`, `Submit(ctx, callback)`, or `SubmitAsync(ctx)` | The send family returns `(bool, error)`, the reply family returns `error`, and the request family completes via callback or the `<-chan RequestReplyCompletion` that `SubmitAsync(ctx)` returns | `Submit(ctx, callback)` | `context.Context` is passed at execution time, not at the operation's starting point. The request builder offers both a callback path (`Submit`) and a channel path (`SubmitAsync`). |
| Rust | `submit(callback)` or immediate submit | Callback completion or `Result<_, SubmitError>` | `submit(callback)` | A typestate builder uses the type system, as far as possible, to enforce the required-payload condition and block duplicate submission. bindings does not provide `async fn submit_async`. |

## How frameworks attach coroutines

A framework converts the completion boundary bindings provides into its
own execution model. The conversion code is owned by the framework, or by
the framework's language wrapper.

### Java framework

- The Java framework wires the `CompletionStage` it gets from `submit()` into the handler executor, virtual threads, and the timeout policy.
- Even when using virtual threads, it does not add a virtual-thread-only recv or submit API to bindings.
- `await()` is a blocking adapter meant to be called on a virtual thread. When a virtual thread blocks, the JVM parks that thread and frees the carrier (platform) thread for other work, so — unlike blocking a platform thread — it does not waste a thread. Because of this property, sample code or code inside a virtual thread can safely use `await()` for a sequential flow instead of `submit()`'s `CompletionStage`. The framework's own default async path is based on `submit()`.

### Kotlin framework

- The Kotlin wrapper does not call Java's `await()`.
- The Kotlin wrapper gets the `CompletionStage` from the Java builder's `submit()`, then connects it to coroutine suspension via `kotlinx-coroutines-jdk8`'s `await()`.
- Kotlin user code and Kotlin samples do not call Java's `submit()` directly — they use Kotlin wrappers such as `connect().await()`, `request(...).await<T>()`, and `waitFor<T>(...).await()`.
- The Kotlin framework owns the coroutine scope, dispatcher, and cancellation handling. The bindings library does not create a scope or dispatcher.

### C++ framework

- C++ bindings provides the `async_result_t<T>` completion object via `async()`, and callback completion via `submit(callback)`.
- When the C++ framework supports coroutines, the framework wraps the `async_result_t<T>` or callback completion in its own `task_t<T>`, then `co_await`s it inside a framework coroutine.
- The framework owns the handler coroutine executor, handler dispatch, cancellation, and resume-thread policy.
- The bindings public API does not add a `co_await` awaiter, a coroutine-only `request_async` or `request_coroutine`, a framework executor argument, or a framework dispatcher argument.

### Other language frameworks

- The .NET framework `await`s bindings' `Task`/`ValueTask` return as-is.
- The Node framework `await`s bindings' `Promise` return according to its event loop policy.
- The Python framework converts bindings' callback completion into an `asyncio.Future` or a framework task.
- The Rust framework converts bindings' callback completion into a runtime-specific `Future`, or connects it to a framework channel.

This way, bindings keeps its responsibility as a C API wrapper, while each
framework can independently provide coroutine support that fits its own
execution model.
