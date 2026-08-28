# Async Execution Model and Completion Surface Terminology

> This is a **reference document** that defines, in one place, the
> **asynchronous/synchronous, execution model, and completion surface** terms
> used across the bindings spec. It does not create any contract — other spec
> documents reuse these terms verbatim. When terminology in code or another
> document disagrees, align it to this document.

## 1. Distinguish the two axes

To avoid confusion, separate two different questions.

| Axis | Question | Values |
|---|---|---|
| **Completion nature** | **When** does the call hand back completion | **now** (synchronous) / **later** (asynchronous) |
| **Execution model** | **What** absorbs the wait | OS thread / virtual thread·goroutine / coroutine / event loop |

Completion nature is a property of the **API terminal**; the execution model is
a property of **where the user calls that API**. (Completion nature itself splits
further into two axes — `synchronous/asynchronous` and `blocking/non-blocking` —
see §2.) For example, an asynchronous API can be `await`ed from a coroutine or
received via callback on an event loop; a synchronous API can be called blocking
on an OS thread or on a virtual thread.

## 2. Completion nature: synchronous/asynchronous and blocking/non-blocking are different axes

The two pairs are often conflated but answer **different questions**. Keeping
them apart is what makes the terminology precise.

| Axis | Question | Values |
|---|---|---|
| **synchronous / asynchronous** | **When** is completion delivered | synchronous: completion is settled by the time the call returns (received in place) · asynchronous: the call only starts the work and completion is delivered later via an awaitable/callback |
| **blocking / non-blocking** | Does the **thread stop** while waiting | blocking: if it cannot proceed, the calling thread is parked and waits · non-blocking: if it cannot proceed, it returns immediately with a failure (no wait). `DONTWAIT` lives here |

The two axes **combine.** Placing zlink's send terminals on a grid makes the
relationship clear.

| | **blocking** (park if it cannot proceed) | **non-blocking** (fail immediately if it cannot proceed) |
|---|---|---|
| **synchronous** (completion in place) | `submit()` (`NONE`) — park until admitted, then result | `submit(DONTWAIT)` — immediate `BACKPRESSURED`/`EAGAIN` |
| **asynchronous** (completion later) | (unused — we do not block *and* defer) | `async()`/awaitable — return immediately, completion notified later |

- Therefore a **sync terminal selects blocking/non-blocking via a flag**
  (`NONE` = blocking, `DONTWAIT` = non-blocking). An async terminal is
  **always non-blocking** (returns as soon as it starts) and only the completion
  is later.
- Native `zlink_send_async` always sits in the "asynchronous·non-blocking" cell,
  so it cannot express the "synchronous·non-blocking" cell where `DONTWAIT` is
  meaningful — which is exactly why a separate sync(+flags) terminal is needed
  (see the [routed submit policy](async-coroutine-policy.en.md)).

- **"Asynchronous" is the umbrella term.** It covers not only coroutines but also
  **submitting work to a thread pool/executor and being notified of completion
  separately via a Future** and the like. If "return now without waiting for
  final completion, notify completion later" holds, it is asynchronous no matter
  how it is implemented.
- However, **calling a blocking API from a virtual thread/goroutine is not itself
  asynchronous** — that call is still synchronous (blocking); the runtime merely
  absorbs the wait cheaply (§3). Being a lightweight execution unit does not by
  itself make an API asynchronous.
- We do not use "coroutine" as the umbrella term — virtual threads, goroutines,
  and event loops are not coroutines (§3).

## 3. Execution model

This is **how** the asynchrony is implemented. The four below are **not mutually
exclusive categories but combinable implementation elements** — precisely, they
are combinations of two sub-axes: `execution unit (thread / coroutine)` and
`completion dispatch (event loop / executor / direct continuation)`. The names
below are the combinations commonly used in practice.

| Execution model | How the wait is absorbed | Code shape | Examples |
|---|---|---|---|
| **OS thread (platform thread)** | kernel parks the thread | blocking call | C, C++ `std::thread`, Java platform thread |
| **Lightweight execution unit (virtual thread / goroutine)** | runtime parks a lightweight unit | blocking call (looks synchronous) | Java virtual thread, Go goroutine |
| **Coroutine** | the function suspends/resumes (usually a stackless state machine) | explicit `await`/`suspend` | Kotlin `suspend`, Python `async def`, C++20 coroutine, Rust `async`/`Future` |
| **Event loop** | callbacks/Promises are queued, single-threaded loop | `await` or callback | Node.js, Python asyncio, browser JS |

