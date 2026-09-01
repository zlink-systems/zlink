# Async Execution Model and Completion Surface Terminology

> This document defines the meanings of synchronous and asynchronous, blocking and non-blocking,
> execution environments, awaitables, and completion progress throughout the binding specifications.
> Each binding specification and the [async completion surface policy](async-coroutine-policy.en.md)
> own the public signatures for that language.

## 1. Completion and execution-environment axes

When an API returns its result and the environment in which the API runs are separate decisions.

| Axis | Question | Values |
|---|---|---|
| Completion time | When does the call return its final result? | Synchronous or asynchronous |
| Waiting behavior | Does the calling thread stop when the operation cannot progress? | Blocking or non-blocking |
| Execution environment | What handles waits and continuations? | OS thread, virtual thread/goroutine, coroutine, or event loop |

A synchronous terminal has determined the final result when the call returns. An asynchronous terminal
returns an awaitable first and determines the result later. A blocking call makes the calling thread wait
until it can progress; a non-blocking call does not wait.

High-level send/request awaitable terminals use native `DONTWAIT` submission and completion drain.
Blocking send/request terminals use native `NONE`. Go exposes only one public
`Submit(context.Context)` terminal but internally waits for a completion after native `DONTWAIT`
submission. A publish operation's `DONTWAIT` choice is part of its lossy/NODROP contract and is separate
from the send/request completion model.

## 2. Execution environments

The execution environment does not change the completion semantics of a public terminal. Different
languages can consume the same kind of awaitable in different execution environments.

| Execution environment | How it handles a wait | Representative bindings |
|---|---|---|
| OS thread | The kernel makes the thread wait. | C, C++ blocking terminal, Java platform thread |
| Virtual thread/goroutine | The language runtime waits a lightweight execution unit. | Java virtual thread, Go |
| Coroutine | The function suspends and resumes through a continuation. | C++, Kotlin, Python, Rust |
| Event loop | Runs Promise or coroutine continuations from a queue. | Node, Python asyncio |

Calling a blocking API from a virtual thread or goroutine does not make the API asynchronous. Conversely,
waiting for an awaitable from an OS thread does not make the public terminal synchronous. Use
`coroutine` only for an actual coroutine facility, not as an umbrella term for every asynchronous
execution method.

## 3. Awaitables and synchronous results

A method that finishes an operation and returns either its result or a completion handle is called a
terminal. Awaitable is the documentation term for the language-specific values that carry asynchronous
completion.

| Binding | Awaitable or completion-wait form |
|---|---|
| C++ | `async_result_t<T>` |
| .NET | `Task` |
| Java/Kotlin | `CompletionStage<T>`; Kotlin uses the Java contract. |
| Node | `Promise<T>` |
| Python | `Awaitable[T]` |
| Rust | `Future<Output = T>` |
| Go | The goroutine executing `Submit(context.Context)` waits for an internal completion. |

A synchronous terminal's return value is not an awaitable. It conveys the result determined within the
call as `void`, a collection, `Result`, `error`, or a language-specific exception.

## 4. Pollers and completion drain

In C, `ZLINK_POLLCOMPLETION` is non-consuming level readiness indicating that the next
`zlink_completion_recv()` can return one record. `zlink_poller_wait()` does not consume a completion.
The caller repeats `zlink_completion_recv(..., ZLINK_RECV_FLAGS_DONTWAIT)` for each ready socket through
`ZLINK_RECV_NO_DATA`.

A high-level binding that does not expose raw completions has exactly one drain owner per socket. The
binding runtime owns a socket that is not registered with a public poller for `PollCompletion`.
Registration transfers ownership atomically to the thread calling `wait()`; removal or clearing the
completion bit transfers it back to the runtime owner. The two owners never drain the same queue
concurrently.

High-level `PollCompletion` is a completion progress event returned after the binding takes at least one
record from the native queue and either completes a live waiter or cleans up detached state. The queue
can already be empty when it returns, and it does not guarantee a new public-awaitable state change. It
does not consume application DATA even when `POLLIN` is ready at the same time.

