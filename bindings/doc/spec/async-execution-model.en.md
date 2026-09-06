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

The [Core completion pull and ownership contract](../../../core/doc/spec/core/socket/README.en.md#completion-pull-and-ownership)
owns raw C readiness, `zlink_completion_recv()`, and record lifetime.

- **A high-level binding has one completion owner per socket: the participant that reads completions and delivers them to language terminals.**
  This prevents two participants from consuming the same queue. The runtime owns a socket that is
  not registered with a public poller for `PollCompletion`; registration transfers ownership atomically
  to the thread calling `wait()`. Removal or clearing the completion bit returns ownership to the runtime.

High-level `PollCompletion` is a completion progress event returned after the binding takes at least one
record from the native queue and either completes a live waiter or cleans up detached state. The queue
can already be empty when it returns, and it does not guarantee a new public-awaitable state change. It
does not consume application DATA even when `POLLIN` is ready at the same time.

While a public poller is the owner, completion-backed terminals depend on drain by that poller's
`wait()`. Using a blocking request or Go `Submit(context.Context)` at the same time requires another
thread or goroutine to keep executing the `wait()` loop. Do not invoke `wait()` and a blocking terminal
serially on the same execution thread. The binding neither takes ownership for a blocking terminal nor
creates a separate drain thread.

## 5. Joining submit results and completions

- **The binding keeps the context passed to Core valid, completes the language terminal exactly once even when submit results race with completions, and releases any remaining native payload exactly once.**
  A completion can be read before submit returns, so return order must not cause a lost result,
  duplicate completion, or duplicate release. Native context lifetime belongs to
  [Core part send](../../../core/doc/spec/core/socket/README.en.md#part-send-and-pending-admission) and
  the [request contract](../../../core/doc/spec/core/socket/README.en.md#request-and-reply).
  The internal check is that each native payload has exactly one release or transfer to language ownership.

The [common result projection](README.en.md#submit-result-projection) defines how submit results and
wait tokens connect to language results. An early completion does not finish a terminal before it
joins the corresponding submit result. A `wait()` that processes an early completion also does not
return `PollCompletion` progress before that join and settlement or cleanup finish.
Blocking send and reply have no completion to join.

**Language discretion** — Each implementation chooses registration timing and data structures.
Registration before submit returns, serialization of drain with registration, and retention of early
records are equivalent only when they preserve the completion and lifetime results above.
Check equivalence through the terminal results, errors, completion counts, and post-cancellation
observations in the [verification requirements](#7-implementation-and-contract-test-verification-requirements).

## 6. Caller wait cancellation

High-level cancellation cancels the caller's wait on a language terminal, not the Core operation.

- If cancellation is decided before the native call, the binding neither calls Core nor leaves
  operation state.
- If cancellation races with submit failure during the native call, the binding records only the
  cancellation claim until native returns. An exact submit error takes precedence when submit fails.
- If cancellation or Future drop is decided first after successful submit, the binding completes only
  the live waiter once as canceled or detached. Operation state remains until a late completion or
  socket/context lifecycle cleanup.
- A late completion releases the native payload and state but does not complete the public waiter again.
  A successful ID `0` send whose cancellation was decided first is also not completed again as success.
- If socket close or context termination makes completion unavailable, the binding completes only the
  live waiter with the shutdown or terminated error and removes all operation state.

- **Transfer only a successful REQUEST reply into a language message collection; on conversion failure, release both wrappers already created and remaining native parts.**
  Partial conversion must not leave a payload without an owner or with two owners.
  Native-array cleanup follows [Core completion ownership](../../../core/doc/spec/core/socket/README.en.md#completion-pull-and-ownership).

For a non-OK request completion, the high-level binding exposes only a typed request error. It does not
move the error-reply payload into a language message collection or error property, and closes the native
completion exactly once before completing the user-visible error. In Go, a typed request error is
`(nil, error)`; when Context cancellation is decided first, it is `(nil, ctx.Err())`.

## 7. Implementation and contract-test verification requirements

Verify the following using only public terminals, public poller events, and language-specific
cancellation results. Each item maps to one contract test.

**Completion and drain**

Raw C completion observations follow the
[Core verification requirements](../../../core/doc/spec/core/socket/README.en.md#8-implementation-and-contract-test-verification-requirements).

- Observing high-level `PollCompletion` does not require a new public-awaitable state change;
  DATA ready at the same time remains available to the subsequent application receive.
- On a socket registered for completion with a public poller, blocking requests and Go
  `Submit(context.Context)` can receive completion while another thread or goroutine executes `wait()`.

**Submit and completion races**

- Submitting through a high-level terminal with the same input as a native submission that fails
  without a wait token yields that submit error once.
- An immediately admitted send's terminal succeeds once without waiting for a completion record.
- When several requests receive immediate replies, each terminal returns its corresponding reply's
  part order and bytes exactly once.

**Cancellation and lifecycle**

- A send/request canceled before invocation returns cancellation to the caller terminal and sends
  no corresponding message to the peer.
- If caller wait cancellation wins after successful submit, a later reply neither changes the caller's
  cancellation result to success nor completes the terminal again.
- Socket close and context termination finish live waiters with the corresponding lifecycle error.
- A non-OK request result is observed as a typed request error without the error-reply payload as a
  success value or error property. The Go result is `(nil, error)`.
