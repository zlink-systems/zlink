# 5. Execution Model

> [Common contract table of contents](README.en.md)
>
> The terminator and Spot execution context combination is owned by
> [12 HTTP Client §3](12-http-client.en.md) and
> [04 Async Execution Policy §1.1](../05-async-execution-policy.en.md).
> §5.1 below is a summary to understand the call form. From §5.2, this
> document defines the non-blocking basis of HTTP transport and the
> cancellation and timeout boundary.

## 5.1 The Two Execution Modes And Callback

The HTTP client provides one-way submission and response completion.
The exact terminator name is `.NET`'s `Async`, Kotlin wrapper's
`await`, Java/C++'s `submit`. Node distinguishes raw response
`submitRaw`, typed response/callback `async`, and one-way `submit`.
`Yield`, which returns the shared Spot gate, is provided to the server
HTTP request builder, server request, and Worker call running in an
execution context where returning the gate is allowed — namely,
`SpotWide` User Spot and Instance Spot. A standalone client has no gate
to return, so it isn't provided.

| Execution Mode | What It Waits For | [Spot](../01-glossary.en.md#spot) Execution Queue |
| --- | --- | --- |
| **one-way submission** | Waits until the HTTP request is submitted at the transport boundary | Keeps the current turn. No normal completion value |
| **response completion** | Waits until the HTTP response arrives | Keeps the current turn |

**Callback is a separate completion path.** Used by a caller that
doesn't use an awaitable, and the completion callback enters as a
**new turn** of the Spot execution queue
([framework 12 §3](12-http-client.en.md)).

Submit returns the language's standard async value, and doesn't
occupy the caller's thread/event loop while waiting on the network.

| Language | Async Return Type | Non-Blocking Basis |
| --- | --- | --- |
| cpp | `task_t<T>` (`co_await`) | Offloaded to the execute scheduler when `.coroutines()` is active |
| dotnet | `ValueTask<T>` | `SocketsHttpHandler` epoll/IOCP |
| java | `CompletionStage<T>` | `java.net.http` NIO selector |
| kotlin | `suspend` function | java runtime + `CompletionStage.await()` bridge |
| node | `Promise<T>` | undici libuv |

**The terminator name follows framework convention.** `.NET` uses
`Async(...)`, Kotlin wrapper uses `await(...)`, Java/C++ use
`submit(...)`. Node's HTTP typed response and callback keep `async(...)`
to avoid a TypeScript inheritance signature conflict, and raw response
uses `submitRaw()` ([04 §2](../05-async-execution-policy.en.md)).

## 5.2 External HTTP Wait And The Spot Execution Queue

If another work item and timer of the same Spot must progress while
waiting for an external API, the gate must be returned. The server HTTP
request builder directly provides `Yield` in that context.

```csharp
var profile = await http.Get($"/players/{id}").Yield<Profile>(ct);
```

If other work must also proceed while waiting for the response, wrap it
in an I/O Worker and finish with the Worker call's `Yield`. The example
below is that form.

```csharp
var profile = await Context
    .RunIoWorker(async workerCancellation =>
        await http.Get($"/players/{id}").Fetch<Profile>(workerCancellation))
    .Yield(ct);
```

- **Don't assume Spot state stays the same across a Worker `Yield`.**
  Another callback can change state while waiting.
- **The response completion terminator keeps the turn.** If Spot state
  must be handled continuously before and after the async wait, use
  this terminator.

## 5.3 The Turn Seam — Injecting An Execution Scheduler

**The HTTP client doesn't know framework.** The only thing that knows a
Spot's turn is the **injected execution scheduler**. The binary
dependency keeps the one-way direction `framework → HTTP client`.

- The HTTP client puts an **execution scheduler injection point** as a
  public contract. The scheduler decides where to resume completion.
- **The framework wires in a callback completion scheduler at DI
  registration.** The callback enters as a new turn of the Spot
  execution queue.
- **A standalone-use HTTP request builder doesn't expose `Yield`.**
  There's no Spot gate to return.

cpp's `coroutines(resume_scheduler)` / `framework_resume_scheduler_t`
is the precedent for this seam.

- `coroutines()` — uses the default scheduler.
- `coroutines(resume_scheduler)` — injects the resume location
  (resumes the continuation on the framework execution queue).
- `coroutines(execute_scheduler, resume_scheduler)` — injects both
  execute and resume.

Caution (a current implementation characteristic, not a contract): the
cpp default scheduler uses a single thread shared for execute/resume,
so requests are serialized, and blocking-waiting for a different task
on the same scheduler from a resumed continuation can deadlock.

## 5.4 Server Runtime Doesn't Put A Blocking Terminator

`.NET`, Java, Kotlin, and Node's `Fetch` family directly return the
decoded body but complete asynchronously. Don't wait occupying the
current thread just because the name is `Fetch`.

- Prohibited: an API that waits for an async result on the current
  thread, such as `.result()`, `.join()`, `.get()`.
- If a synchronous wait is needed in a test or CLI, **the caller**
  wraps it with a language idiom (`GetAwaiter().GetResult()`,
  `runBlocking`, `.join()`).
- Composition uses `co_await` / `await` / `thenCompose` / suspend.

C++'s `fetch<T>()` is a separate convenience API for a blocking client
scenario. It isn't used in a Framework server handler.

## 5.5 Server Surface And Client Lifetime

**The client used in a server (Spot handler/channel handler) is
injected through DI.** Don't build a client with a static factory
inside a handler — it loses the connection pool and turn seam.

| Surface | Who Uses It | Terminator |
|------|-----------|------------|
| Static factory | CLI · client scenario | response completion / callback |
| **DI-injected client** | **Spot handler · server code** | one-way / response completion / callback |

- Build one client per service and reuse it (pool/keep-alive benefit).
- The builder verb shorthand (one-shot) is a **convenience path** that
  lazy-builds the client at submit and closes it after completion.
  Since it pays the transport stack initialization cost per request,
  don't use it for repeated/high-load calls. A one-shot request object
  can't be resubmitted (resubmission is `InvalidOperation`).

## 5.6 Cancellation

- dotnet takes a `CancellationToken` on submit.
- Propagation of kotlin coroutine cancellation to the underlying
  request isn't currently implemented (revision candidate
  [R5](10-revision-candidates.en.md)).
- cpp/java/node don't expose a per-request cancellation API (bounded
  only by timeout).