While a public poller is the owner, completion-backed terminals depend on drain by that poller's
`wait()`. Using a blocking request or Go `Submit(context.Context)` at the same time requires another
thread or goroutine to keep executing the `wait()` loop. Do not invoke `wait()` and a blocking terminal
serially on the same execution thread. The binding neither takes ownership for a blocking terminal nor
creates a separate drain thread.

## 5. Provisional registry and completion join

Before a completion-backed terminal invokes native `FINAL`, it registers provisional language operation
state in a socket-local registry under a stable `user_context`. It cannot register by completion ID first
because the ID is not known until the native call returns.

Submit and completion join in the following order.

1. If native submit fails, the binding observes ID `0`, removes the provisional state, and completes the
   terminal with the submit error. This path creates no completion.
2. If a successful send returns ID `0`, the binding completes the terminal with inline success and
   removes the state.
3. For a successful request or a nonzero send ID, the binding atomically publishes the submit outcome,
   ID, and Core ownership into the provisional state.
4. If the completion is drained before submit returns, the binding locates the state by `user_context`
   and captures the result and native aggregate ownership.
5. Only after submit publication and completion capture have both finished does the binding complete the
   public terminal exactly once and remove the registry entry once.

A `wait()` that processes a pre-return completion also does not return `PollCompletion` progress until
publication and capture join and settlement or cleanup completes. Blocking send and reply do not create
completions and therefore do not use the provisional registry.

## 6. Caller wait cancellation

High-level cancellation cancels the caller's wait on a language terminal, not the Core operation.

- If cancellation is decided before the native call, the binding neither calls Core nor leaves
  provisional state.
- If cancellation races with submit failure during the native call, the binding records only the
  cancellation claim until native returns. An exact submit error takes precedence when submit fails.
- If cancellation or Future drop is decided first after successful submit, the binding completes only
  the live waiter once as canceled or detached. Registry state remains until a late completion or
  socket/context lifecycle cleanup.
- A late completion releases the native payload and state but does not complete the public waiter again.
  A successful ID `0` send whose cancellation was decided first is also not completed again as success.
- If socket close or context termination makes completion unavailable, the binding completes only the
  live waiter with the shutdown or terminated error and removes all registry state.

For a non-OK request completion, the high-level binding exposes only a typed request error. It does not
move the error-reply payload into a language message collection or error property, and closes the native
completion exactly once before completing the user-visible error. In Go, a typed request error is
`(nil, error)`; when Context cancellation is decided first, it is `(nil, ctx.Err())`.

## 7. Implementation and contract-test verification requirements

Verify the following using only public terminals, public poller events, and language-specific
cancellation results. Each item maps to one contract test.

**Completion and drain**

- After a C poller returns `ZLINK_POLLCOMPLETION`, raw drain can receive each queued record once, and
  poller wait itself does not consume a record.
- When a high-level poller returns `PollCompletion`, it has completely processed at least one native
  completion. The event does not guarantee a successful raw receive or a new public-waiter state change.
- When another thread or goroutine executes `wait()` under public poller ownership, blocking requests
  and Go `Submit(context.Context)` progress. Without `wait()`, the binding does not take the queue
  arbitrarily.

**Submit and completion races**

- Native submit failure completes once with the exact submit error and creates no completion progress.
- A successful ID `0` send succeeds once. Even when a nonzero completion is drained before submit
  returns, the public terminal completes only once after joining submit publication.
- A successful request joins a nonzero completion and removes the registry entry once after result
  conversion or cleanup.

**Cancellation and lifecycle**

- Cancellation before the call does not start a native operation and cancels only the caller terminal.
- Wait cancellation or Future drop after successful submit completes the waiter once; a late completion
  does not complete it again and releases the payload.
- Socket close and context termination complete live waiters with the corresponding lifecycle error and
  leave no detached state or native payload.