Default execution model per language (bindings targeted by this project):
> **Note.** As of 0.14.0 the send family (PAIR/routed/`Received.send()`) has both an async and a sync(+flags) terminal in every binding (owned by the [normative table](async-coroutine-policy.en.md)). The "Completion surface shape" below shows only each language's default/representative surface — Node/Rust/Python have a sync terminal too.


| Language | Default execution model | Completion surface shape | Notes |
|---|---|---|---|
| **C** | OS thread | blocking | core C API |
| **C++** | OS thread + coroutine | blocking `submit()` / `co_await async()` | C++20 coroutine optional |
| **.NET** | coroutine (async/await) + thread pool | send: async `Async(ct)`→`Task` · sync `Submit(SendFlags)`→`void` | send has both async/sync terminals (0.14.0). **request stays async-only** |
| **Java** | no coroutine (`CompletionStage`) · recv on OS/virtual thread | send: async `submit()`→`CompletionStage` · sync `submit(SendFlags)`→`void` | send gains a sync overload (0.14.0); request stays async-only; virtual thread is **optional** |
| **Kotlin** | coroutine | `suspend` / `await()` | `kotlinx-coroutines` |
| **Node** | event loop | `Promise` / `await` | single-threaded |
| **Python** | coroutine + event loop (asyncio) · OS thread | `await` coroutine object / blocking | GIL, `async def` |
| **Rust** | coroutine (async) | `.await` `Future` | runtime-agnostic |
| **Go** | virtual thread (goroutine) | blocking `Submit(ctx)` | completion observed via channel |

Subtle distinctions:

- **Virtual thread vs coroutine** — both are lightweight concurrency, but a
  virtual thread runs **blocking code as-is** while the runtime parks/unparks it
  behind the scenes (asynchrony is not visible in the code). A coroutine makes
  `suspend`/`await` **explicit** (visible in the code).
- **Coroutine vs event loop** — orthogonal (execution unit vs dispatch mechanism).
  They are used together: Python asyncio is a **coroutine running on top of an
  event loop**, and Node **processes `Promise` continuations on the event loop**
  (JS standard terminology is `async function`/`Promise`, and one does not call
  Node's execution as a whole a "coroutine"). In both cases the event loop is the
  underlying dispatch engine.

## 4. Completion surface (terminal) and return values

Bindings provide a per-language **completion surface** over the core C API. This
document calls **a method that finishes an operation and returns a result or a
completion handle a `terminal`** (a different meaning from the `terminal
operation` of Java Stream/Rx; here we use this document's definition). The
standard names for the values a terminal returns are:

| Return value | Nature | Language |
|---|---|---|
| **awaitable** (async completion value) | this document's umbrella for values holding an async completion | umbrella for all below |
| `Task` / `ValueTask` | .NET awaitable | .NET |
| `CompletionStage` / `CompletableFuture` | Java completion handle (no language `await`; observed via chaining/`.get()`) | Java |
| `Promise` | JS awaitable | Node |
| coroutine object | `await`-able Python awaitable | Python |
| `Future` | Rust awaitable | Rust |
| `async_result_t<T>` | C++ move-only awaitable | C++ (`async()`) |
| completion channel | Go completion handle (observed via receive/`select`) | Go (request) |

- The document groups these under **awaitable**, but strictly it is an umbrella
  over both **values that implement a language `await` protocol directly** (like
  `Task`, a Python coroutine object, a C++ awaiter) and **handles whose
  completion is observed via chaining/receive** (like Java `CompletionStage`, a
  Go channel).
- A synchronous terminal's return is not an awaitable — it is an idiomatic
  immediate value (`void`/`bool`/`None`/`Result`/`error`) or an exception.

## 5. Standard terms used in the docs (summary)

| Concept | Standard term in the docs | Term to avoid |
|---|---|---|
| Receiving completion later (umbrella) | **asynchronous** | "coroutine" (as the umbrella) |
| Receiving completion during the call | **synchronous** | — |
| Thread parked while waiting | **blocking** | — |
| Immediate failure return if it cannot proceed | **non-blocking** | — |
| Umbrella for how asynchrony is implemented | **execution model** | — |
| Its kinds (not exclusive; combinable) | **OS thread / virtual thread·goroutine / coroutine / event loop** | — |
| Value holding an async completion | **awaitable** | — |
| An API's completion surface | **terminal (completion surface)** | — |

- **Use "coroutine" only when naming an actual coroutine language** (Kotlin
  `suspend`, Python `async def`, C++20, Rust `async`). Use "asynchronous" or
  "execution model" when covering everything.
- When listing several execution models, write them as "thread (including virtual
  threads)·coroutine·event loop".
