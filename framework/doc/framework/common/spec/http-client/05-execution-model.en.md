# 5. Execution Model

> [Common contract table of contents](README.en.md)
>
> The terminator and Spot execution context combination is owned by
> [12 HTTP Client §3](12-http-client.en.md) and
> [04 Async Execution Policy §1.1](../server/01-execution/README.en.md).
> §5.1 below is a summary to understand the call form. From §5.2, this
> document defines the non-blocking basis of HTTP transport and the
> cancellation and timeout boundary.

## 5.1 Execution Modes And Callback

The HTTP terminator surface follows
[Language interfaces §1.4](language-interfaces.en.md#14-terminators), and the
completion meaning of each execution mode (response completion, `Yield`) is
owned by [12 §3](12-http-client.en.md#3-execution-terminator--response-completion--callback).
The eligible contexts and gate-return meaning of `Yield` are decided by the
framework's
[Submit and completion §2](../server/01-execution/01-submit-and-completion.en.md#2-completion-meaning-per-terminator-and-per-language-names),
and the two forms used by HTTP are explained in
[§5.2](#52-external-http-wait-and-the-spot-execution-queue).

**Callback is a separate completion path.** Used by a caller that
doesn't use an awaitable, and the completion callback enters as a
**new turn** of the Spot execution queue
([framework 12 §3](12-http-client.en.md)).

Submit returns the language's standard async value, and doesn't
occupy the caller's thread/event loop while waiting on the network.

| Language | Async Return Type | Non-Blocking Basis |
| --- | --- | --- |
| cpp | `task_t<T>` (`co_await`) | Offloaded to the execute scheduler (the default one, or the one injected with `.coroutines(...)`) |
| dotnet | `ValueTask<T>` | `SocketsHttpHandler` epoll/IOCP |
| java | `CompletionStage<T>` | `java.net.http` NIO selector |
| kotlin | `suspend` function | java runtime + `CompletionStage.await()` bridge |
| node | `Promise<T>` | undici libuv |

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

[01 Scope And Architecture §1.3](01-scope-and-architecture.en.md#13-relationship-with-framework--one-way-dependency) owns the HTTP client's Framework contract dependency and deliverable boundary.
Spot turn integration and completion scheduler injection follow
[HTTP Client §3.2](12-http-client.en.md#32-the-turn-seam--a-single-injection-point).

- The HTTP client puts an **execution scheduler injection point** as a
  public contract. The scheduler decides where to resume completion.
- **The framework injects the callback completion scheduler at DI
  registration.** The callback enters as a new turn of the Spot
  execution queue.

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

## 5.4 A Runtime Execution Context Rejects Blocking Terminator Calls

The `Fetch` family directly returns the decoded body but completes
asynchronously in all five languages. The name `Fetch` does not permit a wait that occupies the current thread.

- Prohibited: an API that waits for an async result on the current
  thread, such as `.result()`, `.join()`, `.get()`.
- If a synchronous wait is needed in a test or CLI, **the caller**
  wraps it with a language idiom (`GetAwaiter().GetResult()`,
  `runBlocking`, `.join()`).
- Composition uses `co_await` / `await` / `thenCompose` / suspend.

The C++ blocking terminators (named in
[Language interfaces §1.4](language-interfaces.en.md#14-terminators)) are for CLI and
client scenarios only. In a runtime execution context they fail immediately with
`InvalidOperation`, per
[Submit and completion §2](../server/01-execution/01-submit-and-completion.en.md#2-completion-meaning-per-terminator-and-per-language-names).

## 5.5 Server Surface And Client Lifetime

**The client used in a server (Spot handler/channel handler) is
injected through DI.** Don't build a client with a static factory
inside a handler — it loses the connection pool and turn seam.

| Surface | Who Uses It | Terminator |
|------|-----------|------------|
| Static factory | CLI · client scenario | response completion / callback |
| **DI-injected client** | **Spot handler · server code** | response completion / callback / `Yield` |

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
